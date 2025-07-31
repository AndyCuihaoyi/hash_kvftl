#include "dftl_cache.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "bm.h"
#include "demand.h"
#include "request.h"
#include "../lower/lower.h"
#include "dftl_pg.h"
#include "../tools/valueset.h"
#include "glib-2.0/glib.h"
#include "write_buffer.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef CMT_USE_NUMA
#include <numa.h>
#endif // CMT_USE_NUMA

extern block_mgr_t bm;
extern block_mgr_t *pbm;

demand_cache d_cache = {
	.create = dftl_cache_create,
	.destroy = dftl_cache_destroy,
	.load = dftl_cache_load,
	.list_up = dftl_cache_list_up,
	.wait_if_flying = dftl_cache_wait_if_flying,
	.touch = dftl_cache_touch,
	.update = dftl_cache_update,
	.get_pte = dftl_cache_get_pte,
	.get_cmt = dftl_cache_get_cmt,
	.is_hit = dftl_cache_is_hit,
	.is_full = dftl_cache_is_full,
};

demand_cache *pd_cache = &d_cache;

static void cache_env_init(struct cache_env *env)
{
	env->nr_tpages_optimal_caching = _NOP_NO_OP * 4 / PAGESIZE;
	// env->nr_valid_tpages = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3;	// 0.75 is for hash table load factor
	env->nr_valid_tpages = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE;
	env->nr_valid_tentries = env->nr_valid_tpages * EPP;
	// env->max_cache_entry = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3; 	// number of tpages
	env->max_cache_entry = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE;
	env->max_cached_tpages = _NOP_NO_OP / 1024;
	// env->max_cached_tpages = _NOP_NO_OP;
	env->max_cached_tentries = env->max_cached_tpages * EPP;
}

static void cache_member_init(struct cache_member *member)
{
	struct cmt_struct **cmt = g_malloc0(d_cache.env.nr_valid_tpages * sizeof(struct cmt_struct *));
	for (int i = 0; i < d_cache.env.nr_valid_tpages; i++)
	{
		cmt[i] = g_malloc0(sizeof(struct cmt_struct));
		cmt[i]->idx = i;
		cmt[i]->pt = NULL;
		cmt[i]->t_ppa = UINT32_MAX;
		cmt[i]->state = CLEAN;
		cmt[i]->is_flying = false;
		cmt[i]->lru_ptr = NULL;
		cmt[i]->is_cached = NULL;
		cmt[i]->cached_cnt = 0;
		cmt[i]->dirty_cnt = 0;

		cmt[i]->retry_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
		cmt[i]->wait_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
	}
	member->cmt = cmt;

	member->mem_table = g_malloc0(d_cache.env.nr_valid_tpages * sizeof(struct pt_struct *));
	for (int i = 0; i < d_cache.env.nr_valid_tpages; i++)
	{
#ifdef CMT_USE_NUMA
		member->mem_table[i] = numa_alloc_onnode(EPP * sizeof(struct pt_struct), 1);
		memset(member->mem_table[i], 0, EPP * sizeof(struct pt_struct));
#else
		member->mem_table[i] = g_malloc0(EPP * sizeof(struct pt_struct));
		mlock(member->mem_table[i], EPP * sizeof(struct pt_struct));
#endif // CMT_USE_NUMA
		for (int j = 0; j < EPP; j++)
		{
			member->mem_table[i][j].ppa = UINT32_MAX;
#ifdef STORE_KEY_FP
			member->mem_table[i][j].key_fp = 0;
#endif
		}
	}
	lru_init(&(member->lru));
	member->nr_cached_tpages = 0;
	member->nr_cached_tentries = 0;
}

static void cache_stat_init(struct cache_stat *stat)
{
	stat->cache_hit = 0;
	stat->cache_miss = 0;
	stat->clean_evict = 0;
	stat->dirty_evict = 0;
	stat->blocked_miss = 0;
	stat->cache_miss_by_collision = 0;
	stat->cache_hit_by_collision = 0;
	stat->cache_load = 0;
}

int dftl_cache_create(demand_cache *d_cache)
{
	cache_env_init(&(d_cache->env));
	cache_member_init(&(d_cache->member));
	cache_stat_init(&(d_cache->stat));
	return 0;
}

static void cache_member_free(demand_cache *self, struct cache_member *member)
{
	for (int i = 0; i < self->env.nr_valid_tpages; i++)
	{
		free(member->cmt[i]);
	}
	free(member->cmt);

	for (int i = 0; i < self->env.nr_valid_tpages; i++)
	{
#ifdef CMT_USE_NUMA
		numa_free(member->mem_table[i], EPP * sizeof(struct pt_struct));
#else
		free(member->mem_table[i]);
#endif // CMT_USE_NUMA
	}

	lru_free(member->lru);
}

int dftl_cache_destroy(demand_cache *self)
{
	cache_member_free(self, &(self->member));
	return 0;
}

int dftl_cache_load(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	struct inflight_params *i_params;

	if (IS_INITIAL_PPA(cmt->t_ppa))
	{
		return 0;
	}

	i_params = get_iparams(req, wb_entry);
	i_params->jump = GOTO_LIST;

	self->stat.cache_load++;
	uint64_t lat = __demand.li->read(cmt->t_ppa, PAGESIZE, 0);
	if (req)
	{
		req->etime = clock_get_ns() + lat;
	}
	else if (wb_entry)
	{
		wb_entry->etime = clock_get_ns() + lat;
	}
	else
	{
		abort();
	}
	cmt->is_flying = true;
	return 1;
}

int dftl_cache_list_up(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	int rc = 0;

	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	struct cmt_struct *victim = NULL;
	algorithm *palgo = self->env.palgo;
	w_buffer_t *pw_buffer = D_ENV(palgo)->pw_buffer;

	struct inflight_params *i_params;

	if (self->is_full(self))
	{
		victim = (struct cmt_struct *)lru_pop(self->member.lru);
		self->member.nr_cached_tpages--;

		victim->lru_ptr = NULL;
		victim->pt = NULL;

		if (victim->state == DIRTY)
		{
			self->stat.dirty_evict++;

			i_params = get_iparams(req, wb_entry);
			i_params->jump = GOTO_COMPLETE;
			// i_params->pte = cmbr->mem_table[D_IDX][P_IDX];

			victim->t_ppa = tp_alloc(pbm);
			pbm->validate_page(pbm, victim->t_ppa);
			victim->state = CLEAN;

			// struct pt_struct pte = cmbr->mem_table[D_IDX][P_IDX];

			uint64_t lat = __demand.li->write(victim->t_ppa, PAGESIZE, 0);
			if (req)
			{
				req->etime = clock_get_ns() + lat;
			}
			else if (wb_entry)
			{
				wb_entry->etime = clock_get_ns() + lat;
			}
			else
			{
				abort();
			}

			bm_oob_t new_oob = {
				.is_tpage = true,
				.lpa = victim->idx,
				.length = PAGESIZE};
			pbm->set_oob(pbm, victim->t_ppa * GRAIN_PER_PAGE, &new_oob);

			rc = 1;
		}
		else
		{
			self->stat.clean_evict++;
		}
	}

	cmt->pt = self->member.mem_table[D_IDX];
	cmt->lru_ptr = lru_push(self->member.lru, (void *)cmt);
	self->member.nr_cached_tpages++;

	if (cmt->is_flying)
	{
		cmt->is_flying = false;

		if (req)
		{
			request *retry_req;
			while (ring_dequeue(cmt->retry_q, (void *)&retry_req, 1))
			{
				struct inflight_params *i_params = get_iparams(retry_req, NULL);
				i_params->jump = GOTO_COMPLETE;
				algo_q_insert_sorted(palgo->retry_q, retry_req, NULL);
			}
		}
		else if (wb_entry)
		{
			snode *retry_wbe;
			while (ring_dequeue(cmt->retry_q, (void *)&retry_wbe, 1))
			{
				// lpa_t retry_lpa = get_lpa(retry_wbe->key, retry_wbe->hash_params);

				struct inflight_params *i_params = get_iparams(NULL, retry_wbe);
				i_params->jump = GOTO_COMPLETE;
				// i_params->pte = cmt->pt[OFFSET(retry_lpa)];
				algo_q_insert_sorted(pw_buffer->env->wb_retry_q, NULL, retry_wbe);
			}
		}
	}

	return rc;
}

int dftl_cache_wait_if_flying(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	if (cmt->is_flying)
	{
		self->stat.blocked_miss++;

		if (req)
			while (!ring_enqueue(cmt->retry_q, (void *)&req, 1))
				;
		else if (wb_entry)
			while (!ring_enqueue(cmt->retry_q, (void *)&wb_entry, 1))
				;
		else
			abort();

		return 1;
	}
	return 0;
}

int dftl_cache_touch(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	lru_update(self->member.lru, cmt->lru_ptr);
	return 0;
}

int dftl_cache_update(demand_cache *self, lpa_t lpa, struct pt_struct pte)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];

	if (cmt->pt)
	{
		cmt->pt[P_IDX] = pte;

		if (!IS_INITIAL_PPA(cmt->t_ppa) && cmt->state == CLEAN)
		{
			pbm->invalidate_page(pbm, cmt->t_ppa);
			cmt->t_ppa = UINT32_MAX;
		}
		cmt->state = DIRTY;
		lru_update(self->member.lru, cmt->lru_ptr);
	}
	else
	{
		/* FIXME: to handle later update after evict */
		self->member.mem_table[D_IDX][P_IDX] = pte;
	}
	return 0;
}

struct pt_struct dftl_cache_get_pte(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	if (cmt->pt)
	{
		return cmt->pt[P_IDX];
	}
	else
	{
		/* FIXME: to handle later update after evict */
		return self->member.mem_table[D_IDX][P_IDX];
	}
}

struct cmt_struct *dftl_cache_get_cmt(demand_cache *self, lpa_t lpa)
{
	return self->member.cmt[D_IDX];
}

bool dftl_cache_is_hit(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cmt[D_IDX];
	if (cmt->pt != NULL)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

bool dftl_cache_is_full(demand_cache *self)
{
	return (self->member.nr_cached_tpages >= self->env.max_cached_tpages);
}