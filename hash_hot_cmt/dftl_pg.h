#ifndef __DFTL_PG_H__
#define __DFTL_PG_H__

#include "bm.h"
#include "dftl_utils.h"
#include "../tools/skiplist.h"
/* Structures */
typedef struct gc_bucket_node
{
	PTR ptr;
	int32_t lpa;
	int32_t ppa;
	int len;
} gc_bucket_node;

typedef struct gc_table_struct
{
	value_set *origin;
	lpa_t lpa;
	ppa_t ppa;
} gc_table_struct;

struct gc_bucket
{
	struct gc_bucket_node bucket[PAGESIZE / GRAINED_UNIT + 1][_PPS * GRAIN_PER_PAGE];
	uint32_t idx[PAGESIZE / GRAINED_UNIT + 1];
};

typedef ppa_t pga_t;

int dftl_page_init(block_mgr_t *bm);
ppa_t dp_alloc(block_mgr_t *bm, lpa_t lpa); // Allocate a data page. Note: the page is not validated!!!
ppa_t tp_alloc(block_mgr_t *bm);			// Allocate a translate page. Note: the page is not validated!!!
int dpage_gc_dvalue(block_mgr_t *bm, int stream_idx);
int tpage_gc(block_mgr_t *bm);
uint32_t read_actual_dpage(block_mgr_t *bm, ppa_t ppa, request *const req);
uint32_t read_for_data_check(block_mgr_t *bm, ppa_t ppa, snode *wb_entry);
#ifdef DATA_SEGREGATION
ppa_t gc_dp_alloc(block_mgr_t *bm, lpa_t lpa);
#endif
#endif // __DFTL_PG_H__