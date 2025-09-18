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
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef CMT_USE_NUMA
#include <numa.h>
#endif // CMT_USE_NUMA
extern block_mgr_t bm;
extern block_mgr_t *pbm;
#define T (3) // hot threshold
#define HOT_CMT_FAC (0.03)
#define MAX_PROBE (20)
// #define BLOOM_FILTER
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
	.upgrade_hot = dftl_cache_upgrade_hot,
	.hot_evict = dftl_cache_hot_evict,
	.reset = dftl_cache_reset,
	.hot_is_hit = dftl_cache_hot_is_hit};
demand_cache *pd_cache = &d_cache;
static void cache_env_init(struct cache_env *env)
{
	/* hash table cache */
	env->nr_tpages_optimal_caching = _NOP_NO_OP * 4 / PAGESIZE;
	// num of mapping page div hash factor(0.75)
	env->nr_valid_tpages = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3; // 0.75 is for hash table load factor
	env->nr_valid_tentries = env->nr_valid_tpages * EPP;
	env->max_cache_entry = (_NOP_NO_OP / EPP + ((_NOP_NO_OP % EPP) ? 1 : 0)) * GRAIN_PER_PAGE * 4 / 3; // number of tpages
	env->max_cached_tpages = ceil(_NOP_NO_OP / 1024 * (1 - HOT_CMT_FAC));
	env->max_cached_hot_tpages = ceil(_NOP_NO_OP / 1024 * HOT_CMT_FAC);
	env->nr_valid_hot_tpages = env->max_cached_hot_tpages;
	// env->nr_valid_hot_tpages = env->max_cached_hot_tpages * 2;
	env->max_cached_tentries = env->max_cached_tpages * EPP;
}

static void cache_member_init(struct cache_member *member)
{
	/*init cold cmt pages*/
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
		cmt[i]->is_cached = false;
		cmt[i]->cached_cnt = 0;
		cmt[i]->dirty_cnt = 0;
		cmt[i]->heat_cnt = 0;
		cmt[i]->retry_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
		cmt[i]->wait_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
	}
	member->cold_cmt = cmt;
	/*init hot mapping pages*/
	struct cmt_struct **hot_cmt = g_malloc0(d_cache.env.nr_valid_hot_tpages * sizeof(struct cmt_struct *));
	for (int i = 0; i < d_cache.env.nr_valid_hot_tpages; i++)
	{
		hot_cmt[i] = g_malloc0(sizeof(struct cmt_struct));
		hot_cmt[i]->idx = i + d_cache.env.nr_valid_tpages;
		hot_cmt[i]->pt = NULL;
		hot_cmt[i]->t_ppa = UINT32_MAX;
		hot_cmt[i]->state = CLEAN;
		hot_cmt[i]->is_flying = false;
		hot_cmt[i]->lru_ptr = NULL;
		hot_cmt[i]->is_cached = false;
		hot_cmt[i]->cached_cnt = 0;
		hot_cmt[i]->dirty_cnt = 0;
		hot_cmt[i]->heat_cnt = 0;
		hot_cmt[i]->retry_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
		hot_cmt[i]->wait_q = ring_create(RING_TYPE_MP_SC, MAX_WRITE_BUF);
	}
	member->hot_cmt = hot_cmt;
#ifdef BLOOM_FILTER
	member->hot_bf = bf_init(EPP * d_cache.env.max_cached_hot_tpages, bf_fpr_from_memory(EPP * d_cache.env.max_cached_hot_tpages, 4 * 1024 * 1024 * (HOT_CMT_FAC / 0.1)));
#endif
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
	member->hot_mem_table = g_malloc0(d_cache.env.nr_valid_hot_tpages * sizeof(struct pt_struct *));
	for (int i = 0; i < d_cache.env.nr_valid_hot_tpages; i++)
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
	lru_init(&(member->cold_lru));
	lru_init(&(member->hot_lru));
	member->nr_cached_tpages = 0;
	member->nr_cached_hot_tpages = 0;
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
	stat->hot_cmt_evict = 0;
	stat->hot_cmt_hit = 0;
	stat->hot_valid_entries = 0;
	stat->hot_rewrite_grain = 0;
	stat->hot_miss = 0;
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
		free(member->cold_cmt[i]);
	}
	free(member->cold_cmt);

	for (int i = 0; i < self->env.nr_valid_tpages; i++)
	{
#ifdef CMT_USE_NUMA
		numa_free(member->mem_table[i], EPP * sizeof(struct pt_struct));
#else
		free(member->mem_table[i]);
#endif // CMT_USE_NUMA
	}
	for (int i = 0; i < self->env.nr_valid_hot_tpages; i++)
	{
		free(member->hot_cmt[i]);
	}
	free(member->hot_cmt);
	for (int i = 0; i < self->env.nr_valid_hot_tpages; i++)
	{
#ifdef CMT_USE_NUMA
		numa_free(member->hot_mem_table[i], EPP * sizeof(struct pt_struct));
#else
		free(member->hot_mem_table[i]);
#endif // CMT_USE_NUMA
	}
	lru_free(member->cold_lru);
	lru_free(member->hot_lru);
}

int dftl_cache_destroy(demand_cache *self)
{
	cache_member_free(self, &(self->member));
	return 0;
}

int dftl_cache_reset(demand_cache *d_cache)
{
	d_cache->stat.hot_valid_entries = 0;
	d_cache->stat.hot_rewrite_grain = 0;
#ifdef BLOOM_FILTER
	bf_reset(d_cache->member.hot_bf);
#endif
	for (int i = 0; i < d_cache->env.nr_valid_hot_tpages; i++)
	{
		for (int j = 0; j < EPP; j++)
		{
			d_cache->member.hot_mem_table[i][j].lpa = UINT32_MAX;
			d_cache->member.hot_mem_table[i][j].ppa = UINT32_MAX;
#ifdef STORE_KEY_FP
			d_cache->member.hot_mem_table[i][j].key_fp = 0;
#endif
		}
	}
	return 0;
}

int dftl_cache_load(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, bool is_hot)
{
	struct cmt_struct *cmt;
	if (is_hot)
	{
		lpa_t new_lpa = lpa;
		cmt = self->member.hot_cmt[D_IDX_HOT];
	}
	else
	{
		cmt = self->member.cold_cmt[D_IDX];
	}

	struct inflight_params *i_params;
	if (IS_INITIAL_PPA(cmt->t_ppa))
	{
		return 0;
	}

	i_params = get_iparams(req, wb_entry);
	i_params->jump = GOTO_LIST;

	self->stat.cache_load++;
	uint64_t lat = 0;
	__demand.li->read(cmt->t_ppa, PAGESIZE, 0);
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

int dftl_cache_upgrade_hot(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim)
{
	for (int i = 0; i < EPP; i++)
	{
		/*direct mapping may cause rewrite and collision*/
		if (victim->pt[i].ppa != UINT32_MAX && victim->cnt_map[i] >= T)
		{
			lpa_t lpa = IDX_TO_LPA(victim->idx, i);
			lpa_t new_lpa = lpa % (self->env.nr_valid_hot_tpages * EPP);
			struct cmt_struct *cmt = self->member.hot_cmt[D_IDX_HOT];
			self->stat.up_grain_cnt++;
			self->stat.up_hit_cnt += victim->cnt_map[i];
			int d_idx = D_IDX_HOT;
			int p_idx = P_IDX_HOT;
			int probe = 1;
			bool found = false;

			// 线性探测找到空闲位置
			while (self->member.hot_mem_table[d_idx][p_idx].lpa != UINT32_MAX && probe < MAX_PROBE)
			{
				// linear
				// p_idx = (p_idx + 1);
				// quadratic
				p_idx = (p_idx + probe * probe);
				probe++;
				if (p_idx > EPP)
				{
					d_idx = (d_idx + p_idx / EPP) % self->env.nr_valid_hot_tpages;
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
				self->stat.hot_rewrite_grain++;
				d_idx = D_IDX_HOT;
				p_idx = P_IDX_HOT;
			}
#ifdef BLOOM_FILTER
			bf_set(self->member.hot_bf, lpa);
			self->member.hot_bf->valid_entry++;
#endif
			self->member.hot_mem_table[d_idx][p_idx].lpa = lpa;
			self->member.hot_mem_table[d_idx][p_idx].ppa = victim->pt[i].ppa;
			self->member.hot_mem_table[d_idx][p_idx].key_fp = victim->pt[i].key_fp;
		}
		self->stat.up_grain_distribute[victim->cnt_map[i]]++;
	}
	self->stat.up_page_cnt++;
	return 1;
}

int dftl_cache_hot_evict(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	int result = 0;
	lpa_t new_lpa = lpa;
	struct cmt_struct *cmt = self->member.hot_cmt[D_IDX_HOT];
	struct cmt_struct *victim = NULL;
	algorithm *palgo = self->env.palgo;
	w_buffer_t *pw_buffer = D_ENV(palgo)->pw_buffer;

	if (self->is_full(self, true) == HOT_FULL)
	{
		victim = (struct cmt_struct *)lru_pop(self->member.hot_lru);
		self->member.nr_cached_hot_tpages--;
		victim->lru_ptr = NULL;
		victim->is_cached = false;
		if (!IS_INITIAL_PPA(victim->t_ppa))
			pbm->invalidate_page(pbm, victim->t_ppa);

		victim->t_ppa = tp_alloc(pbm);
		pbm->validate_page(pbm, victim->t_ppa);
		victim->state = CLEAN;

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
	}

	cmt->is_cached = true;
	cmt->lru_ptr = lru_push(self->member.hot_lru, (void *)cmt);
	self->member.nr_cached_hot_tpages++;
	return result;
}

int dftl_cache_list_up(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry)
{
	int rc = 0;

	struct cmt_struct *cmt = self->member.cold_cmt[D_IDX];
	struct cmt_struct *victim = NULL;
	algorithm *palgo = self->env.palgo;
	w_buffer_t *pw_buffer = D_ENV(palgo)->pw_buffer;

	struct inflight_params *i_params;

	if (self->is_full(self, false) == COLD_FULL)
	{
		// upgrade to hot cmt
		// victim = (struct cmt_struct *)lru_pop(self->member.cold_lru);
		victim = (struct cmt_struct *)lru_pop(self->member.cold_lru);

		if (victim->heat_cnt > T)
		{
			self->upgrade_hot(self, UINT32_MAX, NULL, NULL, victim);
		}
		self->member.nr_cached_tpages--;
		victim->lru_ptr = NULL;
		victim->pt = NULL;
		victim->heat_cnt = 0;
		g_free(victim->hit_bitmap);
		g_free(victim->cnt_map);
		// flush dirty page
		if (victim->state == DIRTY)
		{
			self->stat.dirty_evict++;
			i_params = get_iparams(req, wb_entry);
			i_params->jump = GOTO_COMPLETE;

			victim->t_ppa = tp_alloc(pbm);
			pbm->validate_page(pbm, victim->t_ppa);
			victim->state = CLEAN;

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
	cmt->hit_bitmap = (bool *)g_malloc0(sizeof(bool) * EPP);
	cmt->cnt_map = (uint8_t *)g_malloc0(sizeof(uint8_t) * EPP);
	cmt->heat_cnt = 0;
	cmt->lru_ptr = lru_push(self->member.cold_lru, (void *)cmt);
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
	struct cmt_struct *cmt = self->member.cold_cmt[D_IDX];
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
	struct cmt_struct *cmt = self->member.cold_cmt[D_IDX];
	lru_update(self->member.cold_lru, cmt->lru_ptr);
	return 0;
}

int dftl_cache_update(demand_cache *self, lpa_t lpa, struct pt_struct pte)
{
	struct cmt_struct *cmt = self->member.cold_cmt[D_IDX];

	if (cmt->pt)
	{
		cmt->pt[P_IDX] = pte;
		cmt->hit_bitmap[P_IDX] = true;
		if (!IS_INITIAL_PPA(cmt->t_ppa) && cmt->state == CLEAN)
		{
			pbm->invalidate_page(pbm, cmt->t_ppa);
			cmt->t_ppa = UINT32_MAX;
		}
		cmt->state = DIRTY;
		lru_update(self->member.cold_lru, cmt->lru_ptr);
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
	struct cmt_struct *cmt = self->member.cold_cmt[D_IDX];
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
	return self->member.cold_cmt[D_IDX];
}

bool dftl_cache_is_hit(demand_cache *self, lpa_t lpa)
{
	struct cmt_struct *cmt = self->member.cold_cmt[D_IDX];
	if (cmt->pt != NULL)
	{
		cmt->hit_bitmap[P_IDX] = true;
		cmt->cnt_map[P_IDX]++;
		cmt->heat_cnt++;
		return 1;
	}
	else
	{
		return 0;
	}
}

uint32_t dftl_cache_is_full(demand_cache *self, bool is_hot)
{
	if (self->member.nr_cached_hot_tpages > self->env.max_cached_hot_tpages && is_hot)
		return HOT_FULL;
	else if (self->member.nr_cached_tpages >= self->env.max_cached_tpages)
		return COLD_FULL;
	return NOT_FULL;
}

int dftl_cache_hot_is_hit(demand_cache *self, lpa_t lpa, pte_t *pte)
{
	// 计算初始索引
	lpa_t new_lpa = lpa % (self->env.nr_valid_hot_tpages * EPP);
	int d_idx = D_IDX_HOT;
	int p_idx = P_IDX_HOT;
	int probe = 1;
#ifdef BLOOM_FILTER
	if (!bf_check(self->member.hot_bf, lpa))
	{
		return HOT_MISS;
	}
#endif
	while (probe < MAX_PROBE)
	{
		if (self->member.hot_mem_table[d_idx][p_idx].lpa == lpa)
		{
			pte->ppa = self->member.hot_mem_table[d_idx][p_idx].ppa;
			pte->key_fp = self->member.hot_mem_table[d_idx][p_idx].key_fp;
			return HOT_HIT;
		}
		else if (self->member.hot_mem_table[d_idx][p_idx].lpa == UINT32_MAX)
		{
			self->stat.hot_miss++;
			return HOT_MISS;
		}
		// linear
		// p_idx = p_idx + 1;
		// quadratic
		p_idx = (p_idx + probe * probe);
		probe++;
		if (p_idx > EPP)
		{
			d_idx = (d_idx + p_idx / EPP) % self->env.nr_valid_hot_tpages;
			p_idx = p_idx % EPP;
		}
	}
	self->stat.hot_miss++;
	return HOT_MISS;
}