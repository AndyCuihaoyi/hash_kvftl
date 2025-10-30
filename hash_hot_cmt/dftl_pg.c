#include "dftl_pg.h"
#include "dftl_types.h"
#include "request.h"
#include "bm.h"
#include "cache.h"
#include "demand.h"
#include "dftl_cache.h"
#include "dftl_utils.h"
#include "../lower/lower.h"
#include "../tools/valueset.h"
#include "write_buffer.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

extern demand_cache *pd_cache;
extern w_buffer_t *pw_buffer;
bm_superblock_t *d_active = NULL, *d_reserve = NULL;
bm_superblock_t *t_active = NULL, *t_reserve = NULL;
#ifdef DATA_SEGREGATION
struct flush_list *fl = NULL;
#endif
static bool contains_valid_grain(block_mgr_t *bm, ppa_t ppa);
static int _do_bulk_write_valid_items(block_mgr_t *bm,
                                      gc_table_struct **bulk_table,
                                      int nr_read_pages, page_t type, bm_superblock_t *target_seg);
static int _do_bulk_mapping_update(block_mgr_t *bm, int nr_valid_items,
                                   page_t type);
static void _do_wait_until_read_all(int nr_valid_pages);

static int lpa_compare(const void *a, const void *b)
{
    lpa_t a_lpa = (*(struct gc_bucket_node **)a)->lpa;
    lpa_t b_lpa = (*(struct gc_bucket_node **)b)->lpa;

    if (a_lpa < b_lpa)
        return -1;
    else if (a_lpa == b_lpa)
        return 0;
    else
        return 1;
}

int dftl_page_init(block_mgr_t *bm)
{
    d_reserve = bm->get_active_superblock(bm, DATA_S, true);
    d_active = NULL;

    t_reserve = bm->get_active_superblock(bm, MAP_S, true);
    t_active = NULL;

    return 0;
}

#ifdef DATA_SEGREGATION
ppa_t gc_dp_alloc(block_mgr_t *bm, uint32_t idx)
{
    ppa_t ppa;
    bm_superblock_t *active = bm->get_stream_superblock(bm, idx, true);
    ppa = bm->get_page_num(bm, active);
    return ppa;
}
#endif

ppa_t dp_alloc(block_mgr_t *bm, lpa_t lpa)
{
    ppa_t ppa;
#ifdef DATA_SEGREGATION
    bm_env_t *env = bm->env;
    int idx = D_IDX % MAX_GC_STREAM;
    bm_stream_manager_t *stream = &env->stream[idx];
    if (bm->check_full(bm, stream->active_sblk))
    {
        if (bm->isgc_needed(bm, DATA_S))
        {
            int nr_valid_pages = dpage_gc_dvalue(bm, idx);
        }
    }
    d_active = bm->get_stream_superblock(bm, idx, false);
#else
    if (!d_active || bm->check_full(bm, d_active))
    {
        if (bm->isgc_needed(bm, DATA_S))
        {
            int nr_valid_pages = dpage_gc_dvalue(bm, -1);

#ifdef PRINT_GC_STATUS
            printf("DATA GC - (valid/total: %d/%d)\n", nr_valid_pages, _PPS);
#endif
        }
        else
        {

            d_active = bm->get_active_superblock(bm, DATA_S, false);
        }
    }
#endif
    ppa = bm->get_page_num(bm, d_active);
    return ppa;
}

ppa_t tp_alloc(block_mgr_t *bm)
{
    ppa_t ppa;
    if (!t_active || bm->check_full(bm, t_active))
    {
        if (bm->isgc_needed(bm, MAP_S))
        {
            int nr_valid_pages = tpage_gc(bm);
#ifdef PRINT_GC_STATUS
            printf("TRANS GC - (valid/total: %d/%d)\n", nr_valid_pages, _PPS);
#endif
        }
        else
        {
            t_active = bm->get_active_superblock(bm, MAP_S, false);
        }
    }
    ppa = bm->get_page_num(bm, t_active);
    return ppa;
}

static bool contains_valid_grain(block_mgr_t *bm, ppa_t ppa)
{
    for (int i = 0; i < GRAIN_PER_PAGE; i++)
    {
        ppa_t grain = ppa * GRAIN_PER_PAGE + i;
        if (bm->isvalid_grain(bm, grain))
            return true;
    }
    return false;
}

struct gc_bucket *gc_bucket;
struct gc_bucket_node *gcb_node_arr[_PPS * GRAIN_PER_PAGE];
static int _do_bulk_write_valid_items(block_mgr_t *bm,
                                      gc_table_struct **bulk_table,
                                      int nr_read_pages, page_t type, bm_superblock_t *target_seg)
{
    if (!nr_read_pages)
        return 0;
    gc_bucket = (struct gc_bucket *)malloc(sizeof(struct gc_bucket));
    for (int i = 0; i < PAGESIZE / GRAINED_UNIT + 1; i++)
        gc_bucket->idx[i] = 0;

    int nr_valid_grains = 0;
    int nr_valid_items = 0;
    for (int i = 0; i < nr_read_pages; i++)
    {
        for (int j = 0; j < GRAIN_PER_PAGE; j++)
        {
            pga_t pga = bulk_table[i]->ppa * GRAIN_PER_PAGE + j;
            if (bm->isvalid_grain(bm, pga))
            {
                bm_oob_t *oob = bm->get_oob(bm, pga);
                int len = oob->length;

                struct gc_bucket_node *gcb_node =
                    &gc_bucket->bucket[len][gc_bucket->idx[len]];
                // here we do not assign gcb_node->ptr since no memcpy is needed
                // gcb_node->ptr = bulk_table[i]->origin->value + j * GRAINED_UNIT;
                gcb_node->lpa = oob->lpa;
                gcb_node->len = oob->length;
                if (unlikely(oob->length == 0))
                {
                    ftl_err("pga: %d has no corresponding oob data!\n", pga);
                    abort();
                }

                gc_bucket->idx[len]++;

                gcb_node_arr[nr_valid_items++] = gcb_node;

                for (int inner_pga = pga; inner_pga < pga + len; inner_pga++)
                {
                    nr_valid_grains++;
                    bm->invalidate_grain(bm, inner_pga);
                }

                j += (len - 1);
            }
        }
        if (unlikely(contains_valid_grain(bm, bulk_table[i]->ppa)))
            abort();
    }
    int copied_pages = 0;

#ifdef DATA_SEGREGATION
    for (int i = 0; i < nr_valid_items; i++)
    {
        struct gc_bucket_node *node = gcb_node_arr[i];
        uint32_t stream_idx = target_seg->stream_idx;
        bm_stream_manager_t *stream = &bm->env->stream[stream_idx];
        if (stream->grain_remain == 0)
        {
            copied_pages++;
            stream->active_ppa = gc_dp_alloc(bm, stream_idx);
            stream->grain_remain = GRAIN_PER_PAGE;
            stream->page_remain--;
        }
        if (stream->page_remain == 0)
        {
            uint64_t lat = __demand.li->write(stream->flush_ppa, stream->flush_page * PAGESIZE, 0);
            if (lat == -1)
            {
                printf("write PPA: %d SBLK(%d), write length:%d \n", stream->flush_ppa, PPA2SBLK_IDX(stream->flush_ppa), stream->flush_page);
                // bm->show_sblk_state(bm, DATA_S);
            }
            stream->flush_ppa = stream->active_ppa;
            int page_remain = SBLK_OFFT2PPA(stream->active_sblk, SBLK_END) - stream->active_ppa;
            stream->page_remain = page_remain >= 64 ? 64 : page_remain;
            stream->flush_page = stream->page_remain;
            stream->grain_remain = GRAIN_PER_PAGE;
        }
        ppa_t ppa = stream->active_ppa;
        uint32_t offset = GRAIN_PER_PAGE - stream->grain_remain;
        int len = 1;
        stream->grain_remain -= len;
        node->ppa = PPA_TO_PGA(ppa, offset);
        for (int i = 0; i < len; i++)
        {
            bm->validate_grain(bm, node->ppa + i);
        }
        bm_oob_t new_oob = {
            .is_tpage = false,
            .lpa = node->lpa,
            .length = len,
        };
        bm->set_oob(bm, node->ppa, &new_oob); // here ppa = pga
    }

#else
    /*origin gc write*/
    uint64_t tt_pg_offset = 0;
    ppa_t sppa = -2;
    ppa_t last_ppa = -2;
    int ordering_done = 0;

    while (ordering_done < nr_valid_items)
    {
        int remain = PAGESIZE;
        // 每次GC的superblock 数据都写到reserve block上，之后再切换reserve和active
        ppa_t ppa = bm->get_page_num(bm, d_reserve);
        uint64_t offset = 0;

        ftl_assert(last_ppa == -2 || last_ppa == ppa - 1);
        last_ppa = ppa;
        if (sppa == -2)
        {
            sppa = ppa;
        }

        while (remain > 0)
        {
            int target_length = remain / GRAINED_UNIT;
            while (gc_bucket->idx[target_length] == 0 && target_length != 0)
            {
                target_length--;
            }
            if (target_length == 0)
                break;

            struct gc_bucket_node *gcb_node =
                &gc_bucket
                     ->bucket[target_length][gc_bucket->idx[target_length] - 1];
            gc_bucket->idx[target_length]--;
            gcb_node->ppa = PPA_TO_PGA(ppa, offset);

            bm_oob_t new_oob = {
                .is_tpage = false,
                .lpa = gcb_node->lpa,
                .length = target_length,
            };
            bm->set_oob(bm, gcb_node->ppa, &new_oob); // here ppa = pga
            for (int i = 0; i < target_length; i++)
            {
                bm->validate_grain(bm, gcb_node->ppa + i);
            }

            offset += target_length;
            remain -= target_length * GRAINED_UNIT;

            ordering_done++;
        }

        copied_pages++;
        tt_pg_offset++;
    }
    __demand.li->write(sppa, (uint64_t)copied_pages * PAGESIZE, 0);

#endif
    ((demand_env *)__demand.env)->num_gc_flash_write++;
    ftl_log("dGC: [valid grains: %d -> packed grains: %d], reclaimed: %d\n", nr_valid_grains,
            copied_pages * GRAIN_PER_PAGE, _PPS * GRAIN_PER_PAGE - copied_pages * GRAIN_PER_PAGE);
    return nr_valid_items;
}

static int
_do_bulk_read_pages_containing_valid_grain(block_mgr_t *bm,
                                           gc_table_struct **bulk_table,
                                           bm_superblock_t *target_seg)
{
    int i = 0;
    for (int ppa_offt = 0; ppa_offt < SBLK_END; ++ppa_offt)
    {
        ppa_t ppa = SBLK_OFFT2PPA(target_seg, ppa_offt);
        if (contains_valid_grain(bm, ppa))
        {
            __demand.li->read(ppa, PAGESIZE, 0);

            bulk_table[i] = (struct gc_table_struct *)malloc(
                sizeof(struct gc_table_struct));
            bulk_table[i]->origin = NULL;
            // bulk_table[i]->lpa = bm->get_oob(bm, ppa * GRAIN_PER_PAGE)->lpa;
            bulk_table[i]->lpa = 0; // not used
            bulk_table[i]->ppa = ppa;
            i++;
        }
    }
    int nr_read_pages = i;
    return nr_read_pages;
}

static int _do_bulk_mapping_update(block_mgr_t *bm, int nr_valid_grains,
                                   page_t type)
{
    if (!nr_valid_grains)
        return 0;
    qsort(gcb_node_arr, nr_valid_grains, sizeof(struct gc_bucket_node *),
          lpa_compare);

    bool *skip_update = (bool *)calloc(nr_valid_grains, sizeof(bool));
    bool *cmt_cnt_array = (bool *)calloc(pd_cache->env.nr_valid_tpages, sizeof(bool));
    int cmt_cnt = 0;
    /* read mapping table which needs update */
    volatile int nr_update_tpages = 0;
    for (int i = 0; i < nr_valid_grains; i++)
    {
        struct gc_bucket_node *gcb_node = gcb_node_arr[i];
        lpa_t lpa = gcb_node->lpa;
        /*For Test*/
        if (cmt_cnt_array[D_IDX] == false)
        {
            cmt_cnt_array[D_IDX] = true;
            cmt_cnt++;
        }
        if (pd_cache->is_hit(pd_cache, lpa))
        {
            struct pt_struct pte = pd_cache->get_pte(pd_cache, lpa);
            pte.ppa = gcb_node->ppa;
            pd_cache->update(pd_cache, lpa, pte);
            skip_update[i] = true;
        }
        else
        {
            struct cmt_struct *cmt = pd_cache->get_cmt(pd_cache, lpa);
            if (cmt->t_ppa == UINT32_MAX)
            {
                continue;
            }
            __demand.li->read(cmt->t_ppa, PAGESIZE, 0);
            ((demand_env *)__demand.env)->num_gc_flash_read++;
            bm->invalidate_page(bm, cmt->t_ppa);
            cmt->t_ppa = UINT32_MAX;
            nr_update_tpages++;
        }
    }
    // printf("cmt in target seg: %d\n", cmt_cnt);
    //  /* wait */
    //  while (pd_cache->member.nr_tpages_read_done != nr_update_tpages) {
    //  }    // already sync, no need
    //  pd_cache->member.nr_tpages_read_done = 0;

    /* write */
    for (int i = 0; i < nr_valid_grains; i++)
    {
        if (skip_update[i])
        {
            continue;
        }
        lpa_t lpa = gcb_node_arr[i]->lpa;
        struct cmt_struct *cmt = pd_cache->member.cmt[D_IDX];
        struct pt_struct *pt = pd_cache->member.mem_table[cmt->idx];

        pt[P_IDX].ppa = gcb_node_arr[i]->ppa;
        if (i + 1 < nr_valid_grains)
        {
            lpa = gcb_node_arr[i + 1]->lpa;
            while (D_IDX == cmt->idx)
            {
                pt[P_IDX].ppa = gcb_node_arr[i + 1]->ppa;
                i++;
                if (i + 1 >= nr_valid_grains)
                    break;
                lpa = gcb_node_arr[i + 1]->lpa;
            }
        }

        cmt->t_ppa = tp_alloc(bm);
        bm->validate_page(bm, cmt->t_ppa);
        __demand.li->write(cmt->t_ppa, PAGESIZE, 0);

        bm_oob_t new_oob = {
            .is_tpage = true, .lpa = cmt->idx, .length = PAGESIZE};
        bm->set_oob(bm, cmt->t_ppa * GRAIN_PER_PAGE, &new_oob);

        cmt->state = CLEAN;
    }

    free(skip_update);
    return 0;
}

static int _do_bulk_read_valid_tpages(block_mgr_t *bm,
                                      struct gc_table_struct **bulk_table,
                                      bm_superblock_t *target_seg)
{
    int i = 0;
    for (int ppa_offt = 0; ppa_offt < SBLK_END; ++ppa_offt)
    {
        ppa_t ppa = SBLK_OFFT2PPA(target_seg, ppa_offt);
        if (bm->isvalid_page(bm, ppa))
        {
            int res = __demand.li->read(ppa, PAGESIZE, 0);
            if (res == -1)
            {
                abort();
            }
            bm->invalidate_page(bm, ppa);

            bulk_table[i] = (struct gc_table_struct *)malloc(
                sizeof(struct gc_table_struct));
            bulk_table[i]->origin = NULL;
            bulk_table[i]->lpa = bm->get_oob(bm, ppa * GRAIN_PER_PAGE)->lpa;
            bulk_table[i]->ppa = ppa;
            i++;
        }
    }
    int nr_valid_pages = i;
    return nr_valid_pages;
}

static int _do_bulk_write_valid_tpages(block_mgr_t *bm,
                                       struct gc_table_struct **bulk_table,
                                       int nr_valid_pages)
{
    if (!nr_valid_pages)
        return 0;
    ppa_t sppa = -1;
    for (int i = 0; i < nr_valid_pages; i++)
    {
        ppa_t new_ppa = bm->get_page_num(bm, t_reserve);
        if (sppa == -1)
            sppa = new_ppa;

        bulk_table[i]->ppa = new_ppa;
        bm->validate_page(bm, new_ppa);
        bm_oob_t new_oob = {
            .is_tpage = true,
            .lpa = bulk_table[i]->lpa,
            .length = PAGESIZE,
        };
        bm->set_oob(bm, new_ppa * GRAIN_PER_PAGE, &new_oob);

        pd_cache->member.cmt[bulk_table[i]->lpa]->t_ppa = new_ppa;

        // inf_free_valueset(&bulk_table[i]->origin); // NULL
    }
    __demand.li->write(sppa, (uint64_t)nr_valid_pages * PAGESIZE, 0);

    return 0;
}

static void _do_wait_until_read_all(int nr_valid_pages)
{
    while (pd_cache->member.nr_valid_read_done != nr_valid_pages)
    {
    }
    pd_cache->member.nr_valid_read_done = 0;
}

int dpage_gc_dvalue(block_mgr_t *bm, int stream_idx)
{
    ((demand_env *)__demand.env)->num_data_gc++;
    gc_table_struct **bulk_table =
        (struct gc_table_struct **)calloc(_PPS, sizeof(gc_table_struct *));
    bm_superblock_t *target_seg = bm->get_gc_target(bm, DATA_S, stream_idx);
    if (target_seg->valid_cnt >= target_seg->wp_offt * GRAIN_PER_PAGE)
    {
        ftl_err("no invalid grain left!\n");
        abort();
    }

    int nr_read_pages =
        _do_bulk_read_pages_containing_valid_grain(bm, bulk_table, target_seg);

    // _do_wait_until_read_all(nr_read_pages); // already sync, no need

    int nr_valid_items =
        _do_bulk_write_valid_items(bm, bulk_table, nr_read_pages, DATA, target_seg);

    _do_bulk_mapping_update(bm, nr_valid_items, DATA);

    /* trim blocks on the gsegemnt */
    bm->trim_segment(bm, target_seg, __demand.li);
#ifdef DATA_SEGREGATION
    target_seg->stream_idx = -1;
    // printf("trim sblk[%d]\n", target_seg->index);
#endif

    /* reserve -> active / target_seg -> reserve */
#ifdef DATA_SEGREGATION
    target_seg->is_rsv = true;
    target_seg->is_flying = false;
#else
    bm_superblock_t *tobe_reserve = target_seg;
    d_active = d_reserve;
    d_reserve = bm->change_reserve(bm, DATA_S, tobe_reserve);
#endif
    for (int i = 0; i < nr_read_pages; i++)
    {
        free(bulk_table[i]);
    }
    free(bulk_table);

    free(gc_bucket);

    return nr_read_pages;
}

int tpage_gc(block_mgr_t *bm)
{
    struct gc_table_struct **bulk_table = (struct gc_table_struct **)calloc(
        sizeof(struct gc_table_struct *), _PPS);

    /* get the target gsegment */
    bm_superblock_t *target_seg = bm->get_gc_target(bm, MAP_S, -1);

    int nr_valid_pages = _do_bulk_read_valid_tpages(bm, bulk_table, target_seg);

    // _do_wait_until_read_all(nr_valid_pages); // already sync, no need

    _do_bulk_write_valid_tpages(bm, bulk_table, nr_valid_pages);

    /* trim blocks on the gsegemnt */
    bm->trim_segment(bm, target_seg, __demand.li);

    /* reserve -> active / target_seg -> reserve */
    bm_superblock_t *tobe_reserve = target_seg;
    t_active = t_reserve;
    t_reserve = bm->change_reserve(bm, MAP_S, tobe_reserve);

    for (int i = 0; i < nr_valid_pages; i++)
        free(bulk_table[i]);
    free(bulk_table);
    // ftl_log("tGC: [valid pages: %d], reclaimed: %d\n", nr_valid_pages, _PPS - nr_valid_pages);

    return nr_valid_pages;
}

uint32_t read_actual_dpage(block_mgr_t *bm, ppa_t ppa, request *const req)
{
    if (ppa == UINT32_MAX)
    {
        warn_notfound(__FILE__, __LINE__);
        return UINT32_MAX;
    }
    uint64_t real_ppa = G_IDX(ppa);
#ifdef DATA_SEGREGATION
    for (int i = 0; i < MAX_GC_STREAM; i++)
    {
        if (real_ppa >= bm->env->stream[i].flush_ppa && real_ppa <= bm->env->stream[i].flush_ppa + bm->env->stream[i].flush_page)
        {
            return 0;
        }
    }
#endif
    uint64_t lat = __demand.li->read(real_ppa, PAGESIZE, 0);
    req->etime = clock_get_ns() + lat;
    return 0;
}

uint32_t read_for_data_check(block_mgr_t *bm, ppa_t ppa, snode *wb_entry)
{
    uint64_t real_ppa = G_IDX(ppa);
#ifdef DATA_SEGREGATION
    for (int i = 0; i < MAX_GC_STREAM; i++)
    {
        if (real_ppa >= bm->env->stream[i].flush_ppa - 64 && real_ppa <= bm->env->stream[i].flush_ppa + bm->env->stream[i].flush_page)
        {
            return 0;
        }
    }
#endif
    uint64_t lat = __demand.li->read(real_ppa, PAGESIZE, 0);
#ifdef DATA_SEGREGATION
    if (lat == -1)
    {

        printf("FATAL: Physical read failed for PPA: %u, SBLK(%d)\n", real_ppa, PPA2SBLK_IDX(real_ppa));
        bm->show_sblk_state(bm, DATA_S);
        printf("--- System State Dump for Debugging ---\n");
        printf("The PPA was NOT in any pending flush stream. Current stream states:\n");
        for (int i = 0; i < MAX_GC_STREAM; i++)
        {
            printf("  Stream[%d]: flush_ppa range [%u, %u) with %u pages.\n",
                   i, bm->env->stream[i].flush_ppa,
                   bm->env->stream[i].flush_ppa + bm->env->stream[i].flush_page,
                   bm->env->stream[i].flush_page);
        }
        printf("---------------------------------------\n");

        abort();

        wb_entry->etime = clock_get_ns() + lat;
        return 0;
    }
#endif
}