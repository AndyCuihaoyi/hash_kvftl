#include "dftl_wb.h"
#include "request.h"
#include "bm.h"
#include "demand.h"
#include "dftl_cache.h"
#include "dftl_pg.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "../lower/lower.h"
#include "../tools/d_htable.h"
#include "../tools/skiplist.h"
#include "../tools/valueset.h"
#include "write_buffer.h"
#include <pthread.h>
#include <string.h>
#include <glib-2.0/glib.h>

#define WB_HIT(x) ((x) != NULL)

extern block_mgr_t bm;
extern block_mgr_t *pbm;

wb_stats_t wb_stats;

wb_env_t wb_env = {.max_wb_size = MAX_WRITE_BUF,
                   .flush_list = NULL,
                   .wb_master_q = NULL,
                   .wb_retry_q = NULL};

w_buffer_t write_buffer = {.wb = NULL,
                           .env = &wb_env,
                           .create = dftl_wb_init,
                           .destroy = dftl_wb_destroy,
                           .is_full = wb_is_full,
                           .do_check = do_wb_check,
                           .insert = _do_wb_insert,
                           .assign_ppa = _do_wb_assign_ppa,
                           .mapping_update = _do_wb_mapping_update,
                           .flush = _do_wb_flush,
                           .stats = &wb_stats};
w_buffer_t *pw_buffer = &write_buffer;

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

static void copy_value(value_set *dst, value_set *src, int size)
{
    memcpy(dst->value, src->value, size);
}

void dftl_wb_stats_init(w_buffer_t *self)
{
    ftl_assert(w_buffer->stats);
    self->stats->nr_rd_hit = 0;
    self->stats->nr_rd_miss = 0;
    self->stats->nr_wr_hit = 0;
    self->stats->nr_wr_miss = 0;
}

void dftl_wb_init(w_buffer_t *self)
{
    wb_env_t *env = self->env;
    dftl_wb_stats_init(self);
    self->wb = skiplist_init();
    env->flush_list = (struct flush_list *)malloc(sizeof(struct flush_list));

#ifdef DATA_SEGREGATION
    env->flush_list->list = (struct flush_node *)calloc(
        self->env->max_wb_size, sizeof(struct flush_node));
    env->flush_list->size = 0;
#else
    env->flush_list->size = 0;
    env->flush_list->list = (struct flush_node *)calloc(
        self->env->max_wb_size, sizeof(struct flush_node));
#endif
    env->wb_master_q = ring_create(RING_TYPE_MP_SC, self->env->max_wb_size * 2);
    env->wb_retry_q = algo_q_create();
    env->hash_table = d_htable_init(self->env->max_wb_size * 2);
}

void dftl_wb_destroy(w_buffer_t *self)
{
    wb_env_t *env = self->env;
    skiplist_free(self->wb);
    free(env->flush_list->list);
    free(env->flush_list);
    ring_free(env->wb_master_q);
    algo_q_free(env->wb_retry_q);
}

bool wb_is_full(w_buffer_t *self)
{
    return (self->wb->size == self->env->max_wb_size);
}

uint32_t do_wb_check(w_buffer_t *self, request *const req)
{
    skiplist *wb = self->wb;
    snode *wb_entry = skiplist_find(wb, req->key);
    if (WB_HIT(wb_entry))
    {
        self->stats->nr_rd_hit++;
        free(req->h_params);
        copy_value(req->value, wb_entry->value,
                   wb_entry->value->length_in_bytes);
        req->value->length_in_bytes = 0;
        req->value->offset = 0;
        // req->type_ftl = 0;
        // req->type_lower = 0;
        return 1;
    }
    else
    {
        self->stats->nr_rd_miss++;
    }
    return 0;
}

uint32_t _do_wb_insert(w_buffer_t *self, request *const req)
{
    skiplist *wb = self->wb;

// #define FOR_WB_WR_HIT_STATS
#ifdef FOR_WB_WR_HIT_STATS
    snode *tmp_wb_entry = skiplist_find(wb, req->key);
    if (WB_HIT(tmp_wb_entry))
    {
        w_buffer->stats->nr_wr_hit++;
    }
    else
    {
        w_buffer->stats->nr_wr_miss++;
    }
#endif // FOR_WB_WR_HIT_STATS

    // snode *wb_entry = skiplist_insert(wb, req->key, req->value, true);
    snode *wb_entry = skiplist_insert(wb, req->key, req->value, true);
    wb_entry->hash_params = (void *)req->h_params;
    ftl_assert(wb_entry->value->value);
    req->value = NULL;

    if (wb_is_full(self))
        return 1;
    else
        return 0;
}

void _do_wb_assign_ppa(w_buffer_t *self, request *req)
{
    skiplist *wb = self->wb;
    wb_env_t *env = self->env;
    struct flush_list *fl = env->flush_list;

    snode *wb_entry;
    sk_iter *iter = skiplist_get_iterator(wb);
#ifdef DATA_SEGREGATION
    for (size_t i = 0; i < MAX_WRITE_BUF; i++)
    {
        wb_entry = skiplist_get_next(iter);
        int val_len_in_bytes = (wb_entry->value->length_in_bytes + sizeof(uint8_t) + sizeof(uint32_t) + wb_entry->key.len);
        int val_len = val_len_in_bytes / GRAINED_UNIT + (val_len_in_bytes % GRAINED_UNIT != 0 ? 1 : 0);
        lpa_t lpa = get_lpa(((demand_env *)self->env->palgo->env)->pd_cache, wb_entry->key, wb_entry->hash_params);
        ppa_t ppa;
        uint32_t stream_idx = D_IDX % MAX_GC_STREAM;
        bm_stream_manager_t *stream = &pbm->env->stream[stream_idx];
        if (stream->grain_remain == 0)
        {
            stream->active_ppa = dp_alloc(pbm, lpa);
            stream->grain_remain = GRAIN_PER_PAGE;
            stream->page_remain--;
        }
        if (stream->page_remain == 0)
        {
            uint64_t maxlat = 0;
            uint64_t lat = __demand.li->write(stream->flush_ppa, stream->flush_page * PAGESIZE, 0);
            if (lat == -1 || lat == -2)
            {
                abort();
            }
            maxlat = lat > maxlat ? lat : maxlat;
            req->etime = clock_get_ns() + maxlat;

            stream->flush_ppa = stream->active_ppa;
            int page_remain = SBLK_OFFT2PPA(stream->active_sblk, SBLK_END) - stream->active_ppa;
            stream->page_remain = page_remain >= 64 ? 64 : page_remain;
            stream->flush_page = stream->page_remain;
            stream->grain_remain = GRAIN_PER_PAGE;
        }
        ppa = stream->active_ppa;
        uint32_t offset = GRAIN_PER_PAGE - stream->grain_remain;
        wb_entry->ppa = PPA_TO_PGA(ppa, offset);
        stream->grain_remain -= val_len;

        inf_free_valueset(&wb_entry->value);
        wb_entry->value = NULL;
        // if (stream->active_sblk->wp_offt == _PPS)
        // {
        //     printf("stream idx:%d, valid cnt:%d, remain page: %d, remain grain:%d, sblk wp:%d\n",
        //            stream_idx, stream->active_sblk->valid_cnt, stream->page_remain, stream->grain_remain, stream->active_sblk->wp_offt);
        // }
        for (int i = 0; i < val_len; i++)
        {
            pbm->validate_grain(pbm, wb_entry->ppa + i);
        }

        bm_oob_t new_oob = {
            .is_tpage = false,
            .lpa = 0,
            .length = val_len,
        };
        pbm->set_oob(pbm, wb_entry->ppa, &new_oob); // wb_entry->ppa is a pga
    }
    free(iter);
#else
    l_bucket *wb_bucket = (l_bucket *)g_malloc0(sizeof(l_bucket));
    for (int i = 1; i <= GRAIN_PER_PAGE; i++)
    {
        wb_bucket->bucket[i] = (snode **)calloc(MAX_WRITE_BUF, sizeof(snode *));
        wb_bucket->idx[i] = 0;
    }

    for (size_t i = 0; i < MAX_WRITE_BUF; i++)
    {
        wb_entry = skiplist_get_next(iter);

        int val_len_in_bytes = (wb_entry->value->length_in_bytes + sizeof(uint8_t) + sizeof(uint32_t) + wb_entry->key.len);
        int val_len = val_len_in_bytes / GRAINED_UNIT + (val_len_in_bytes % GRAINED_UNIT != 0 ? 1 : 0);
        wb_bucket->bucket[val_len][wb_bucket->idx[val_len]] = wb_entry;
        wb_bucket->idx[val_len]++;
    }

    int ordering_done = 0;
    uint64_t tt_pg_offset = 0;
    ppa_t last_ppa = -2;
    while (ordering_done < MAX_WRITE_BUF)
    {
        // value_set *new_vs = inf_get_valueset(NULL, PAGESIZE);
        int remain = PAGESIZE;
        ppa_t ppa;
        ppa = dp_alloc(pbm, wb_entry->lpa);
        uint64_t offset = 0;
        if (ppa != last_ppa + 1)
        {
            fl->list[fl->size].ppa = ppa;
            fl->list[fl->size].length = 1;
            fl->list[fl->size].value = NULL;
            fl->list[fl->size].value_offt = tt_pg_offset;
        }
        else
        {
            fl->size--;
            fl->list[fl->size].length++;
        }
        last_ppa = ppa;

        while (remain > 0)
        {
            int target_length = remain / GRAINED_UNIT;
            while (wb_bucket->idx[target_length] == 0 && target_length != 0)
                --target_length;
            if (target_length == 0)
            {
                break;
            }

            wb_entry =
                wb_bucket
                    ->bucket[target_length][wb_bucket->idx[target_length] - 1];
            wb_bucket->idx[target_length]--;
            wb_entry->ppa = PPA_TO_PGA(ppa, offset);

            ftl_assert(offset * GRAINED_UNIT + sizeof(uint8_t) + wb_entry->key.len + sizeof(uint32_t) + wb_entry->value->length_in_bytes <= PAGESIZE);

            inf_free_valueset(&wb_entry->value);
            wb_entry->value = NULL;

            for (int i = 0; i < target_length; i++)
            {
                pbm->validate_grain(pbm, wb_entry->ppa + i);
            }

            bm_oob_t new_oob = {
                .is_tpage = false,
                .lpa = 0,
                .length = target_length,
            };
            pbm->set_oob(pbm, wb_entry->ppa, &new_oob); // wb_entry->ppa is a pga

            offset += target_length;
            remain -= target_length * GRAINED_UNIT;

            ordering_done++;
        }
        fl->size++;
        tt_pg_offset++;
    }

    for (int i = 1; i <= GRAIN_PER_PAGE; i++)
    {
        free(wb_bucket->bucket[i]);
    }
    free(wb_bucket);
    free(iter);
#endif
}

void _do_wb_mapping_update(w_buffer_t *self, request *req)
{
    wb_env_t *env = self->env;
    skiplist *wb = self->wb;
    algorithm *palgo = env->palgo;
    demand_cache *pd_cache = D_ENV(palgo)->pd_cache;
    int rc = 0;

    snode *wb_entry;
    hash_params *h_params;

    lpa_t lpa;
    pte_t pte, new_pte;

    /* push all the wb_entries to queue */
    sk_iter *iter = skiplist_get_iterator(wb);
    for (int i = 0; i < MAX_WRITE_BUF; i++)
    {
        wb_entry = skiplist_get_next(iter);
        while (!ring_enqueue(env->wb_master_q, (void *)&wb_entry, 1))
            ;
    }
    free(iter);

    /* mapping update */
    volatile int updated = 0;
    while (updated < MAX_WRITE_BUF)
    {
        uint64_t now = clock_get_ns();
        if (env->wb_retry_q->head && ((snode *)env->wb_retry_q->head->payload)->etime <= now)
        {
            wb_entry = algo_q_dequeue(env->wb_retry_q);
            rc = 1;
        }
        else
        {
            rc = 0;
        }
        if (!rc)
        {
            rc = ring_dequeue(env->wb_master_q, (void *)&wb_entry, 1);
            if (rc)
            {
                wb_entry->etime = req->stime;
            }
        }
        if (!rc)
            continue;

    wb_retry:
        h_params = wb_entry->hash_params;
        lpa = get_lpa(((demand_env *)self->env->palgo->env)->pd_cache, wb_entry->key, wb_entry->hash_params);
        new_pte.ppa = wb_entry->ppa;
#ifdef STORE_KEY_FP
        new_pte.key_fp = h_params->key_fp;
#endif
        KEYT new_real_key;
        new_real_key.len = wb_entry->key.len;
        memcpy(new_real_key.key, wb_entry->key.key, sizeof(new_real_key.key));

        /* inflight wb_entries */
        if (IS_INFLIGHT(wb_entry->params))
        {
            struct inflight_params *i_params =
                (struct inflight_params *)wb_entry->params;
            jump_t jump = i_params->jump;
            free_iparams(NULL, wb_entry);

            switch (jump)
            {
            case GOTO_LOAD:
                goto wb_cache_load;
            case GOTO_LIST:
                goto wb_cache_list_up;
            case GOTO_COMPLETE:
                goto wb_data_check;
            case GOTO_UPDATE:
                goto wb_update;
            default:
                printf("[ERROR] No jump type found, at %s:%d\n", __FILE__,
                       __LINE__);
                abort();
            }
        }
        if (pd_cache->is_hit(pd_cache, lpa))
        {
            pd_cache->touch(pd_cache, lpa);
            if (((hash_params *)wb_entry->hash_params)->cnt > 0)
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
            if (((hash_params *)wb_entry->hash_params)->cnt > 0)
            {
                pd_cache->stat.cache_miss_by_collision++;
            }
            else
            {
                pd_cache->stat.cache_miss++;
            }
        wb_cache_load:
            // rc = pd_cache->wait_if_flying(lpa, NULL, wb_entry);
            // if (rc)
            //     continue; /* pending */

            rc = pd_cache->load(pd_cache, lpa, NULL, wb_entry);
            if (rc)
            {
                algo_q_insert_sorted(env->wb_retry_q, NULL, wb_entry);
                continue; /* mapping read */
            }
        wb_cache_list_up:
            rc = pd_cache->list_up(pd_cache, lpa, NULL, wb_entry);
            if (rc)
            {
                algo_q_insert_sorted(env->wb_retry_q, NULL, wb_entry);
                continue; /* mapping write */
            }
        }

    wb_data_check:
        /* get page_table entry which contains {ppa, key_fp} */
        pte = pd_cache->get_pte(pd_cache, lpa);

        /* direct update at initial case */
        if (IS_INITIAL_PPA(pte.ppa))
        {
            goto wb_direct_update;
        }
        /* fast fingerprint compare */
#ifdef STORE_KEY_FP
        if (h_params->key_fp != pte.key_fp)
        {
            h_params->find = HASH_KEY_DIFF;
            h_params->cnt++;
            goto wb_retry;
        }
#endif
        /* hash_table lookup to filter same wb element.
         * 如果8B哈希冲突，则无法通过指纹判断。倘若上一个冲突项是在本次mapping更新中，则可以快速判断。否则需要上读数据检查key。
         */
        rc = d_htable_find(env->hash_table, pte.ppa, lpa);
        if (rc)
        {
            h_params->find = HASH_KEY_DIFF;
            h_params->cnt++;

            goto wb_retry;
        }

        // add latency for memory cache access
        busy_wait_ns(extra_mem_lat);

#ifdef UPDATE_DATA_CHECK
        /* data check is necessary before update */
        D_ENV(palgo)->num_rd_data_rd++;
        read_for_data_check(pbm, pte.ppa, wb_entry);
        KEYT *real_key = &real_keys[lpa];
        if (KEYCMP(wb_entry->key, *real_key) == 0)
        {
            h_params->find = HASH_KEY_SAME;
            struct inflight_params *i_params = get_iparams(NULL, wb_entry);
            i_params->jump = GOTO_UPDATE;
            req->etime = wb_entry->etime;
        }
        else
        {
            h_params->find = HASH_KEY_DIFF;
            h_params->cnt++;
            D_ENV(palgo)->num_rd_data_miss_rd++;
        }
        algo_q_insert_sorted(env->wb_retry_q, NULL, wb_entry);
#else
        req->etime = wb_entry->etime;
        goto wb_update;
#endif
        continue;
    /*update existing mapping item*/
    wb_update:
        pte = pd_cache->get_pte(pd_cache, lpa);
        if (!IS_INITIAL_PPA(pte.ppa))
        {
            int len = pbm->get_oob(pbm, pte.ppa)->length;
            for (int i = 0; i < len; i++)
            {
                pbm->invalidate_grain(pbm, pte.ppa + i);
            }
            static int over_cnt = 0;
            over_cnt++;
            if (over_cnt % 102400 == 0)
                ftl_log("overwrite: %d\n", over_cnt);
        }
    /*insert new mapping item*/
    wb_direct_update:
        pd_cache->update(pd_cache, lpa, new_pte);
        real_keys[lpa] = new_real_key;
        updated++;
        // inflight--;

        d_htable_insert(env->hash_table, new_pte.ppa, lpa);

        D_ENV(palgo)->max_try =
            (h_params->cnt > D_ENV(palgo)->max_try) ? h_params->cnt : D_ENV(palgo)->max_try;
        if (D_ENV(palgo)->max_try > MAX_HASH_COLLISION)
        {
            ftl_err("??? hash collision > 1024 ???\n");
        }
        hash_collision_logging(h_params->cnt, WRITE);

        bm_oob_t new_oob = *pbm->get_oob(pbm, new_pte.ppa);
        new_oob.lpa = lpa; // for data GC
        pbm->set_oob(pbm, new_pte.ppa, &new_oob);
    }

    if (unlikely(ring_count(env->wb_master_q) > 0 || env->wb_retry_q->head))
    {
        printf("[ERROR] wb_entry still remains in queues, at %s:%d\n", __FILE__,
               __LINE__);
        abort();
    }

    iter = skiplist_get_iterator(wb);
    for (size_t i = 0; i < env->max_wb_size; i++)
    {
        snode *wb_entry = skiplist_get_next(iter);
        if (wb_entry->hash_params)
            free(wb_entry->hash_params);
        free_iparams(NULL, wb_entry);
    }
    free(iter);
}

void _do_wb_flush(w_buffer_t *self, request *req)
{
    wb_env_t *env = self->env;
    skiplist **wb = &self->wb;
    flush_list *fl = env->flush_list;
    ftl_assert(fl->size > 0);
#ifndef DATA_SEGREGATION
    uint64_t maxlat = 0;
    uint32_t tt_pgs = 0;
    for (int i = 0; i < fl->size; i++)
    {
        if (fl->list[i].ppa == 0)
            continue;
        ppa_t ppa = fl->list[i].ppa;
        uint64_t lat = __demand.li->write(ppa, (uint64_t)fl->list[i].length * PAGESIZE, 0);
        maxlat = lat > maxlat ? lat : maxlat;
        tt_pgs += fl->list[i].length;
    }
    req->etime = clock_get_ns() + maxlat;

    fl->size = 0;
    memset(fl->list, 0, env->max_wb_size * sizeof(struct flush_node));
#endif

    d_htable_free(env->hash_table);
    env->hash_table = d_htable_init(env->max_wb_size * 2);

    skiplist_free(*wb);
    *wb = skiplist_init();
}