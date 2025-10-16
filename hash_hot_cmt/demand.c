#include "demand.h"
#include "bm.h"
#include "cache.h"
#include "dftl_pg.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "../lower/lower.h"
#include "../tools/rte_ring/rte_ring.h"
#include "../tools/skiplist.h"
#include "request.h"
#include "write_buffer.h"
#include <linux/limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

KEYT *real_keys;

uint64_t extra_mem_lat = 0;

KEYT key_max, key_min;

extern lower_info ssd_li; // in lower/ssd.c

algorithm __demand = {.create = demand_create,
                      .destroy = demand_destroy,
                      .read = demand_get,
                      .write = demand_set,
                      .remove = NULL,
                      .range_query = NULL,
                      .li = &ssd_li,
                      .env = &d_env};

extern w_buffer_t write_buffer;
extern w_buffer_t *pw_buffer;
extern demand_cache d_cache;
extern demand_cache *pd_cache;
extern block_mgr_t bm;
extern block_mgr_t *pbm;

struct demand_env d_env;

static inline int KEYCMP(KEYT a, KEYT b)
{
    if (!a.len && !b.len)
        return 0;
    else if (a.len == 0)
        return -1;
    else if (b.len == 0)
        return 1;

    int r = memcmp(a.key, b.key, a.len > b.len ? b.len : a.len);
    if (r != 0 || a.len == b.len)
    {
        return r;
    }
    return a.len < b.len ? -1 : 1;
}

uint32_t demand_create(algorithm *palgo, lower_info *li)
{
    /* Initialize pre-defined values by using macro */
    D_ENV(palgo)->num_page = _NOP;
    ftl_log("[DVALUE] Using grained mapping table (GRAINED_UNIT:%dB)\n",
            (int)GRAINED_UNIT);
    D_ENV(palgo)->num_grain = D_ENV(palgo)->num_page * GRAIN_PER_PAGE;
    D_ENV(palgo)->max_cache_entry =
        (D_ENV(palgo)->num_grain / EPP) + ((D_ENV(palgo)->num_grain % EPP != 0) ? 1 : 0); // total number of tpages

    D_ENV(palgo)->num_block = _NOS;
    D_ENV(palgo)->p_p_b = _PPS;
    D_ENV(palgo)->num_tblock = (D_ENV(palgo)->max_cache_entry / D_ENV(palgo)->p_p_b) +
                               (D_ENV(palgo)->max_cache_entry % D_ENV(palgo)->p_p_b ? 1 : 0);
    D_ENV(palgo)->num_tblock = (D_ENV(palgo)->num_tblock < 2) ? 2 : D_ENV(palgo)->num_tblock;
    // num_tblock      = ((num_block / EPP) + ((num_block % EPP != 0) ? 1 : 0))
    // * 4;
    D_ENV(palgo)->num_tpage = D_ENV(palgo)->num_tblock * D_ENV(palgo)->p_p_b;
    D_ENV(palgo)->num_dblock = D_ENV(palgo)->num_block - D_ENV(palgo)->num_tblock - 2;
    D_ENV(palgo)->num_dpage = D_ENV(palgo)->num_dblock * D_ENV(palgo)->p_p_b;
    D_ENV(palgo)->num_dgrain = D_ENV(palgo)->num_dpage * GRAIN_PER_PAGE;

    /* Init statistics */
    D_ENV(palgo)->num_rd_wb_hit = 0;
    D_ENV(palgo)->num_rd_data_rd = 0;
    D_ENV(palgo)->num_rd_data_miss_rd = 0;
    D_ENV(palgo)->num_data_gc = 0;
    D_ENV(palgo)->num_gc_flash_read = 0;
    D_ENV(palgo)->num_gc_flash_write = 0;

    /* Init Write Buffer */
    key_max.len = MAXKEYSIZE;
    memset(key_max.key, -1, sizeof(char) * MAXKEYSIZE);

    key_min.len = MAXKEYSIZE;
    memset(key_min.key, 0, sizeof(char) * MAXKEYSIZE);

    pw_buffer->create(pw_buffer);
    D_ENV(palgo)->pw_buffer = pw_buffer;
    D_ENV(palgo)->pw_buffer->env->palgo = palgo;

    /* Cache control & Init */
    D_ENV(palgo)->nr_pages_optimal_caching = (D_ENV(palgo)->num_page * 4 / PAGESIZE);
    //	num_max_cache = nr_pages_optimal_caching / 25;
    D_ENV(palgo)->num_max_cache = D_ENV(palgo)->nr_pages_optimal_caching / 5;
    // num_max_cache = max_cache_entry * 2;
    // num_max_cache = max_cache_entry; // Full cache
    // num_max_cache = max_cache_entry / 4; // 25%
    // num_max_cache = max_cache_entry / 8; // 12.5%
    // num_max_cache = max_cache_entry / 10; // 10%
    // num_max_cache = max_cache_entry / 20; // 5%
    // num_max_cache = max_cache_entry / 25; // 4%
    // num_max_cache = 1; // 1 cache

    D_ENV(palgo)->real_max_cache = D_ENV(palgo)->num_max_cache;

    /* mapping cache init */
    pd_cache->create(pd_cache);
    D_ENV(palgo)->pd_cache = pd_cache;
    pd_cache->env.palgo = palgo;

    D_ENV(palgo)->max_write_buf = MAX_WRITE_BUF;
    D_ENV(palgo)->max_try = 0;

    /* bm init */
    pbm->create(pbm);

    /* pg init */
    dftl_page_init(pbm);

    /* queue init */
    palgo->req_q = ring_create(RING_TYPE_MP_SC, MAX_INF_REQS);
    palgo->retry_q = algo_q_create();
    palgo->finish_q = ring_create(RING_TYPE_MP_MC, MAX_INF_REQS);

    /* real_keys init (for simplicity) */
    real_keys = (KEYT *)malloc(sizeof(KEYT) * D_ENV(palgo)->pd_cache->env.nr_valid_tpages * EPP);
    if (-1 == mlock(real_keys, sizeof(KEYT) * D_ENV(palgo)->pd_cache->env.nr_valid_tpages * EPP))
    {
        ftl_err("real_keys mlock() failed!\n");
        abort();
    }

    memset(D_ENV(palgo)->r_hash_collision_cnt, 0, sizeof(D_ENV(palgo)->r_hash_collision_cnt));
    memset(D_ENV(palgo)->w_hash_collision_cnt, 0, sizeof(D_ENV(palgo)->w_hash_collision_cnt));

    return 0;
}

void demand_destroy(algorithm *palgo, lower_info *li)
{
    ring_free(palgo->req_q);
    palgo->req_q = NULL;
    algo_q_free(palgo->retry_q);
    palgo->retry_q = NULL;
    ring_free(palgo->finish_q);
    palgo->finish_q = NULL;
    free(real_keys);
    real_keys = NULL;
    pbm->destroy(pbm);
    li->destroy(li);
    demand_cache *pd_cache = D_ENV(palgo)->pd_cache;
    w_buffer_t *pw_buffer = D_ENV(palgo)->pw_buffer;
    pw_buffer->destroy(pw_buffer);
    pd_cache->destroy(pd_cache);
}

lpa_t get_lpa(demand_cache *pd_cache, KEYT key, void *_h_params)
{
    struct hash_params *h_params = (struct hash_params *)_h_params;
    uint32_t lpa = PROBING_FUNC(h_params->hash, h_params->cnt) % pd_cache->env.nr_valid_tentries;
    return lpa;
}

uint32_t __demand_set(algorithm *palgo, request *const req)
{
    uint32_t rc = 0;
    ftl_assert(pw_buffer);
    w_buffer_t *wb = D_ENV(palgo)->pw_buffer;

    /* flush the buffer if full */
    if (wb->is_full(wb))
    {
        /* assign ppa first */
        wb->assign_ppa(wb, req);

        /* mapping update [lpa, origin]->[lpa, new] */
        wb->mapping_update(wb, req);

        /* flush the buffer */
        // wb->stats->nr_flush++;
        wb->flush(wb, req);
    }

    /* default: insert to the buffer */
    rc = wb->insert(wb, req); // rc: is the write buffer is full? 1 : 0
    while (!ring_enqueue(__demand.finish_q, (void *)&req, 1))
        ;
    return rc;
}

uint32_t __demand_get(algorithm *palgo, request *const req)
{
    uint32_t rc = 0;
    ftl_assert(pw_buffer);
    w_buffer_t *wb = D_ENV(palgo)->pw_buffer;
    demand_cache *pd_cache = D_ENV(palgo)->pd_cache;

    hash_params *h_params = req->h_params;
    lpa_t lpa;
    pte_t pte;

read_retry:
    lpa = get_lpa(D_ENV(palgo)->pd_cache, req->key, req->h_params);
    pte.ppa = UINT32_MAX;
#ifdef STORE_KEY_FP
    pte.key_fp = FP_MAX;
#endif

    if (h_params->cnt > D_ENV(palgo)->max_try)
    {
        rc = UINT32_MAX;
        warn_notfound(__FILE__, __LINE__);
        goto read_ret;
    }

    /* inflight request */
    if (IS_INFLIGHT(req->params))
    {
        struct inflight_params *i_params = (struct inflight_params *)req->params;
        jump_t jump = i_params->jump;
        free_iparams(req, NULL);

        switch (jump)
        {
        case GOTO_LOAD:
            goto cache_load;
        case GOTO_LIST:
        case GOTO_EVICT:
            goto cache_list_up;
        case GOTO_COMPLETE:
            // pte = i_params->pte;
            goto cache_check_complete;
        case GOTO_READ:
            goto data_read;
        default:
            printf("[ERROR] No jump type found, at %s:%d\n", __FILE__, __LINE__);
            abort();
        }
    }

    /* 1. check write buffer first */
    if (h_params->cnt == 0)
    {
        rc = wb->do_check(wb, req);
        if (rc)
        {
            D_ENV(palgo)->num_rd_wb_hit++;
            while (!ring_enqueue(__demand.finish_q, (void *)&req, 1))
                ;
            goto read_ret;
        }
    }

/* 2. check cache */
#ifdef HOT_CMT
    if (h_params->cnt == 0 && pd_cache->hot_is_hit(pd_cache, lpa, &pte) == true)
    {
        if (h_params->key_fp == pte.key_fp)
        {
            pd_cache->stat.hot_cmt_hit++;
            goto data_read;
        }
    }
#endif
    if (pd_cache->is_hit(pd_cache, lpa))
    {
        pd_cache->touch(pd_cache, lpa);
        if (req->h_params->cnt > 0)
        {
            pd_cache->stat.cache_hit_by_collision++;
        }
        else
        {
            pd_cache->stat.cache_hit++;
        }
    }
    else
    {
        if (req->h_params->cnt > 0)
        {
            pd_cache->stat.cache_miss_by_collision++;
        }
        else
        {
            pd_cache->stat.cache_miss++;
        }
    cache_load:
        // rc = pd_cache->wait_if_flying(lpa, req, NULL);
        if (rc)
        {
            goto read_ret;
        }
        rc = pd_cache->load(pd_cache, lpa, req, NULL);
        if (!rc)
        {
            rc = UINT32_MAX;
            warn_notfound(__FILE__, __LINE__);
            req->state = ALGO_REQ_NOT_FOUND;
            while (!ring_enqueue(__demand.finish_q, (void *)&req, 1))
                ;
        }
        algo_q_insert_sorted(__demand.retry_q, req, NULL);
        goto read_ret;
    cache_list_up:
        rc = pd_cache->list_up(pd_cache, lpa, req, NULL);
        if (rc)
        {
            algo_q_insert_sorted(__demand.retry_q, req, NULL);
            goto read_ret;
        }
    }

cache_check_complete:
    // free_iparams(req, NULL);

    pte = pd_cache->get_pte(pd_cache, lpa);
#ifdef STORE_KEY_FP
    /* fast fingerprint compare */
    if (h_params->key_fp != pte.key_fp)
    {
        h_params->cnt++;
        goto read_retry;
    }
#endif

    // add latency for memory cache access
    busy_wait_ns(extra_mem_lat);

data_read:
    /* 3. read actual data */
    D_ENV(palgo)->num_rd_data_rd++;
    rc = read_actual_dpage(pbm, pte.ppa, req); // after async read, should check full key.
    if (rc == UINT32_MAX)
    {
        goto read_ret;
    }
    h_params = (struct hash_params *)req->h_params;
    KEYT *real_key = &real_keys[lpa];
    if (KEYCMP(req->key, *real_key) == 0)
    {
        hash_collision_logging(h_params->cnt, READ);
        free(h_params);
        while (!ring_enqueue(__demand.finish_q, (void *)&req, 1))
            ;
    }
    else
    {
        h_params->find = HASH_KEY_DIFF;
        h_params->cnt++;
        D_ENV(palgo)->num_rd_data_miss_rd++;
        algo_q_insert_sorted(__demand.retry_q, req, NULL);
    }

read_ret:
    return rc;
}

uint32_t demand_get(algorithm *palgo, request *const req)
{
    if (!req->h_params)
    {
        req->h_params = make_hash_params(req);
    }
    uint32_t rc = __demand_get(palgo, req);
    if (rc == UINT32_MAX)
    {
        req->state = ALGO_REQ_NOT_FOUND;
        while (!ring_enqueue(__demand.finish_q, (void *)&req, 1))
            ;
    }

    return 0;
}

uint32_t demand_set(algorithm *palgo, request *const req)
{
    if (!req->h_params)
    {
        req->h_params = make_hash_params(req);
    }
    uint32_t rc = __demand_set(palgo, req);
    return 0;
}