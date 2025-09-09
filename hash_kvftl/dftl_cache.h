#ifndef __DFTL_CACHE_H__
#define __DFTL_CACHE_H__

#include "cache.h"

extern demand_cache d_cache;

int dftl_cache_create(demand_cache *self);
int dftl_cache_destroy(demand_cache *self);
int dftl_cache_load(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, bool is_hot);
int dftl_cache_list_up(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
int dftl_cache_wait_if_flying(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);
int dftl_cache_touch(demand_cache *self, lpa_t lpa);
int dftl_cache_update(demand_cache *self, lpa_t lpa, struct pt_struct pte);
struct pt_struct dftl_cache_get_pte(demand_cache *self, lpa_t lpa);
struct cmt_struct *dftl_cache_get_cmt(demand_cache *self, lpa_t lpa);
bool dftl_cache_is_hit(demand_cache *self, lpa_t lpa);
int dftl_cache_upgrade_hot(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry, struct cmt_struct *victim);
uint32_t dftl_cache_is_full(demand_cache *self, bool is_hot);
int dftl_cache_hot_is_hit(demand_cache *self, lpa_t lpa, struct pt_struct *pte);
int dftl_cache_hot_evict(demand_cache *self, lpa_t lpa, request *const req, snode *wb_entry);

#endif // __DFTL_CACHE_H__