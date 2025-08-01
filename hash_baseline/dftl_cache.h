#ifndef __DFTL_CACHE_H__
#define __DFTL_CACHE_H__

#include "cache.h"

extern demand_cache d_cache;

int dftl_cache_create(demand_cache *self);
int dftl_cache_destroy(demand_cache *self);
int dftl_cache_load(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
int dftl_cache_list_up(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
int dftl_cache_wait_if_flying(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
int dftl_cache_touch(demand_cache *self, lpa_t lpa);
int dftl_cache_update(demand_cache *self, lpa_t lpa, struct pt_struct pte);
struct pt_struct dftl_cache_get_pte(demand_cache *self, lpa_t lpa);
struct cmt_struct* dftl_cache_get_cmt(demand_cache *self, lpa_t lpa);
bool dftl_cache_is_hit(demand_cache *self, lpa_t lpa);
bool dftl_cache_is_full(demand_cache *self);


#endif // __DFTL_CACHE_H__