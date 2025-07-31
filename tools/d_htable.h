/*
 * Simple hash table in Demand-based FTL
 */

#ifndef __DEMAND_HASH_TABLE_H__
#define __DEMAND_HASH_TABLE_H__

#include "../hash_baseline/dftl_utils.h"

typedef uint32_t ppa_t;
typedef uint32_t lpa_t;

struct d_hnode {
	lpa_t item;
	struct d_hnode *next;
};
typedef struct d_htable {
	struct d_hnode *bucket;
	int max;
} d_htable;

d_htable *d_htable_init(int max);
void d_htable_free(d_htable *ht);
int d_htable_insert(d_htable *ht, ppa_t ppa, lpa_t lpa);
int d_htable_find(d_htable *ht, ppa_t ppa, lpa_t lpa);

#endif
