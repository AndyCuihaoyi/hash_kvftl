#ifndef __DFTL_BM_H__
#define __DFTL_BM_H__

#include "bm.h"

void dftl_bm_init(block_mgr_t *self);
void dftl_bm_free(block_mgr_t *self);
bm_oob_t *dftl_bm_get_oob(block_mgr_t *self, ppa_t grain);
void dftl_bm_set_oob(block_mgr_t *self, ppa_t grain, bm_oob_t *oob);
bool dftl_check_full(block_mgr_t *self, bm_superblock_t *sblk);
ppa_t dftl_get_page_num(block_mgr_t *self, bm_superblock_t *sblk);
bm_superblock_t *dftl_get_active_sblk(block_mgr_t *self, int pt_num,
                                      bool isreserve);
bool dftl_isgc_needed(block_mgr_t *self, int pt_num);
bool dftl_isvalid_grain(block_mgr_t *self, ppa_t grain);
void dftl_validate_grain(block_mgr_t *self, ppa_t grain);
void dftl_invalidate_grain(block_mgr_t *self, ppa_t grain);
bool dftl_isvalid_page(block_mgr_t *self, ppa_t ppa);
void dftl_validate_page(block_mgr_t *self, ppa_t ppa);
void dftl_invalidate_page(block_mgr_t *self, ppa_t ppa);
bm_superblock_t *dftl_get_gc_target(block_mgr_t *self, int pt_num);
void dftl_trim_segment(block_mgr_t *self, bm_superblock_t *sblk, lower_info *li);
bm_superblock_t *dftl_change_reserve(block_mgr_t *self, int pt_num, bm_superblock_t *reserve);


#endif // __DFTL_BM_H__