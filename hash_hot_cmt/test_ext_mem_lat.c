#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/prctl.h>
#include "algo_queue.h"
#include "cache.h"
#include "demand.h"
#include "dftl_cache.h"
#include "dftl_wb.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "dftl_pg.h"
#include "request.h"
#include "../tools/rte_ring/rte_ring.h"
#include "../tools/valueset.h"
#include "../lower/lower.h"
#include <glib-2.0/glib.h>
#include <unistd.h>
#include "../tools/random/zipf.h"
#include <getopt.h>
#define NUM_ITEMS (44000000)
#define NUM_UPDATES (44000000)
#define NUM_WORKERS (8)
volatile bool thread_ended[NUM_WORKERS] = {false};
bool full_worker = true;
// #define VALUE_CHECK

extern lower_info ssd_li;
int iodepth = 64;
pthread_spinlock_t global_inner_timer_lock;
struct req_inner_timer global_inner_timer[INNER_TIMER_SIZE] = {0};

void submit_req(algorithm *palgo, request *req);

static uint64_t finished_r = 0;
static uint64_t finished_w = 0;
static uint64_t wrong_value_cnt = 0;
#ifdef VALUE_CHECK
char *values[NUM_ITEMS] = {0};
#endif
#define BATCH_SIZE (200000)
#define MAX_LATENCY_US 100000000 // 10s

static uint64_t wlat_arr[MAX_LATENCY_US] = {0};
static uint64_t rlat_arr[MAX_LATENCY_US] = {0};
static uint64_t pt_wlat_arr = 0, pt_rlat_arr = 0;

static uint64_t rd_start_ns = 0;
static uint64_t wr_start_ns = 0;

static uint64_t complete_r_slow = 0;
static uint64_t complete_w_slow = 0;

static double read_iops = 0;
static double write_iops = 0;
static uint64_t read_batch_cnt = 0;
static uint64_t write_batch_cnt = 0;

void *end_request(request *req)
{
    // req->etime = clock_get_ns();
    switch (req->type)
    {
    case DATAR:
        if (req->state == ALGO_REQ_NOT_FOUND)
        {
            ftl_log("%lu th read request end. key: %s, value not found\n", ++finished_r, req->key.key);
        }
        else
        {
            // finished_r++;
            uint64_t finished_r_local = g_atomic_pointer_add(&finished_r, 1);
            static uint64_t sum_rlat_ns = 0;
            uint64_t sum_rlat_ns_local = g_atomic_pointer_add(&sum_rlat_ns, (req->etime - req->stime));
            if ((++finished_r_local) % BATCH_SIZE == 0)
            {
                uint64_t now = clock_get_ns();
                double elapsed = (now - rd_start_ns) / 1e9;
                if (full_worker)
                {
                    for (int i = 0; i < NUM_WORKERS; ++i)
                    {
                        if (thread_ended[i])
                        {
                            ftl_log("Thread %d ended !\n", i);
                            full_worker = false;
                        }
                    }
                }
                if (full_worker)
                {
                    read_iops += (double)BATCH_SIZE / elapsed;
                    read_batch_cnt++;
                }
                rd_start_ns = now;
                ftl_log("%lu th read request end. key: %s, iops: %lf, avg_lat: %ld, slow: %ld\n", finished_r_local, req->key.key, (double)BATCH_SIZE / elapsed, sum_rlat_ns_local / BATCH_SIZE, complete_r_slow);
                g_atomic_pointer_set(&sum_rlat_ns, 0);
                complete_r_slow = 0;
                fflush(stdout);
                fflush(stderr);
            }
            uint64_t rlat_us = (req->etime - req->stime) / 1000;
            g_atomic_pointer_add(&rlat_arr[rlat_us], 1);
            if (rlat_us > pt_rlat_arr)
            {
                g_atomic_pointer_set(&pt_rlat_arr, rlat_us);
            }
        }
        if (req->value)
        {
            inf_free_valueset(&req->value);
        }
        break;
    case DATAW:
    {
        uint64_t finished_w_local = g_atomic_pointer_add(&finished_w, 1);
        static uint64_t sum_wlat_ns = 0;
        uint64_t latency = req->etime - req->stime;
        uint64_t sum_wlat_ns_local = __atomic_add_fetch(&sum_wlat_ns, latency, __ATOMIC_RELAXED);
        if ((++finished_w_local) % BATCH_SIZE == 0)
        {
            // ftl_log("\033[1;32m%lu\033[0m th write request end.\n", finished_w);
            uint64_t now = clock_get_ns();
            double elapsed = (now - wr_start_ns) / 1e9;
            write_iops += (double)BATCH_SIZE / elapsed;
            write_batch_cnt++;
            wr_start_ns = now;
            ftl_log("%lu th write request end, iops: %lf, avg_lat: %ld, slow: %ld\n", finished_w_local, (double)BATCH_SIZE / elapsed, sum_wlat_ns_local / BATCH_SIZE, complete_w_slow);
            g_atomic_pointer_set(&sum_wlat_ns, 0);
            complete_w_slow = 0;
            fflush(stdout);
            fflush(stderr);
        }
        uint64_t wlat_us = (req->etime - req->stime) / 1000;
        g_atomic_pointer_add(&wlat_arr[wlat_us], 1);
        if (wlat_us > pt_wlat_arr)
        {
            g_atomic_pointer_set(&pt_wlat_arr, wlat_us);
        }
    }
    break;
    }
    for (int i = 0; i < INNER_TIMER_SIZE; ++i)
    {
        if (req->inner_timer[i].elapsed)
        {
            pthread_spin_lock(&global_inner_timer_lock);
            global_inner_timer[i].elapsed += req->inner_timer[i].elapsed;
            pthread_spin_unlock(&global_inner_timer_lock);
        }
    }
    pthread_spin_destroy(&req->timer_lock);
    g_atomic_int_add(req->ptr_nr_ios, -1);
    free(req);
    return NULL;
}

pthread_spinlock_t submit_lock;

void *algo_thread()
{
    prctl(PR_SET_NAME, "algo_thread");
    algorithm *palgo = &__demand;
    pthread_spin_init(&submit_lock, PTHREAD_PROCESS_PRIVATE);
    while (1)
    {
        request *req = NULL;
        uint64_t now = clock_get_ns();
        while (palgo->retry_q->head != NULL && ((request *)palgo->retry_q->head->payload)->etime <= now)
        {
            req = algo_q_dequeue(palgo->retry_q);
            switch (req->type)
            {
            case DATAR:
                palgo->read(palgo, req);
                break;
            case DATAW:
                palgo->write(palgo, req);
                break;
            default:
                break;
            }
            now = clock_get_ns();
        }
        if (ring_count(palgo->req_q) > 0)
        {
            ring_dequeue(palgo->req_q, (void *)&req, 1);
            req->etime = clock_get_ns();
            switch (req->type)
            {
            case DATAR:
                palgo->read(palgo, req);
                break;
            case DATAW:
                palgo->write(palgo, req);
                break;
            default:
                break;
            }
        }
    }
}

void check_dcache_queue()
{
    demand_cache *pd_cache = &d_cache;
    struct cmt_struct **cmt = pd_cache->member.cmt;
    for (int i = 0; i < d_cache.env.nr_valid_tpages; ++i)
    {
        int retry_cnt = cmt[i]->retry_q->size;
        int wait_cnt = cmt[i]->wait_q->size;
        if (retry_cnt || wait_cnt)
        {
            ftl_err("cmt[%d]: retry: %d, wait: %d\n", i, retry_cnt, wait_cnt);
        }
    }
    ftl_log("cmt check finished.\n");
    return;
}

pthread_t algo_tr;
pthread_t finish_tr[2];

void *process_cq_cpl()
{
    prctl(PR_SET_NAME, "cq_cpl_thread");
    algorithm *palgo = &__demand;
    request *req = NULL;
    algo_q *complete_q = algo_q_create();
    while (1)
    {
        uint64_t now = clock_get_ns();
        if (ring_dequeue(palgo->finish_q, (void *)&req, 1))
        {
            if (req->etime <= now)
            {
                req->end_req(req);
            }
            else
            {
                algo_q_insert_sorted(complete_q, req, NULL);
            }
        }
        if (complete_q->head)
        {
            uint64_t now = clock_get_ns();
        retry:
            req = (request *)complete_q->head->payload;
            if (req->etime <= now)
            {
                if (now - req->etime > 10000)
                {
                    switch (req->type)
                    {
                    case DATAR:
                        complete_r_slow++;
                        break;
                    case DATAW:
                        complete_w_slow++;
                        break;
                    default:
                        break;
                    }
                }
                algo_q_dequeue(complete_q);
                req->end_req(req);
                if (complete_q->head)
                {
                    goto retry;
                }
            }
        }
    }
    return NULL;
}

void test_insert_read(algorithm *palgo)
{
    for (uint64_t i = 1; i < NUM_ITEMS; ++i)
    {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);
#ifdef VALUE_CHECK
        if (!values[i])
        {
            values[i] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[i], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        memcpy(w_req->key.key, key, keylen);
        w_req->key.len = keylen;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        submit_req(palgo, w_req);
    }
    for (uint64_t i = 1; i < NUM_ITEMS; ++i)
    {
        value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);
        request *r_req = g_malloc0(sizeof(request));
        r_req->type = DATAR;
        memcpy(r_req->key.key, key, keylen);
        r_req->key.len = keylen;
        r_req->h_params = NULL;
        r_req->params = NULL;
        r_req->state = ALGO_REQ_PENDING;
        r_req->value = r_value;
        r_req->end_req = end_request;
        submit_req(palgo, r_req);
    }
}

void test_insert_update_read(algorithm *palgo)
{
    // insert
    for (uint64_t i = 1; i < NUM_ITEMS; ++i)
    {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);

#ifdef VALUE_CHECK
        if (!values[i])
        {
            values[i] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[i], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        memcpy(w_req->key.key, key, keylen);
        w_req->key.len = keylen;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        submit_req(palgo, w_req);
    }
    // update
    for (uint64_t i = 1; i < NUM_UPDATES; ++i)
    {
        int rndkey = rand() % (NUM_ITEMS - 1) + 1;
        int rnd = rand() % (NUM_ITEMS - 1) + 1;
        // int len = rand() % 3500 + 9;    // variable, must larger than len(str(rnd))
        int len = 512; // fixed, must larger than len(str(rnd))
        char value_str[len];
        memset(value_str, 0, len);
        sprintf(value_str, "%0*d", len - 1, rnd);
        value_set *value = inf_get_valueset(value_str, len);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%d", rndkey);

#ifdef VALUE_CHECK
        if (values[atoi(key)])
        {
            sprintf(values[atoi(key)], "%s", value->value);
        }
        else
        {
            ftl_err("value[%lu] is NULL\n", i);
        }
#endif

        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        memcpy(w_req->key.key, key, keylen);
        w_req->key.len = keylen;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        submit_req(palgo, w_req);
    }
    // read
    for (uint64_t i = 1; i < NUM_ITEMS; ++i)
    {
        value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
        char *key = (char *)malloc(128 * sizeof(char));
        int keylen = sprintf(key, "%lu", i);
        request *r_req = g_malloc0(sizeof(request));
        r_req->type = DATAR;
        memcpy(r_req->key.key, key, keylen);
        r_req->key.len = keylen;
        r_req->h_params = NULL;
        r_req->params = NULL;
        r_req->state = ALGO_REQ_PENDING;
        r_req->value = r_value;
        r_req->end_req = end_request;
        submit_req(palgo, r_req);
    }
}

void test_load(algorithm *palgo, uint64_t num)
{
    // load
    ftl_log("load workload size: %.2f GB\n", (double)num * PIECE / G);
    wr_start_ns = clock_get_ns();
    uint64_t max = num;
    volatile int tr_nr_ios = 0;
    for (uint64_t i = 1; i < num + 1; ++i)
    {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
#ifdef VALUE_CHECK
        if (!values[i])
        {
            values[i] = (char *)malloc(PAGESIZE * sizeof(char));
            sprintf(values[i], "%s", value->value);
        }
#endif
        request *w_req = g_malloc0(sizeof(request));
        w_req->key.len = sprintf(w_req->key.key, "%lu", i);
        w_req->type = DATAW;
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        w_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, w_req);
    }
    usleep(2000000);
}

void *test_update_tr(void *args)
{
    // update
    struct
    {
        algorithm *palgo;
        uint64_t max;
        uint64_t num;
        bool is_zipf;
        int seed;
        volatile bool *pstart;
    } *pargs = args;
    algorithm *palgo = pargs->palgo;
    uint64_t max = pargs->max;
    uint64_t num = pargs->num;
    bool is_zipf = pargs->is_zipf;
    int seed = pargs->seed;
    volatile int tr_nr_ios = 0;

    struct zipf_state zs;
    srand(seed);
    zipf_init(&zs, max - 1, 0.99, -1, seed);
    zipf_disable_hash(&zs);
    while (!(*pargs->pstart))
        ;
    for (uint64_t i = 0; i < num; ++i)
    {
        int len = 8;
        value_set *value = inf_get_valueset("1234567\0", len);
        uint64_t rndkey;
        // int rnd = rand() % (max - 1) + 1;
        if (is_zipf)
            rndkey = zipf_next(&zs) + 1;
        else
            rndkey = rand() % (max - 1) + 1;
        // int len = rand() % 3500 + 9;    // variable, must larger than len(str(rnd))
        // int len = 512;    // fixed, must larger than len(str(rnd))
        char value_str[len];
        memset(value_str, 0, len);
        // sprintf(value_str, "%0*d", len - 1, rnd);
        // value_set *value = inf_get_valueset(value_str, len);
        request *w_req = g_malloc0(sizeof(request));
        w_req->type = DATAW;
        w_req->key.len = sprintf(w_req->key.key, "%lu", rndkey);
        // #ifdef VALUE_CHECK
        //         if (values[atoi(key)]) {
        //             sprintf(values[atoi(key)], "%s", value->value);
        //         } else {
        //             ftl_err("value[%lu] is NULL\n", i);
        //         }
        // #endif
        w_req->h_params = NULL;
        w_req->params = NULL;
        w_req->value = value;
        w_req->state = ALGO_REQ_PENDING;
        w_req->end_req = end_request;
        w_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, w_req);
    }
    usleep(2000000);
    return NULL;
}

void test_update(algorithm *palgo, uint64_t max, uint64_t num, bool is_zipf, int seed, int nr_workers)
{
    // update
    wr_start_ns = clock_get_ns();
    pthread_t workers[nr_workers];
    volatile bool start = false;
    struct
    {
        algorithm *palgo;
        uint64_t max;
        uint64_t num;
        bool is_zipf;
        int seed;
        volatile bool *pstart;
    } args = {
        .palgo = palgo,
        .max = max,
        .num = num,
        .is_zipf = is_zipf,
        .seed = seed,
        .pstart = &start};
    for (int i = 0; i < nr_workers; ++i)
    {
        pthread_create(&workers[i], NULL, test_update_tr, &args);
    }
    sleep(30);
    start = true;
    for (int i = 0; i < nr_workers; ++i)
    {
        pthread_join(workers[i], NULL);
    }
}
uint64_t *shuffle_map = NULL;
void *test_read_tr(void *args)
{
    // read
    static int worker_cnt = 0;
    int worker_id = g_atomic_int_add(&worker_cnt, 1);
    prctl(PR_SET_NAME, "read_worker");
    struct
    {
        algorithm *palgo;
        uint64_t max;
        uint64_t num;
        bool is_zipf;
        int seed;
        volatile bool *pstart;
    } *pargs = args;
    algorithm *palgo = pargs->palgo;
    uint64_t max = pargs->max;
    uint64_t num = pargs->num;
    bool is_zipf = pargs->is_zipf;
    uint32_t seed = pargs->seed + worker_id;
    volatile int tr_nr_ios = 0;

    struct zipf_state zs;
    if (is_zipf)
    {
        zipf_init(&zs, max - 1, 0.99, -1, seed);
        assert(shuffle_map);
        zipf_use_shuffle_map(&zs, shuffle_map);
    }
    srand(seed);
    while (!(*pargs->pstart))
        ;
    for (uint64_t i = 0; i < num; ++i)
    {
        uint64_t rndkey;
        if (is_zipf)
            rndkey = zipf_next(&zs) + 1;
        else
            rndkey = rand_r(&seed) % (max - 1) + 1;
        value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
        request *r_req = g_malloc0(sizeof(request));
        r_req->type = DATAR;
        r_req->key.len = sprintf(r_req->key.key, "%lu", rndkey);
        r_req->h_params = NULL;
        r_req->params = NULL;
        r_req->state = ALGO_REQ_PENDING;
        r_req->value = r_value;
        r_req->end_req = end_request;
        r_req->ptr_nr_ios = &tr_nr_ios;
        submit_req(palgo, r_req);
    }
    thread_ended[worker_id] = true;
    usleep(2000000);
    return NULL;
}

void test_read(algorithm *palgo, uint64_t max, uint64_t num, bool is_zipf, int seed, int nr_workers)
{
    // read
    ftl_log("read workload size: %.2f GB\n", (double)num * NUM_WORKERS * PIECE / G);
    rd_start_ns = clock_get_ns();
    pthread_t workers[nr_workers];
    volatile bool start = false;
    struct
    {
        algorithm *palgo;
        uint64_t max;
        uint64_t num;
        bool is_zipf;
        int seed;
        volatile bool *pstart;
    } args = {
        .palgo = palgo,
        .max = max,
        .num = num,
        .is_zipf = is_zipf,
        .seed = seed,
        .pstart = &start};
    for (int i = 0; i < nr_workers; ++i)
    {
        pthread_create(&workers[i], NULL, test_read_tr, &args);
    }
    sleep(5);
    start = true;
    for (int i = 0; i < nr_workers; ++i)
    {
        pthread_join(workers[i], NULL);
    }
}

void wait_for_nr_ios(request *req)
{
    while (1)
    {
        if ((*req->ptr_nr_ios) < iodepth)
        {
            g_atomic_int_add(req->ptr_nr_ios, 1);
            return;
        }
    }
}

void submit_req(algorithm *palgo, request *req)
{
    wait_for_nr_ios(req);
    pthread_spin_init(&req->timer_lock, PTHREAD_PROCESS_PRIVATE);
    req->stime = clock_get_ns();
    while (!ring_enqueue(palgo->req_q, (void *)&req, 1))
        ;
    ftl_assert((*req->ptr_nr_ios) <= iodepth);
    // pthread_spin_unlock(&submit_lock);
}

void show_latency_stats()
{
    uint64_t avg_rlat = 0, avg_wlat = 0;
    uint64_t nr_rd = 0, nr_wr = 0;
    for (uint64_t i = 0; i <= pt_rlat_arr; i++)
    {
        if (rlat_arr[i])
        {
            // ftl_log("read latency: %lu us, count: %lu\n", i, rlat_arr[i]);
            nr_rd += rlat_arr[i];
            avg_rlat += rlat_arr[i] * i;
        }
    }
    if (nr_rd)
        avg_rlat /= nr_rd;
    for (uint64_t i = 0; i <= pt_wlat_arr; i++)
    {
        if (wlat_arr[i])
        {
            // ftl_log("write latency: %lu us, count: %lu\n", i, wlat_arr[i]);
            nr_wr += wlat_arr[i];
            avg_wlat += wlat_arr[i] * i;
        }
    }
    if (nr_wr)
        avg_wlat /= nr_wr;
    ftl_log("average read latency: %lu us, average write latency: %lu us\n", avg_rlat, avg_wlat);
}

extern uint64_t timer_start_ns;

void init_global_timer()
{
    pthread_spin_init(&global_inner_timer_lock, PTHREAD_PROCESS_PRIVATE);
}

void clean_stats()
{
    for (uint64_t i = 0; i < MAX_LATENCY_US; i++)
    {
        wlat_arr[i] = 0;
        rlat_arr[i] = 0;
    }
    ssd_li.stats->nr_nand_read = ssd_li.stats->nr_nand_write = ssd_li.stats->nr_nand_erase = 0;
    finished_r = finished_w = 0;
    for (int i = 0; i <= d_env.max_try; ++i)
        d_env.r_hash_collision_cnt[i] = 0;
    for (int i = 0; i <= d_env.max_try; ++i)
        d_env.w_hash_collision_cnt[i] = 0;
    write_buffer.stats->nr_rd_hit = write_buffer.stats->nr_rd_miss = write_buffer.stats->nr_wr_hit = write_buffer.stats->nr_wr_miss = 0;
    d_cache.stat.cache_hit = d_cache.stat.cache_miss = d_cache.stat.cache_miss_by_collision = d_cache.stat.cache_hit_by_collision = 0;
    d_env.num_rd_data_rd = d_env.num_rd_data_miss_rd = 0;
    d_cache.stat.dirty_evict = d_cache.stat.cache_load = 0;
    for (int i = 0; i < sizeof(ssd_li.stats->nr_nand_rd_lun); ++i)
    {
        ssd_li.stats->nr_nand_rd_lun[i] = ssd_li.stats->nr_nand_wr_lun[i] = ssd_li.stats->nr_nand_er_lun[i] = 0;
    }
#ifdef HOT_CMT
    d_cache.stat.hot_cmt_hit = 0;
    d_cache.stat.hot_rewrite_entries = 0;
    d_cache.stat.hot_valid_entries = 0;
    d_cache.stat.up_grain_cnt = 0;
    d_cache.stat.up_hit_cnt = 0;
    d_cache.stat.up_page_cnt = 0;
    memset(d_cache.stat.grain_heat_distribute, 0, sizeof(uint32_t) * 1000);
#endif
}

void show_stats()
{
    show_latency_stats();
    ftl_log("finished_r: %lu, finished_w: %lu, wrong_value_cnt: %lu\n", finished_r, finished_w, wrong_value_cnt);
    ftl_log("data_rd: %lu, mapping_rd: %lu, mapping_wr: %lu\n", d_env.num_rd_data_rd, d_cache.stat.cache_load, d_cache.stat.dirty_evict);
    ftl_log("nand_r: %lu, nand_w: %lu, nand_e: %lu\n",
            ssd_li.stats->nr_nand_read, ssd_li.stats->nr_nand_write, ssd_li.stats->nr_nand_erase);
// for (int i = 0; i <= d_env.max_try; ++i)
//     ftl_log("r_hash_collision[%d]: %lu\n", i, d_env.r_hash_collision_cnt[i]);
// for (int i = 0; i <= d_env.max_try; ++i)
//     ftl_log("w_hash_collision[%d]: %lu\n", i, d_env.w_hash_collision_cnt[i]);
// ftl_log("w_buffer: flush: %lu, w_buffer: rd_hit: %lu, rd_miss: %lu, wr_hit: %lu, wr_miss: %lu\n",
//         write_buffer.stats->nr_flush, write_buffer.stats->nr_rd_hit, write_buffer.stats->nr_rd_miss, write_buffer.stats->nr_wr_hit, write_buffer.stats->nr_wr_miss);
#ifdef HOT_CMT
    ftl_log("read iops: %0.2f,  write iops: %0.2f, hit rt:%0.2f%%\n", read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt, (double)(d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit) / (d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit + d_cache.stat.cache_miss) * 100);
    ftl_log("hot valid entries: %lu, hot valid pages: %lu, max hot pages: %lu\n", d_cache.stat.hot_valid_entries, d_cache.stat.hot_valid_entries / EPP, d_cache.env.max_cached_hot_tpages);
    double avg_grain_per_page = (double)d_cache.stat.up_grain_cnt / d_cache.stat.up_page_cnt;
    double avg_grain_hit = (double)d_cache.stat.up_hit_cnt / d_cache.stat.up_grain_cnt;
    ftl_log("avg_grain_per_page: %.2f, avg_grain_hit: %.2f\n",
            avg_grain_per_page,
            avg_grain_hit);
    ftl_log("hot_hit: %lu, hot_rewrite_entries: %lu, up_page_cnt:%lu, up_grain_cnt:%lu, equal_up_page:%d \n", d_cache.stat.hot_cmt_hit, d_cache.stat.hot_rewrite_entries, d_cache.stat.up_page_cnt, d_cache.stat.up_grain_cnt, d_cache.stat.up_grain_cnt / EPP);
    for (int i = 0; i < 10; i++)
    {
        if (d_cache.stat.grain_heat_distribute[i] > 0)
            ftl_log("grain_heat_distribute[%d]: %0.6f%%\n", i, (double)d_cache.stat.grain_heat_distribute[i] / d_cache.stat.up_page_cnt / EPP * 100);
    }
#else
    ftl_log("read iops: %0.2f,write iops: %0.2f, hit rt:%0.2f%%\n", read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt, (d_cache.stat.cache_hit) / (double)(d_cache.stat.cache_hit + d_cache.stat.cache_miss) * 100);
#endif
    ftl_log("d_cache_hit: %lu, miss: %lu, hit_by_collision: %lu, miss_by_collision: %lu\n",
            d_cache.stat.cache_hit, d_cache.stat.cache_miss, d_cache.stat.cache_hit_by_collision, d_cache.stat.cache_miss_by_collision);

    // ftl_log("cmt_nr_cached_pages: %d, cmt_nr_cached_entries %d\n", d_cache.member.nr_cached_tpages, d_cache.member.nr_cached_tentries);
    // ftl_log("hash_sign_collision: %lu\n", d_env.num_rd_data_miss_rd);
    // for (int i = 0; i < 64; ++i) {
    //     ftl_log("lun[%d]: rd: %ld, wr: %ld, er: %ld\n", i, ssd_li.stats->nr_nand_rd_lun[i], ssd_li.stats->nr_nand_wr_lun[i], ssd_li.stats->nr_nand_er_lun[i]);
    // }
}

int main(int argc, char **argv)
{
    struct option opts[] = {
        {"pool_size", required_argument, NULL, 0},
        {"num_update", required_argument, NULL, 1},
        {"num_read", required_argument, NULL, 2},
        {"map_size_frac", required_argument, NULL, 3},
        {"seed", required_argument, NULL, 4},
        {"ext_mem_lat", required_argument, NULL, 5},
        {0, 0, 0, 0},
    };
    // for read test
    ftl_log("hello world\n");
    uint64_t nr_G_workload = 1048576;
    uint64_t pool_size = 8 * nr_G_workload;
    uint64_t num_update = 1 * nr_G_workload;
    uint64_t num_read = 8 * nr_G_workload / NUM_WORKERS;
    float map_size_frac = 8.0 / 8;

    int seed = 1;
    uint64_t ext_mem_lat = 0;
    char *shortopts = "";

    int ret = 0;
    int option_index = 0;
    while ((ret = getopt_long(argc, argv, shortopts, opts, &option_index)) != -1)
    {
        switch (ret)
        {
        case 0:
            // pool_size
            pool_size = atoi(optarg);
            break;
        case 1:
            // num_update
            num_update = atoi(optarg);
            break;
        case 2:
            // num_read
            num_read = atoi(optarg);
            break;
        case 3:
            // mapping_size_frac
            map_size_frac = atoi(optarg);
            break;
        case 4:
            // seed
            seed = atoi(optarg);
            break;
        case 5:
            // ext_mem_lat
            ext_mem_lat = atoi(optarg);
            break;
        case '?':
            break;
        }
    }
    if (!pool_size || !num_read)
    {
        ftl_err("need pool_size and num_read\n");
        abort();
    }
    ftl_log("pool_size = %lu, num_update = %lu, num_read = %lu, map_size_frac = %0.2f, seed = %d, ext_mem_lat = %lu\n", pool_size, num_update, num_read, map_size_frac, seed, ext_mem_lat);
    // env create
    extra_mem_lat = ext_mem_lat;
    timer_start_ns = clock_get_ns();
    algorithm *palgo = &__demand;
    ssd_li.create(&ssd_li);
    palgo->create(palgo, &ssd_li);
    init_global_timer();
    if (map_size_frac < 1)
    {
        d_cache.env.nr_valid_tpages *= map_size_frac;
        d_cache.env.nr_valid_tentries = d_cache.env.nr_valid_tpages * EPP;
    }
    ftl_log("hash_table_size: %lu MB, cached: %lu MB\n", (uint64_t)d_cache.env.nr_valid_tpages * PAGESIZE / 1024 / 1024, (uint64_t)d_cache.env.max_cached_tpages * PAGESIZE / 1024 / 1024);
    fflush(stdout);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&algo_tr, &attr, algo_thread, NULL);
    pthread_attr_t cq_cpl_attr;
    pthread_attr_init(&cq_cpl_attr);
    pthread_create(&finish_tr[0], &cq_cpl_attr, process_cq_cpl, NULL);
    pthread_create(&finish_tr[1], &cq_cpl_attr, process_cq_cpl, NULL);
    // test
    ftl_log("start test.\n");
    /*load and update*/
    ftl_log("start loading. iodepth: %d\n", iodepth);
    toggle_ssd_lat(true);
    test_load(palgo, pool_size);
    test_update(palgo, pool_size, num_update, false, seed, 1);
    ftl_log("gc data: %d, gc mapping  read: %d, gc write: %d\n", D_ENV(palgo)->num_data_gc, D_ENV(palgo)->num_gc_flash_read, D_ENV(palgo)->num_gc_flash_write);
    ftl_log("load finished.\n");
    fflush(stdout);
    sleep(2);
    // clean_stats();
    /*random read*/
    ftl_log("start random reading. iodepth: %d\n", iodepth);
    toggle_ssd_lat(true);
    test_read(palgo, pool_size, num_read, false, seed, NUM_WORKERS);
    ftl_log("finish random reading.\n");
    fflush(stdout);
    sleep(2);
    /*zipfan read*/
    // ftl_log("start zipfian reading. iodepth: %d\n", iodepth);
    // shuffle_map = create_shuffle_map(pool_size - 1);
    // test_read(palgo, pool_size, num_read, true, seed, NUM_WORKERS);
    // ftl_log("finish zipfian reading.\n");
    // fflush(stdout);
    // sleep(2);
    show_stats();

    return 0;
}