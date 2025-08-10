/*
 * Header for Cache module
 */

#ifndef __CACHE_H__
#define __CACHE_H__

#include "dftl_types.h"
#include "dftl_utils.h"
#include "../tools/lru_list.h"
#include "../tools/skiplist.h"

/* Structures */
struct cache_env
{
	uint64_t nr_tpages_optimal_caching;
	uint64_t nr_valid_tpages;
	uint64_t nr_valid_tentries;

	uint64_t max_cached_tpages;
	uint64_t max_cached_tentries;
	uint64_t max_cached_hot_tpages;
	uint64_t max_cache_entry;

	/* add attributes here */
	algorithm *palgo;
};

struct cache_member
{
	struct cmt_struct **cold_cmt;
	struct hot_cmt_struct **hot_cmt;
	struct pt_struct **mem_table;
	struct pt_struct **hot_mem_table;
	LRU *lru;

	int nr_cached_tpages;
	int nr_cached_tentries;

	/* add attributes here */
	volatile int nr_tpages_read_done;
	volatile int nr_valid_read_done;
	uint32_t max_hit;
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

	struct cache_env env;
	struct cache_member member;
	struct cache_stat stat;
} demand_cache;

#endif
