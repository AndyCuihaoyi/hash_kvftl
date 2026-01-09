#ifndef __SKIPLIST_HEADER
#define __SKIPLIST_HEADER
#include "../hash_hot_cmt/dftl_types.h"
#include "../hash_hot_cmt/dftl_utils.h"
#include <stdbool.h>
#include <stdint.h>

#define S_KEYT KEYT

#define MAX_L 30 // max level number
#define PROB 4   // the probaility of level increasing : 1/PROB => 1/4
#define for_each_sk(node, skip)                              \
    for (node = skip->header->list[1]; node != skip->header; \
         node = node->list[1])

#define for_each_sk_from(node, from, skip) \
    for (node = from; node != skip->header; node = node->list[1])

#define SKIPISHEADER(a, b) (a)->header == b ? 1 : 0

typedef struct snode
{ // skiplist's node
    ppa_t ppa;
    S_KEYT key;
    uint64_t etime; // for mapping_update
    uint32_t level;
    value_set *value;
    bool isvalid;
    uint32_t lpa;
    void *hash_params;
    void *params;

    struct snode **list;
    struct snode *back;
} snode;

// #ifdef Lsmtree
typedef struct length_bucket
{
    // snode *bucket[PAGESIZE/PIECE+1][2048];
    snode **bucket[NPCINPAGE + 1];
    uint32_t idx[NPCINPAGE + 1];
    value_set **contents;
    int contents_num;
} l_bucket;
// #endif

typedef struct skiplist
{
    uint8_t level;
    uint64_t size;
#if defined(KVSSD)
    uint32_t all_length;
#endif
    uint32_t data_size;
    snode *header;
} skiplist;

// read only iterator. don't using iterater after delete iter's now node
typedef struct
{
    skiplist *list;
    snode *now;
} sk_iter;

skiplist *skiplist_init(); // return initialized skiplist*
snode *skiplist_find(
    skiplist *,
    S_KEYT); // find snode having key in skiplist, return NULL:no snode
snode *skiplist_find_lowerbound(skiplist *, S_KEYT);
snode *skiplist_insert(skiplist *, S_KEYT, value_set *,
                       bool); // insert skiplist, return inserted snode
snode *skiplist_insert_iter(skiplist *, S_KEYT lpa, ppa_t ppa);
snode *skiplist_at(skiplist *, int idx);
int skiplist_delete(
    skiplist *, S_KEYT);            // delete by key, return 0:normal -1:empty -2:no key
void skiplist_free(skiplist *list); // free skiplist
void skiplist_clear(
    skiplist *list);                            // clear all snode in skiplist and  reinit skiplist
sk_iter *skiplist_get_iterator(skiplist *list); // get read only iterator
snode *skiplist_get_next(sk_iter *iter);        // get next snode by iterator

#ifdef DVALUE
int bucket_page_cnt(l_bucket *);
#endif
#endif
