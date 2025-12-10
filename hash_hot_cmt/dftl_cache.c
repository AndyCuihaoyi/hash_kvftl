#include "dftl_cache.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "bm.h"
#include "demand.h"
#include "request.h"
#include "../lower/lower.h"
#include "../tools/bloomfilter.h"
#include "dftl_pg.h"
#include "../tools/valueset.h"
#include "../tools/fifo_queue.h"
#include "glib-2.0/glib.h"
#include "write_buffer.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef CMT_USE_NUMA
#include <numa.h>
#endif // CMT_USE_NUMA
#ifdef HOT_CMT
#define T (2) // hot threshold
#define HOT_CMT_FAC (0.06)
#define MAX_PROBE (80)
#endif
extern block_mgr_t bm;
extern block_mgr_t *pbm;
#ifdef HOT_CMT
int dftl_cache_promote_hot(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim);
bool dftl_cache_hot_is_hit(demand_cache *self, lpa_t lpa, h_pte_t **hot_pte);
#endif

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
#ifdef HOT_CMT
	.promote_hot = dftl_cache_promote_hot,
	.hot_is_hit = dftl_cache_hot_is_hit,
	.hot_cmt_reset = dftl_cache_hot_reset,
#endif
};

demand_cache *pd_cache = &d_cache;
static void cache_env_init(struct cache_env *env)
{
	/* hash table cache */
	env->nr_tpages_optimal_caching = _NOP_NO_OP * 4 / PAGESIZE;
	// num of mapping page div hash factor(0.75)
	env->nr_valid_tpages = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3; // 0.75 is for hash table load factor
	env->nr_valid_tentries = env->nr_valid_tpages * EPP;
	env->max_cache_entry = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3; // number of tpages
#ifdef HOT_CMT
	env->max_cached_tpages = ceil(_NOP_NO_OP / 1024 * (1 - HOT_CMT_FAC));
	env->max_cached_hot_tpages = ceil(_NOP_NO_OP / 1024 * HOT_CMT_FAC);
	env->max_cached_hot_entries = env->max_cached_hot_tpages * EPP;
#else
	env->max_cached_tpages = _NOP_NO_OP / 1024;
#endif
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
#ifdef HOT_CMT
		cmt[i]->hit_cnt = 0;
		cmt[i]->valid_cnt = 0;
		cmt[i]->multi_hit_cnt = 0;
#endif
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
#ifdef HOT_CMT
	struct cmt_struct **hot_cmt = g_malloc0(d_cache.env.max_cached_hot_tpages * sizeof(struct cmt_struct *));
	for (int i = 0; i < d_cache.env.max_cached_hot_tpages; i++)
	{
		hot_cmt[i] = g_malloc0(sizeof(struct cmt_struct));
		hot_cmt[i]->idx = i + d_cache.env.nr_valid_tpages;
		hot_cmt[i]->pt = NULL;
		hot_cmt[i]->t_ppa = UINT32_MAX;
		hot_cmt[i]->state = CLEAN;
		hot_cmt[i]->is_flying = false;
		hot_cmt[i]->lru_ptr = NULL;
		hot_cmt[i]->is_cached = NULL;
		hot_cmt[i]->cached_cnt = 0;
		hot_cmt[i]->dirty_cnt = 0;

		hot_cmt[i]->retry_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
		hot_cmt[i]->wait_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
	}
	member->hot_cmt = hot_cmt;
	member->hot_mem_table = g_malloc0(d_cache.env.max_cached_hot_tpages * sizeof(struct hot_pt_struct *));
	for (int i = 0; i < d_cache.env.max_cached_hot_tpages; i++)
	{

#ifdef CMT_USE_NUMA
		member->hot_mem_table[i] = numa_alloc_onnode(EPP * sizeof(struct pt_struct), 1);
		memset(member->hot_mem_table[i], 0, EPP * sizeof(struct pt_struct));
#else
		member->hot_mem_table[i] = g_malloc0(EPP * sizeof(struct hot_pt_struct));
		mlock(member->hot_mem_table[i], EPP * sizeof(struct hot_pt_struct));
#endif // CMT_USE_NUMA
		for (int j = 0; j < EPP; j++)
		{
			member->hot_mem_table[i][j].lpa = UINT32_MAX;
			member->hot_mem_table[i][j].ppa = UINT32_MAX;
#ifdef STORE_KEY_FP
			member->hot_mem_table[i][j].key_fp = 0;
#endif
		}
	}
	member->nr_cached_hot_tpages = 0;
	member->nr_cached_hot_tentries = 0;
#endif
#ifdef PREFILL_CACHE
	QInit(&(member->prefill_q), MAX_WRITE_BUF);
#endif
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
#ifdef HOT_CMT
	stat->hot_cmt_hit = 0;
	stat->hot_rewrite_entries = 0;
	stat->up_grain_cnt = 0;
	stat->up_hit_cnt = 0;
	stat->up_page_cnt = 0;
	stat->grain_heat_distribute = g_malloc0(sizeof(uint32_t) * (1000));
#endif
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
#ifdef HOT_CMT
	for (int i = 0; i < self->env.max_cached_hot_tpages; i++)
	{
		free(member->hot_cmt[i]);
	}
	free(member->hot_cmt);
	for (int i = 0; i < self->env.max_cached_hot_tpages; i++)
	{
#ifdef CMT_USE_NUMA
		numa_free(member->hot_mem_table[i], EPP * sizeof(struct pt_struct));
#else
		free(member->hot_mem_table[i]);
#endif // CMT_USE_NUMA
	}
#endif
}

int dftl_cache_destroy(demand_cache *self)
{
	cache_member_free(self, &(self->member));
	return 0;
}

#ifdef HOT_CMT
int dftl_cache_hot_reset(demand_cache *d_cache)
{
	for (int i = 0; i < d_cache->env.max_cached_hot_tpages; i++)
	{
		for (int j = 0; j < EPP; j++)
		{
			d_cache->member.hot_mem_table[i][j].lpa = UINT32_MAX;
			d_cache->member.hot_mem_table[i][j].ppa = UINT32_MAX;
#ifdef STORE_KEY_FP
			d_cache->member.hot_mem_table[i][j].key_fp = 0;
#endif
#ifdef VERIFY_CACHE
			d_cache->member.hot_mem_table[i][j].real_key.len = 0;
			d_cache->member.hot_mem_table[i][j].real_key.key[0] = 0;
#endif
		}
	}
	return 0;
}

int dftl_cache_promote_hot(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim)
{
	for (int i = 0; i < EPP; i++)
	{
		/*direct mapping may cause rewrite and collision*/
		if (victim->pt[i].ppa != UINT32_MAX && victim->cnt_map[i] >= T)
		{
			lpa_t lpa = IDX_TO_LPA(victim->idx, i);
			lpa_t new_lpa = lpa % (self->env.max_cached_hot_entries);
			struct cmt_struct *cmt = self->member.hot_cmt[D_IDX_HOT];
			self->stat.up_grain_cnt++;
			self->stat.up_hit_cnt += victim->cnt_map[i];
			int d_idx = D_IDX_HOT;
			int p_idx = P_IDX_HOT;
			int probe = 1;
			bool found = false;

			while (self->member.hot_mem_table[d_idx][p_idx].lpa != UINT32_MAX && probe < MAX_PROBE)
			{
				// linear
				// p_idx = (p_idx + 1);
				// quadratic
				p_idx = (p_idx + probe * probe);
				probe++;
				if (p_idx > EPP)
				{
					d_idx = (d_idx + p_idx / EPP) % self->env.max_cached_hot_tpages;
					p_idx = p_idx % EPP;
				}
			}

			if (self->member.hot_mem_table[d_idx][p_idx].lpa == UINT32_MAX)
			{
				self->stat.hot_valid_entries++;
				found = true;
			}
			else
			{
				self->stat.hot_rewrite_entries++;
				d_idx = D_IDX_HOT;
				p_idx = P_IDX_HOT;
			}
			self->member.hot_mem_table[d_idx][p_idx].lpa = lpa;
			self->member.hot_mem_table[d_idx][p_idx].ppa = victim->pt[i].ppa;
			self->member.hot_mem_table[d_idx][p_idx].key_fp = victim->pt[i].key_fp;
// reset real key
#ifdef VERIFY_CACHE
			self->member.hot_mem_table[d_idx][p_idx].real_key.len = 0;
			self->member.hot_mem_table[d_idx][p_idx].real_key.key[0] = 0;
#endif
		}
		self->stat.grain_heat_distribute[victim->cnt_map[i]]++;
	}
	self->stat.up_page_cnt++;
	return 1;
}

bool dftl_cache_hot_is_hit(demand_cache *self, lpa_t lpa, h_pte_t **hot_pte)
{
	lpa_t new_lpa = lpa % (self->env.max_cached_hot_entries);
	int d_idx = D_IDX_HOT;
	int p_idx = P_IDX_HOT;
	int probe = 1;
	while (probe < MAX_PROBE)
	{
		if (self->member.hot_mem_table[d_idx][p_idx].lpa == lpa)
		{
			*hot_pte = &self->member.hot_mem_table[d_idx][p_idx];
			return true;
		}
		else if (self->member.hot_mem_table[d_idx][p_idx].lpa == UINT32_MAX)
		{
			return false;
		}
		// linear
		// p_idx = p_idx + 1;
		// quadratic
		p_idx = (p_idx + probe * probe);
		probe++;
		if (p_idx > EPP)
		{
			d_idx = (d_idx + p_idx / EPP) % self->env.max_cached_hot_tpages;
			p_idx = p_idx % EPP;
		}
	}
	return false;
}

#define W_CONCENTRATION 0.9
#define W_ACTIVITY 0.05
#define W_DENSITY 0.05
#define PROMOTION_SCORE_THRESHOLD 0.6

bool hotness_score(struct cmt_struct *victim)
{
	double concentration_ratio = (double)victim->multi_hit_cnt / victim->hit_cnt;
	if (victim->multi_hit_cnt == 0)
		return false;
	double activity_score = log1p((double)victim->hit_cnt);
	double density_ratio = (double)victim->valid_cnt / EPP;
	double hotness_score = (W_CONCENTRATION * concentration_ratio) +
						   (W_ACTIVITY * activity_score) +
						   (W_DENSITY * density_ratio);
	if (hotness_score < PROMOTION_SCORE_THRESHOLD)
	{
		return false;
	}
	return true;
}
#endif

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
		// victim = (struct cmt_struct *)lru_pop_top(self->member.lru);
#ifdef HOT_CMT
		if (!victim->cnt_map)
			abort();
		// if (victim->hit_cnt > 0 && hotness_score(victim))
		if (victim->hit_cnt >= T)
		{
			self->promote_hot(self, UINT32_MAX, NULL, NULL, victim);
		}
		victim->hit_cnt = 0;
		memset(victim->cnt_map, 0, sizeof(uint8_t) * EPP);
#endif
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
#ifdef HOT_CMT
	if (!cmt->cnt_map)
		cmt->cnt_map = (uint8_t *)g_malloc0(sizeof(uint8_t) * EPP);
	cmt->hit_cnt = 0;
	cmt->multi_hit_cnt = 0;
#endif
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
#ifdef HOT_CMT
		if (cmt->cnt_map[P_IDX]++ > 1)
			cmt->multi_hit_cnt++;
		cmt->hit_cnt++;
#endif
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