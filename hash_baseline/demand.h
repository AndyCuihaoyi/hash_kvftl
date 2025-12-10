#ifndef __DFTL_H__
#define __DFTL_H__

#include "dftl_types.h"
#include "dftl_utils.h"
#include "../tools/lru_list.h"
#include "../tools/rte_ring/rte_ring.h"
#include "../tools/bloomfilter.h"
#include "cache.h"
#include "write_buffer.h"
#include <stdint.h>

extern uint64_t extra_mem_lat;

#define ENTRY_SIZE 8

#define EPP (PAGESIZE / ENTRY_SIZE) // Number of table entries per page
#define D_IDX (lpa / EPP)           // Idx of directory table
#define P_IDX (lpa % EPP)           // Idx of page table

#define CLEAN 0
#define DIRTY 1

#define QUADRATIC_PROBING(h, c) ((h) + (c) + (c) * (c))
#define LINEAR_PROBING(h, c) (h + c)

#define PROBING_FUNC(h, c) QUADRATIC_PROBING(h, c)

#define D_ENV(p_algo) ((demand_env *)(p_algo->env))

// Page table entry
typedef struct __attribute__((packed)) pt_struct
{
    ppa_t ppa; // Index = lpa
#ifdef STORE_KEY_FP
    fp_t key_fp;
#endif
} pte_t;

extern KEYT *real_keys;

// Cache mapping table data strcuture
typedef struct cmt_struct
{
    int32_t idx;
    pte_t *pt;
    ppa_t t_ppa;

    bool state; // CLEAN / DIRTY
    bool is_flying;

    struct rte_ring *retry_q;
    struct rte_ring *wait_q;
    BF *bf;
    NODE *lru_ptr;

    bool *is_cached;
    uint32_t cached_cnt;
    uint32_t dirty_cnt;
} cmt_t;

typedef struct demand_env
{
    uint32_t num_page;
    uint32_t num_grain;
    uint32_t max_cache_entry;
    uint32_t num_block;
    uint32_t p_p_b;
    uint32_t num_tblock;
    uint32_t num_tpage;
    uint32_t num_dblock;
    uint32_t num_dpage;
    uint32_t num_dgrain;
    uint32_t nr_pages_optimal_caching;
    uint32_t num_max_cache;
    uint32_t real_max_cache;
    uint32_t max_write_buf;
    uint32_t max_try;

    /* for statistics */
    uint64_t num_rd_wb_hit;
    uint64_t num_rd_data_rd;
    uint64_t num_rd_data_miss_rd;
    uint64_t r_hash_collision_cnt[MAX_HASH_COLLISION + 1];
    uint64_t w_hash_collision_cnt[MAX_HASH_COLLISION + 1];

    /* components */
    demand_cache *pd_cache;
    w_buffer_t *pw_buffer;
} demand_env;

/* extern variables */
extern algorithm __demand;
extern demand_env d_env;

// dftl.c
uint32_t demand_create(algorithm *, lower_info *);
void demand_destroy(algorithm *, lower_info *);
lpa_t get_lpa(demand_cache *pd_cache, KEYT key, void *_h_params);
uint32_t demand_set(algorithm *, request *const);
uint32_t demand_get(algorithm *, request *const);
uint32_t demand_remove(algorithm *, request *const); // not implemented

// dftl_range.c
uint32_t demand_range_query(algorithm *, request *const);
bool range_end_req(algorithm *, request *);

// dftl_utils.c
void cache_show(char *dest);

#endif // __DFTL_H__
