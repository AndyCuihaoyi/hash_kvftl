#ifndef __BM_H__
#define __BM_H__

#include "dftl_utils.h"
#include "dftl_types.h"

/* Defines */
#define SBLK_IDX(x)         ((x) / _PPS)
#define INSBLK_OFFSET(x)    ((x) % _PPS)
#define SBLK_END            (_PPS)
#define SBLK_OFFT2PPA(sblk, offt)   (sblk->index * _PPS + offt)

enum{
	MAP_S = 0,
    DATA_S = 1,
};

typedef enum {
	DATA, MAP,
} page_t;

/* Structures */
typedef struct bm_oob_t {
    bool is_tpage;
    lpa_t lpa;
    uint32_t length;
} bm_oob_t;

typedef struct bm_superblock_t {    // length = segment = _PPS
    const uint32_t index;
    uint32_t valid_cnt;  // in grains
    uint32_t wp_offt;
} bm_superblock_t;

typedef struct bm_part_ext_t {
    uint32_t s_sblk;
    uint32_t e_sblk;
    uint32_t sblk_rsv;  // reserved superblock
    uint32_t free_sblk_cnt;
    uint32_t active_sblk;
} bm_part_ext_t;

typedef struct bm_env_t {
    uint64_t grain_cnt;
    bool *valid_bitmap;
    bm_oob_t *oob;

    // for superblock
    int part_num;
    bm_part_ext_t *part;
    bm_superblock_t *sblk;
} bm_env_t;

typedef struct block_mgr_t {
    bm_env_t *env;

    void (*create)(struct block_mgr_t *bm);
    void (*destroy)(struct block_mgr_t *bm);

    bool (*isvalid_grain)(struct block_mgr_t *bm, ppa_t grain);
    void (*validate_grain)(struct block_mgr_t *bm, ppa_t grain);
    void (*invalidate_grain)(struct block_mgr_t *bm, ppa_t grain);
    bool (*isvalid_page)(struct block_mgr_t *bm, ppa_t ppa);    // only used for tpages
    void (*validate_page)(struct block_mgr_t *bm, ppa_t ppa);   // only used for tpages
    void (*invalidate_page)(struct block_mgr_t *bm, ppa_t ppa); // only used for tpages
    bool (*check_full)(struct block_mgr_t *bm, bm_superblock_t *sblk);
    bool (*isgc_needed)(struct block_mgr_t *bm, int pt_num);    // parted

    void (*trim_segment)(struct block_mgr_t *bm, bm_superblock_t *sblk, lower_info *li);

    bm_superblock_t* (*change_reserve)(struct block_mgr_t *bm, int pt_num, bm_superblock_t *reserve);
    bm_superblock_t* (*get_gc_target) (struct block_mgr_t*, int pt_num);
    bm_superblock_t* (*get_active_superblock)(struct block_mgr_t *bm, int pt_num, bool isreserve);
    ppa_t (*get_page_num)(struct block_mgr_t *bm, bm_superblock_t *sblk);

    void (*set_oob)(struct block_mgr_t *bm, ppa_t grain, bm_oob_t *oob);
    bm_oob_t *(*get_oob)(struct block_mgr_t *bm, ppa_t grain) ;
} block_mgr_t;

#endif // __BM_H__