#ifndef __ALGO_REQ_H__
#define __ALGO_REQ_H__

#include "dftl_types.h"
#include "../tools/skiplist.h"
#include <stdint.h>

#define LREQ_TYPE_NUM 12
// algo_req types
#define TRIM 0
#define MAPPINGR 1
#define MAPPINGW 2
#define GCMR 3
#define GCMW 4
#define DATAR 5
#define DATAW 6
#define GCDR 7
#define GCDW 8
#define GCMR_DGC 9
#define GCMW_DGC 10
#define PREFILL_DATAR 11
// end algo_req types

uint32_t hashing_key(char *key, uint8_t len);
fp_t hashing_key_fp(char *key, uint8_t len);
hash_params *make_hash_params(request *const req);
inflight_params *get_iparams(request *const req, snode *wb_entry);
void free_iparams(request *const req, snode *wb_entry);

#endif // __ALGO_REQ_H__