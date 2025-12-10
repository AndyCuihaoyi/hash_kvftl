/*
 * Header for Cache module
 */

#ifndef __CACHE_H__
#define __CACHE_H__

#include "dftl_types.h"
#include "dftl_utils.h"
#include "../tools/lru_list.h"
#include "../tools/skiplist.h"
#include "../tools/fifo_queue.h"

/* Structures */
struct cache_env
{
	uint64_t nr_tpages_optimal_caching;
	uint64_t nr_valid_tpages;
	uint64_t nr_valid_tentries;

	uint64_t max_cached_tpages;
	uint64_t max_cached_tentries;
#ifdef HOT_CMT
	uint64_t max_cached_hot_tpages;
	uint64_t max_cached_hot_entries;
#endif
	uint64_t max_cache_entry;

	/* add attributes here */
	algorithm *palgo;
};

struct cache_member
{
	struct cmt_struct **cmt;
	struct pt_struct **mem_table;
#ifdef HOT_CMT
	struct cmt_struct **hot_cmt;
	struct hot_pt_struct **hot_mem_table;
#endif
	LRU *lru;

	int nr_cached_tpages;
	int nr_cached_tentries;
#ifdef HOT_CMT
	int nr_cached_hot_tpages;
	int nr_cached_hot_tentries;
#endif
#ifdef PREFILL_CACHE
	Queue prefill_q;
#endif

	/* add attributes here */
	volatile int nr_tpages_read_done;
	volatile int nr_valid_read_done;
};

struct cache_stat
{
	/* cache performance */
	uint64_t cache_hit;
	uint64_t cache_miss;
	uint64_t clean_evict;
	uint64_t dirty_evict;
	uint64_t blocked_miss;

	/* add attributes here */
	uint64_t cache_miss_by_collision;
	uint64_t cache_hit_by_collision;
	uint64_t cache_load;

#ifdef HOT_CMT
	uint64_t hot_cmt_hit;
	uint64_t hot_valid_entries;
	uint64_t hot_rewrite_entries;
	uint64_t up_grain_cnt;
	uint64_t up_hit_cnt;
	uint64_t up_page_cnt;
	uint32_t *grain_heat_distribute;
#endif
};

typedef struct demand_cache
{
	int (*create)(struct demand_cache *);
	int (*destroy)();

	int (*load)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
	int (*list_up)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
	int (*wait_if_flying)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);

	int (*touch)(struct demand_cache *self, lpa_t lpa);
	int (*update)(struct demand_cache *self, lpa_t lpa, struct pt_struct pte);

	struct pt_struct (*get_pte)(struct demand_cache *self, lpa_t lpa);
	struct cmt_struct *(*get_cmt)(struct demand_cache *self, lpa_t lpa);

	bool (*is_hit)(struct demand_cache *self, lpa_t lpa);
	bool (*is_full)(struct demand_cache *self);
#ifdef HOT_CMT
	int (*promote_hot)(struct demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim);
	bool (*hot_is_hit)(struct demand_cache *self, lpa_t lpa, struct hot_pt_struct **hot_pte);
	int (*hot_cmt_reset)(struct demand_cache *self);
#endif
	struct cache_env env;
	struct cache_member member;
	struct cache_stat stat;
} demand_cache;

#endif
