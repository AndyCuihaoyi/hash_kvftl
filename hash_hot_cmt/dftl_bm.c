#include "dftl_bm.h"
#include "demand.h"
#include "dftl_types.h"
#include "../lower/lower.h"
#include "dftl_utils.h"
#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
bm_env_t bm_env = {
    .grain_cnt = GRAIN_PER_PAGE * _NOP, .valid_bitmap = NULL, .oob = NULL};
#ifdef DATA_SEGREGATION
bm_superblock_t *dftl_get_stream_sblk(block_mgr_t *self, lpa_t lpa);
#endif
block_mgr_t bm = {
    .env = &bm_env,
    .create = dftl_bm_init,
    .destroy = dftl_bm_free,
    .isvalid_page = dftl_isvalid_page,
    .validate_page = dftl_validate_page,
    .invalidate_page = dftl_invalidate_page,
    .isvalid_grain = dftl_isvalid_grain,
    .validate_grain = dftl_validate_grain,
    .invalidate_grain = dftl_invalidate_grain,
    .isgc_needed = dftl_isgc_needed,
    .check_full = dftl_check_full,
    .get_page_num = dftl_get_page_num,
    .get_active_superblock = dftl_get_active_sblk,
    .get_gc_target = dftl_get_gc_target,
    .trim_segment = dftl_trim_segment,
    .change_reserve = dftl_change_reserve,
    .get_oob = dftl_bm_get_oob,
    .set_oob = dftl_bm_set_oob,
#ifdef DATA_SEGREGATION
    .get_stream_superblock = dftl_get_stream_sblk,
#endif
};
block_mgr_t *pbm = &bm;

void dftl_bm_init(block_mgr_t *self)
{
    bm_env_t *env = self->env;
    env->valid_bitmap = (bool *)g_malloc0(sizeof(bool) * env->grain_cnt);
    env->oob = (bm_oob_t *)g_malloc0(sizeof(bm_oob_t) * env->grain_cnt);
    env->sblk = (bm_superblock_t *)g_malloc0(sizeof(bm_superblock_t) * _NOS);
    for (int i = 0; i < _NOS; i++)
    {
        *(uint32_t *)(&(env->sblk[i])) = i; // sblk->index = i
#ifdef DATA_SEGREGATION
        env->sblk[i].is_flying = false;
        env->sblk[i].stream_idx = -1;
#endif
    }
    env->part_num = 2;
    env->part =
        (bm_part_ext_t *)g_malloc0(sizeof(bm_part_ext_t) * env->part_num);
    env->part[0].s_sblk = 0;
    env->part[0].e_sblk = _NOS / 20;
    env->part[0].free_sblk_cnt = _NOS / 20 + 1;
    env->part[0].sblk_rsv = 0;
    env->part[0].active_sblk = -1;
    env->part[1].s_sblk = _NOS / 20 + 1;
    env->part[1].e_sblk = _NOS - 1;
    env->part[1].free_sblk_cnt = _NOS - (_NOS / 20 + 1);
    env->part[1].sblk_rsv = _NOS / 20 + 1;
    env->part[1].active_sblk = -1;
#ifdef DATA_SEGREGATION
    env->stream = (bm_stream_manager_t *)g_malloc0(sizeof(bm_stream_manager_t) * MAX_GC_STREAM);
    for (int i = 0; i < MAX_GC_STREAM; i++)
    {
        env->stream[i].idx = i;
        env->stream[i].active_sblk = self->get_active_superblock(pbm, DATA_S, FALSE);
        env->stream[i].active_sblk->stream_idx = i;
        env->stream[i].active_ppa = self->get_page_num(self, env->stream[i].active_sblk);
        env->stream[i].flush_ppa = env->stream[i].active_ppa;
        int page_remain = SBLK_OFFT2PPA(env->stream[i].active_sblk, SBLK_END) - env->stream[i].active_ppa;
        env->stream[i].grain_remain = GRAIN_PER_PAGE;
        env->stream[i].page_remain = page_remain >= 64 ? 64 : page_remain;
        env->stream[i].flush_page = env->stream[i].page_remain;

        // printf("stream idx: %d , sblk idx: %d , sblk state: %d\n", env->stream[i].idx = i, env->stream[i].active_sblk->index, env->stream[i].active_sblk->is_flying);
    }
#endif
}

void dftl_bm_free(block_mgr_t *self)
{
    bm_env_t *env = self->env;
    g_free(env->oob);
    g_free(env->valid_bitmap);
}

bool dftl_isvalid_page(block_mgr_t *self, ppa_t ppa)
{
    bm_env_t *env = self->env;
    bool state = env->valid_bitmap[ppa * GRAIN_PER_PAGE];
    for (int i = 0; i < GRAIN_PER_PAGE; i++)
    {
        ftl_assert(state == env->valid_bitmap[ppa * GRAIN_PER_PAGE + i]);
    }
    return state;
}

void dftl_validate_page(block_mgr_t *self, ppa_t ppa)
{
    bm_env_t *env = self->env;
    for (int i = 0; i < GRAIN_PER_PAGE; i++)
    {
        ftl_assert(env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] == false);
        env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] = true;
        env->sblk[SBLK_IDX(ppa)].valid_cnt++;
    }
}

void dftl_invalidate_page(block_mgr_t *self, ppa_t ppa)
{
    bm_env_t *env = self->env;
    for (int i = 0; i < GRAIN_PER_PAGE; i++)
    {
        ftl_assert(env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] == true);
        env->valid_bitmap[ppa * GRAIN_PER_PAGE + i] = false;
        env->sblk[SBLK_IDX(ppa)].valid_cnt--;
    }
}

bool dftl_isvalid_grain(block_mgr_t *self, ppa_t grain)
{
    bm_env_t *env = self->env;
    return env->valid_bitmap[grain];
}

void dftl_validate_grain(block_mgr_t *self, ppa_t grain)
{
    bm_env_t *env = self->env;
    ftl_assert(env->valid_bitmap[grain] == false);
    env->valid_bitmap[grain] = true;
    env->sblk[SBLK_IDX(grain / GRAIN_PER_PAGE)].valid_cnt++;
}

void dftl_invalidate_grain(block_mgr_t *self, ppa_t grain)
{
    bm_env_t *env = self->env;
    ftl_assert(env->valid_bitmap[grain] == true);
    env->valid_bitmap[grain] = false;
    env->sblk[SBLK_IDX(grain / GRAIN_PER_PAGE)].valid_cnt--;
}

bm_oob_t *dftl_bm_get_oob(block_mgr_t *self, ppa_t grain)
{
    bm_env_t *env = self->env;
    return &env->oob[grain];
}

void dftl_bm_set_oob(block_mgr_t *self, ppa_t grain, bm_oob_t *oob)
{
    bm_env_t *env = self->env;
    env->oob[grain] = *oob;
}

bool dftl_check_full(block_mgr_t *self, bm_superblock_t *sblk)
{
    ftl_assert(sblk != NULL);
    return sblk->wp_offt >= SBLK_END;
}

ppa_t dftl_get_page_num(block_mgr_t *self, bm_superblock_t *sblk)
{
    ftl_assert(!bm->check_full(bm, sblk));
    return SBLK_OFFT2PPA(sblk, sblk->wp_offt++);
}

bm_superblock_t *dftl_get_active_sblk(block_mgr_t *self, int pt_num, bool isreserve)
{
    bm_env_t *env = self->env;
    ftl_assert(0 <= pt_num && pt_num < env->part_num);
    bm_part_ext_t *pt = &env->part[pt_num];
    if (isreserve)
    {
        ftl_assert(env->sblk[pt->sblk_rsv].wp_offt == 0 &&
                   env->sblk[pt->sblk_rsv].valid_cnt == 0);
        return &env->sblk[pt->sblk_rsv];
    }
    if (pt->active_sblk == -1)
    {
        for (int i = pt->s_sblk; i <= pt->e_sblk; i++)
        {
            if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]))
            {
                pt->active_sblk = i;
                break;
            }
        }
    }
    if (pt_num == DATA_S)
    {
        uint32_t start_sblk = pt->active_sblk;
        pt->active_sblk = -1;
        // 搜索active sblk后面的空闲superblock
        for (int i = start_sblk + 1; i <= pt->e_sblk; i++)
        {
#ifdef DATA_SEGREGATION
            if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]) && env->sblk[i].is_flying == false)
#else

            if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]))
#endif
            {
                pt->active_sblk = i;
                break;
            }
        }
        // 没找到时，从头搜索
        if (pt->active_sblk == -1)
        {
            for (int i = pt->s_sblk; i < start_sblk; i++)
            {
#ifdef DATA_SEGREGATION
                if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]) && env->sblk[i].is_flying == false)
#else
                if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]))
#endif
                {
                    pt->active_sblk = i;
                    break;
                }
            }
        }
#ifdef DATA_SEGREGATION
        env->sblk[pt->active_sblk].is_flying = true;
#endif
    }
    else
    {
        uint32_t start_sblk = pt->active_sblk;
        pt->active_sblk = -1;
        // 搜索active sblk后面的空闲superblock
        for (int i = start_sblk + 1; i <= pt->e_sblk; i++)
        {
            if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]))
            {
                pt->active_sblk = i;
                break;
            }
        }
        // 没找到时，从头搜索
        if (pt->active_sblk == -1)
        {
            for (int i = pt->s_sblk; i < start_sblk; i++)
            {
                if (i != pt->sblk_rsv && !self->check_full(self, &env->sblk[i]))
                {
                    pt->active_sblk = i;
                    break;
                }
            }
        }
    }
    if (pt->active_sblk == -1)
    {
        return NULL;
    }
    if (env->sblk[pt->active_sblk].wp_offt == 0)
    {
        pt->free_sblk_cnt--;
    }

    return &env->sblk[pt->active_sblk];
}

#ifdef DATA_SEGREGATION
bm_superblock_t *dftl_get_stream_sblk(block_mgr_t *self, lpa_t lpa)
{
    bm_env_t *env = self->env;
    int idx = lpa % MAX_GC_STREAM;
    bm_stream_manager_t *stream = &env->stream[idx];
    if (self->check_full(self, stream->active_sblk))
    {
        stream->active_sblk->is_flying = false;
        env->stream[idx].active_sblk = self->get_active_superblock(self, DATA_S, FALSE);
        stream = &env->stream[idx];
        stream->active_sblk->stream_idx = idx;
    }
    return stream->active_sblk;
}
#endif

bool dftl_isgc_needed(block_mgr_t *self, int pt_num)
{
    bm_env_t *env = self->env;
    ftl_assert(0 <= pt_num && pt_num < env->part_num);
    bm_part_ext_t *pt = &env->part[pt_num];
    // #ifdef DATA_SEGREGATION
    //     if (pt_num == DATA_S)
    //         return (pt->free_sblk_cnt <= MAX_GC_STREAM) ? 1 : 0;
    // #endif
    return (pt->free_sblk_cnt <= 1) ? 1 : 0; // reserved 1
}

bm_superblock_t *dftl_get_gc_target(block_mgr_t *self, int pt_num)
{
    bm_env_t *env = self->env;
    ftl_assert(0 <= pt_num && pt_num < env->part_num);
    bm_part_ext_t *pt = &env->part[pt_num];
    bm_superblock_t *target = NULL;
    uint32_t min_valid_cnt = UINT32_MAX;
    for (int i = pt->s_sblk; i <= pt->e_sblk; i++)
    {
#ifdef DATA_SEGREGATION
        if (i != pt->sblk_rsv && env->sblk[i].valid_cnt < min_valid_cnt)
        {
            // if (env->sblk[i].is_flying == true && pt_num == DATA_S)
            // {
            //     env->sblk[i].is_flying = false;
            //     env->stream[env->sblk[i].stream_idx].active_sblk = self->get_active_superblock(self, DATA_S, FALSE);
            //     env->stream[env->sblk[i].stream_idx].active_sblk->stream_idx = env->sblk[i].stream_idx;
            //     env->sblk[i].stream_idx = -1;
            // }
            if (self->check_full(self, &env->sblk[i]) && pt_num == MAP_S)
            {
                target = &env->sblk[i];
                min_valid_cnt = env->sblk[i].valid_cnt;
            }
            else if (pt_num == DATA_S)
            {
                target = &env->sblk[i];
                min_valid_cnt = env->sblk[i].valid_cnt;
            }
        }
#else
        if (i != pt->sblk_rsv && self->check_full(self, &env->sblk[i]) && env->sblk[i].valid_cnt < min_valid_cnt)
        {
            target = &env->sblk[i];
            min_valid_cnt = env->sblk[i].valid_cnt;
        }
#endif
    }
    if (min_valid_cnt == UINT32_MAX)
    {
        ftl_err("No valid superblock for GC\n");
#ifdef DATA_SEGREGATION
        for (int i = pt->s_sblk; i <= pt->e_sblk; i++)
        {

            // 打印包含 is_flying 状态的完整信息
            printf("sblk[%d]: is_rsv=%d, is_full=%d, valid_cnt=%u,sblk wp:%d, is_flying=%s\n",
                   i,
                   (i == pt->sblk_rsv), // 是否是预留块
                   self->check_full(self, &env->sblk[i]),
                   env->sblk[i].valid_cnt,
                   env->sblk[i].wp_offt,
                   env->sblk[i].is_flying ? "true" : "false"); // is_flying 状态
        }
#endif
        abort();
    }
    return target;
}

void dftl_trim_segment(block_mgr_t *self, bm_superblock_t *sblk, lower_info *li)
{
    ftl_assert(sblk != NULL);
    li->trim_block(SBLK_OFFT2PPA(sblk, 0));
    sblk->wp_offt = 0;
    sblk->valid_cnt = 0;
    memset(&self->env->oob[(uint64_t)SBLK_OFFT2PPA(sblk, 0) * GRAIN_PER_PAGE], 0, _PPS);
#ifdef DEBUG_FTL
    for (int i = 0; i < SBLK_END; i++)
    {
        ftl_assert(!bm->isvalid_page(bm, SBLK_OFFT2PPA(sblk, i)));
    }
#endif
}

bm_superblock_t *dftl_change_reserve(block_mgr_t *self, int pt_num, bm_superblock_t *reserve)
{
    bm_env_t *env = self->env;
    ftl_assert(0 <= pt_num && pt_num < env->part_num);
    bm_part_ext_t *pt = &env->part[pt_num];
    if (reserve != NULL)
    {
        pt->sblk_rsv = reserve->index;
    }
    else
    {
        pt->sblk_rsv = -1;
        ftl_err("No reserve superblock\n");
        abort();
    }
    return reserve;
}