#ifndef DFTL_TYPES_H
#define DFTL_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "dftl_settings.h"
#include "../tools/rte_ring/rte_ring.h"
#include <pthread.h>
#include "algo_queue.h"

typedef struct queue queue;

typedef uint32_t ppa_t;
typedef uint32_t lpa_t;
typedef uint32_t fp_t;

typedef struct snode snode;

typedef struct str_key {
    uint8_t len;
    char key[MAXKEYSIZE];
} str_key;

#define KEYT str_key
#define PTR char *

typedef struct value_set {
    PTR value;
    uint32_t length;
    uint32_t ppa;
    uint32_t length_in_bytes;
    uint32_t offset;
} value_set;

typedef struct lower_info lower_info;

typedef struct hash_params {
    uint32_t hash;
#ifdef STORE_KEY_FP
    fp_t key_fp;
#endif

    int cnt;
    int find;
} hash_params;

typedef enum {
	GOTO_LOAD, GOTO_LIST, GOTO_EVICT,
	GOTO_COMPLETE, GOTO_READ, GOTO_WRITE,
	GOTO_UPDATE,
} jump_t;

typedef struct inflight_params {
    jump_t jump;
} inflight_params;

typedef enum req_state_t {
    ALGO_REQ_PENDING = 0,
    ALGO_REQ_NOT_FOUND = 1,
} req_state_t;

enum inner_time_t {
    SQ_SUBMIT = 0,
    META_LI_RD = 1,
    META_LI_WR = 2,
    DATA_LI_RD = 3,
    DATA_LI_WR = 4,
    CQ_COMPLETE = 5,
    REQ_TT_LAT = 6,

    INNER_TIMER_SIZE,
};

struct req_inner_timer {
    bool is_start;
    uint64_t start;
    uint64_t elapsed;
};

typedef struct request {
    uint8_t type;   // in request.h
    KEYT key;
    value_set *value;
    hash_params *h_params;
    req_state_t state;
    uint64_t stime;
    uint64_t etime;
    struct req_inner_timer inner_timer[INNER_TIMER_SIZE];
    pthread_spinlock_t timer_lock;

    inflight_params *params;

    volatile int *ptr_nr_ios;    // for iodepth control

    void *(*end_req)(struct request *const);
} request;

typedef struct demand_params {
    value_set *value;
    snode *wb_entry;
    int offset;
} demand_params;

typedef struct algorithm {
    /*interface*/
    uint32_t (*argument_set)(int argc, char **argv);
    uint32_t (*create)(struct algorithm *, lower_info *);
    void (*destroy)(struct algorithm *, lower_info *);
    uint32_t (*read)(struct algorithm *,request *const);
    uint32_t (*write)(struct algorithm *,request *const);
    uint32_t (*remove)(struct algorithm *,request *const);
#ifdef KVSSD
    uint32_t (*iter_create)(struct algorithm *, request *const);
    uint32_t (*iter_next)(struct algorithm *, request *const);
    uint32_t (*iter_next_with_value)(struct algorithm *, request *const);
    uint32_t (*iter_release)(struct algorithm *, request *const);
    uint32_t (*iter_all_key)(struct algorithm *, request *const);
    uint32_t (*iter_all_value)(struct algorithm *, request *const);
    uint32_t (*multi_set)(struct algorithm *, request *const, int num);
    uint32_t (*multi_get)(struct algorithm *, request *const, int num);
    uint32_t (*range_query)(struct algorithm *, request *const);
#endif
	struct rte_ring *req_q; //for write req in priority
	algo_q *retry_q;
    struct rte_ring *finish_q;
    lower_info *li;

    void *env;
} algorithm;

#endif // DFTL_TYPES_H