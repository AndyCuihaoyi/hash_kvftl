#ifndef __LOWER_H__
#define __LOWER_H__

#include "../hash_baseline/dftl_types.h"
#include "../hash_baseline/dftl_utils.h"
#include <stddef.h>

typedef struct lower_stats {
    uint64_t nr_nand_write;
    uint64_t nr_nand_read;
    uint64_t nr_nand_erase;
    uint64_t *nr_nand_rd_lun;
    uint64_t *nr_nand_wr_lun;
    uint64_t *nr_nand_er_lun;
} lower_stats;

typedef struct lower_info {
    void (*create)(lower_info *);
    void (*destroy)(lower_info *);
    uint64_t (*write)(uint32_t ppa, uint64_t size, uint64_t stime);
    uint64_t (*read)(uint32_t ppa, uint64_t size,  uint64_t stime);
    uint64_t (*trim_block)(uint32_t ppa);

    /* for statistics */
    lower_stats *stats;
} lower_info;

#endif // __LOWER_H__