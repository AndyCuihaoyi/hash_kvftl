#ifndef __BLOOM_H__
#define __BLOOM_H__
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../hash_kvftl/dftl_types.h"
typedef struct
{
    int nr_hash;
    uint64_t m;
    uint32_t targetsize;
    int nr_entry;
    float fpr;
    char *body;
} BF;

BF *bf_init(int entry, float fpr);
float bf_fpr_from_memory(int entry, uint32_t memory);
void bf_free(BF *);
uint64_t bf_bits(int entry, float fpr);
void bf_set(BF *, lpa_t);
BF *bf_cpy(BF *src);
bool bf_check(BF *, lpa_t);
void bf_reset(BF *bf);
#endif