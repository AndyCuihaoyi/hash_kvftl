#include "../lower/lower.h"
#include "../tools/random/zipf.h"
#include "../tools/rte_ring/rte_ring.h"
#include "../tools/valueset.h"
#include "algo_queue.h"
#include "cache.h"
#include "demand.h"
#include "dftl_cache.h"
#include "dftl_pg.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "dftl_wb.h"
#include "request.h"
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <glib-2.0/glib.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#define NUM_ITEMS (44000000)
#define NUM_UPDATES (44000000)
#define NUM_WORKERS (8)
volatile bool thread_ended[NUM_WORKERS] = {false};
bool full_worker = true;
// #define VALUE_CHECK

bool ycsb_running = false;
bool update_running = false;
bool ycsb_rmw_running = false;
// YCSB全局统计变量（不分读写）
static uint64_t ycsb_total_ops = 0;      // YCSB总操作数（原子）
static uint64_t ycsb_total_lat_ns = 0;   // YCSB总延迟（纳秒，原子）
static uint64_t ycsb_batch_start_ns = 0; // YCSB批次开始时间
static double ycsb_total_iops = 0;       // YCSB总IOPS累加值
static int ycsb_batch_cnt = 0;           // YCSB批次计数
static uint64_t ycsb_max_lat_us = 0;     // YCSB最大延迟（微秒）

// 可选：YCSB延迟分布数组（按需定义，和读写延迟数组逻辑一致）
#define MAX_YCSB_LAT_SAMPLES 10000
static uint64_t ycsb_latency_samples[MAX_YCSB_LAT_SAMPLES] = {0};
static int ycsb_sample_count = 0;

void *test_rmw_tr(void *args);
static void *test_scan_tr(void *args);
static void *test_insert_latest_tr(void *args);
// YCSB各负载独立函数声明
void test_ycsb_a(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_b(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_c(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_d(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_e(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void test_ycsb_f(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed);
void ycsb_run_workload(char workload, algorithm *palgo, uint64_t pool_size,
                       uint64_t num_ops, int nr_workers, int seed);

#define MAX_READ_SAMPLES (20000000) // 按需调整，足够存储所有读请求延迟
static uint64_t read_latency_samples[MAX_READ_SAMPLES] = {0};
static uint64_t read_sample_count = 0; // 实际收集的读延迟样本数
uint64_t ycsb_total_batch_avg_lat_us = 0;
uint64_t *shuffle_map = NULL;
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
static double update_iops = 0;
static uint64_t read_batch_cnt = 0;
static uint64_t write_batch_cnt = 0;
static uint64_t update_batch_cnt = 0;

void *end_request(request *req)
{
  // req->etime = clock_get_ns();
  if (ycsb_running)
  {
    uint64_t ycsb_ops_local = g_atomic_pointer_add(&ycsb_total_ops, 1);

    uint64_t curr_lat_ns = req->etime - req->stime;
    uint64_t curr_lat_us = curr_lat_ns / 1000;
    uint64_t ycsb_total_lat_local =
        __atomic_add_fetch(&ycsb_total_lat_ns, curr_lat_ns, __ATOMIC_RELAXED);
    uint64_t curr_max_lat = g_atomic_pointer_get(&ycsb_max_lat_us);
    if (curr_lat_us > curr_max_lat)
    {
      g_atomic_pointer_set(&ycsb_max_lat_us, curr_lat_us);
    }

    if (ycsb_sample_count < MAX_YCSB_LAT_SAMPLES)
    {
      ycsb_latency_samples[ycsb_sample_count++] = curr_lat_us;
    }

    if (ycsb_ops_local == 1)
    {
      ycsb_batch_start_ns = req->stime;
    }

    if (ycsb_ops_local % BATCH_SIZE == 0)
    {
      uint64_t now = clock_get_ns();
      double elapsed_s = (now - ycsb_batch_start_ns) / 1e9;
      double batch_iops = (double)BATCH_SIZE / elapsed_s;
      if (ycsb_rmw_running)
        batch_iops = batch_iops / 2; // RMW算两次IOPS
      ycsb_total_iops += batch_iops;
      ycsb_batch_cnt++;
      uint64_t avg_lat_us = (ycsb_total_lat_local / BATCH_SIZE) / 1000;
      __atomic_add_fetch(&ycsb_total_batch_avg_lat_us, avg_lat_us,
                         __ATOMIC_RELAXED);
      ftl_log("[YCSB GLOBAL] %lu th op end. total_iops: %.2f, batch_iops: "
              "%.2f, avg_lat: %lu us, max_lat: %lu us\n",
              ycsb_ops_local, ycsb_total_iops / ycsb_batch_cnt, batch_iops,
              avg_lat_us, g_atomic_pointer_get(&ycsb_max_lat_us));

      g_atomic_pointer_set(&ycsb_total_lat_ns, 0);
      ycsb_batch_start_ns = now;
      fflush(stdout);
      fflush(stderr);
    }
  }

  switch (req->type)
  {
  case DATAR:
    if (req->state == ALGO_REQ_NOT_FOUND)
    {
      ftl_log("%lu th read request end. key: %s, value not found\n",
              ++finished_r, req->key.key);
    }
    else
    {
      // finished_r++;
      uint64_t finished_r_local = g_atomic_pointer_add(&finished_r, 1);
      static uint64_t sum_rlat_ns = 0;
      uint64_t sum_rlat_ns_local =
          g_atomic_pointer_add(&sum_rlat_ns, (req->etime - req->stime));

      uint64_t latency_us = (req->etime - req->stime) / 1000;
      if (read_sample_count < MAX_READ_SAMPLES)
      {
        read_latency_samples[read_sample_count++] = latency_us;
      }

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
        ftl_log("%lu th read request end. key: %s, iops: %lf, avg_lat: %ld, "
                "slow: %ld\n",
                finished_r_local, req->key.key, (double)BATCH_SIZE / elapsed,
                sum_rlat_ns_local / BATCH_SIZE, complete_r_slow);
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
    uint64_t sum_wlat_ns_local =
        __atomic_add_fetch(&sum_wlat_ns, latency, __ATOMIC_RELAXED);
    if ((++finished_w_local) % BATCH_SIZE == 0)
    {
      // ftl_log("\033[1;32m%lu\033[0m th write request end.\n", finished_w);
      uint64_t now = clock_get_ns();
      double elapsed = (now - wr_start_ns) / 1e9;
      if (update_running)
      {
        update_iops += (double)BATCH_SIZE / elapsed;
        update_batch_cnt++;
        wr_start_ns = now;
        ftl_log("%lu th update request end, iops: %lf, avg_lat: %ld, slow: "
                "%ld\n",
                finished_w_local, (double)BATCH_SIZE / elapsed,
                sum_wlat_ns_local / BATCH_SIZE, complete_w_slow);
      }
      else
      {
        write_iops += (double)BATCH_SIZE / elapsed;
        write_batch_cnt++;
        wr_start_ns = now;
        ftl_log("%lu th write request end, iops: %lf, avg_lat: %ld, slow: %ld\n",
                finished_w_local, (double)BATCH_SIZE / elapsed,
                sum_wlat_ns_local / BATCH_SIZE, complete_w_slow);
      }
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
    while (palgo->retry_q->head != NULL &&
           ((request *)palgo->retry_q->head->payload)->etime <= now)
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
    // int len = rand() % 3500 + 9;    // variable, must larger than
    // len(str(rnd))
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

// 新增：test_load的线程包装函数（内部直接调用原test_load，适配并行启动）
void *test_load_wrapper_tr(void *args)
{
  struct
  {
    algorithm *palgo;
    uint64_t num;
    volatile bool *pstart;
  } *pargs = args;
  algorithm *palgo = pargs->palgo;
  uint64_t num = pargs->num;

  // 等待全局启动信号（和读/扫描线程同步）
  while (!(*pargs->pstart))
    ;

  // 直接调用你原有的test_load函数
  test_load(palgo, num);

  return NULL;
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
    // int len = rand() % 3500 + 9;    // variable, must larger than
    // len(str(rnd)) int len = 512;    // fixed, must larger than len(str(rnd))
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

void test_update(algorithm *palgo, uint64_t max, uint64_t num, bool is_zipf,
                 int seed, int nr_workers)
{
  // update
  D_ENV(palgo)->num_rd_data_rd = 0;
  update_running = true;
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
  } args = {.palgo = palgo,
            .max = max,
            .num = num,
            .is_zipf = is_zipf,
            .seed = seed,
            .pstart = &start};
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_create(&workers[i], NULL, test_update_tr, &args);
  }
  sleep(5);
  start = true;
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_join(workers[i], NULL);
  }
}

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
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } *pargs = args;

  algorithm *palgo = pargs->palgo;
  uint64_t max = pargs->max;
  uint64_t num = pargs->num;
  bool is_zipf = pargs->is_zipf;
  bool is_seq = pargs->is_seq;
  uint32_t seed = pargs->seed + worker_id;
  volatile int tr_nr_ios = 0;
  struct zipf_state zs;

  if (is_zipf && !is_seq)
  {
    zipf_init(&zs, max - 1, 0.9, -1, seed);
    assert(shuffle_map);
    zipf_use_shuffle_map(&zs, shuffle_map);
  }

  srand(seed);

  while (!(*pargs->pstart))
    ;
  for (uint64_t i = 0; i < num; ++i)
  {
    uint64_t rndkey;
    if (is_seq)
    {
      rndkey = (uint64_t)worker_id * num + i + 1;
    }
    else if (is_zipf)
    {
      rndkey = zipf_next(&zs) + 1;
    }
    else
    {
      rndkey = rand_r(&seed) % (max - 1) + 1;
    }

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

void test_read(algorithm *palgo, uint64_t max, uint64_t num, bool is_zipf,
               bool is_seq, int seed, int nr_workers)
{
  // read
  ftl_log("read workload size: %.2f GB\n",
          (double)num * NUM_WORKERS * PIECE / G);
  rd_start_ns = clock_get_ns();
  pthread_t workers[nr_workers];
  volatile bool start = false;
  struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } args = {.palgo = palgo,
            .max = max,
            .num = num,
            .is_zipf = is_zipf,
            .is_seq = is_seq,
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
  ftl_log("average read latency: %lu us, average write latency: %lu us\n",
          avg_rlat, avg_wlat);
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
  ssd_li.stats->nr_nand_read = ssd_li.stats->nr_nand_write =
      ssd_li.stats->nr_nand_erase = 0;
  finished_r = finished_w = 0;
  for (int i = 0; i <= d_env.max_try; ++i)
    d_env.r_hash_collision_cnt[i] = 0;
  for (int i = 0; i <= d_env.max_try; ++i)
    d_env.w_hash_collision_cnt[i] = 0;
  write_buffer.stats->nr_rd_hit = write_buffer.stats->nr_rd_miss =
      write_buffer.stats->nr_wr_hit = write_buffer.stats->nr_wr_miss = 0;
  d_cache.stat.cache_hit = d_cache.stat.cache_miss =
      d_cache.stat.cache_miss_by_collision =
          d_cache.stat.cache_hit_by_collision = 0;
  d_env.num_rd_data_rd = d_env.num_rd_data_miss_rd = 0;
  d_cache.stat.dirty_evict = d_cache.stat.cache_load = 0;
  for (int i = 0; i < sizeof(ssd_li.stats->nr_nand_rd_lun); ++i)
  {
    ssd_li.stats->nr_nand_rd_lun[i] = ssd_li.stats->nr_nand_wr_lun[i] =
        ssd_li.stats->nr_nand_er_lun[i] = 0;
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
  memset(read_latency_samples, 0, sizeof(read_latency_samples));
  read_sample_count = 0;
}

static int compare_uint64(const void *a, const void *b)
{
  uint64_t val_a = *(const uint64_t *)a;
  uint64_t val_b = *(const uint64_t *)b;
  if (val_a < val_b)
    return -1;
  if (val_a > val_b)
    return 1;
  return 0;
}

static uint64_t calculate_tail_latency(uint64_t *samples, uint64_t sample_count,
                                       double percentile)
{
  if (sample_count == 0)
    return 0;

  // 排序延迟样本（升序）
  qsort(samples, sample_count, sizeof(uint64_t), compare_uint64);

  // 计算百分位位置（线性插值，保证精度）
  double index = percentile / 100.0 * (sample_count - 1);
  uint64_t floor_idx = (uint64_t)index;
  uint64_t ceil_idx = floor_idx + 1;
  double frac = index - floor_idx;

  if (ceil_idx >= sample_count)
  {
    return samples[floor_idx]; // 边界情况：取最后一个值
  }
  // 线性插值计算尾时延
  return (uint64_t)(samples[floor_idx] * (1 - frac) + samples[ceil_idx] * frac);
}

void write_test_result_to_file()
{

  FILE *fp = fopen("test_result", "a");
  if (fp == NULL)
  {
    ftl_err("Failed to open test_result file: %s\n", strerror(errno));
    return;
  }
  time_t now = time(NULL);
  struct tm *local_tm = localtime(&now);
  char time_str[64] = {0};
  strftime(time_str, sizeof(time_str),
           "===== Test Time: %Y-%m-%d %H:%M:%S =====", local_tm);
  fprintf(fp, "%s\n", time_str);

  uint64_t avg_rlat = 0, avg_wlat = 0;
  uint64_t nr_rd = 0, nr_wr = 0;
  for (uint64_t i = 0; i <= pt_rlat_arr; i++)
  {
    if (rlat_arr[i])
    {
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
      nr_wr += wlat_arr[i];
      avg_wlat += wlat_arr[i] * i;
    }
  }
  if (nr_wr)
    avg_wlat /= nr_wr;
  fprintf(fp, "average read latency: %lu us, average write latency: %lu us\n",
          avg_rlat, avg_wlat);

  fprintf(fp, "finished_r: %lu, finished_w: %lu, wrong_value_cnt: %lu\n",
          finished_r, finished_w, wrong_value_cnt);
  fprintf(fp, "data_rd: %lu, mapping_rd: %lu, mapping_wr: %lu\n",
          d_env.num_rd_data_rd, d_cache.stat.cache_load,
          d_cache.stat.dirty_evict);
  fprintf(fp, "nand_r: %lu, nand_w: %lu, nand_e: %lu\n",
          ssd_li.stats->nr_nand_read, ssd_li.stats->nr_nand_write,
          ssd_li.stats->nr_nand_erase);

  for (int i = 0; i <= d_env.max_try; ++i)
    fprintf(fp, "r_hash_collision[%d]: %lu\n", i,
            d_env.r_hash_collision_cnt[i]);

#ifdef HOT_CMT
  double hit_ratio =
      (double)(d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit) /
      (d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit +
       d_cache.stat.cache_miss) *
      100;
  fprintf(fp, "read iops: %0.2f,  write iops: %0.2f, hit rt:%0.2f%%\n",
          read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt,
          hit_ratio);
  fprintf(fp,
          "hot valid entries: %lu, hot valid pages: %lu, max hot pages: %lu\n",
          d_cache.stat.hot_valid_entries, d_cache.stat.hot_valid_entries / EPP,
          d_cache.env.max_cached_hot_tpages);
  double avg_grain_per_page =
      (double)d_cache.stat.up_grain_cnt / d_cache.stat.up_page_cnt;
  double avg_grain_hit =
      (double)d_cache.stat.up_hit_cnt / d_cache.stat.up_grain_cnt;
  fprintf(fp, "avg_grain_per_page: %.2f, avg_grain_hit: %.2f\n",
          avg_grain_per_page, avg_grain_hit);
  fprintf(fp,
          "hot_hit: %lu, hot_rewrite_entries: %lu, up_page_cnt:%lu, "
          "up_grain_cnt:%lu, equal_up_page:%d \n",
          d_cache.stat.hot_cmt_hit, d_cache.stat.hot_rewrite_entries,
          d_cache.stat.up_page_cnt, d_cache.stat.up_grain_cnt,
          d_cache.stat.up_grain_cnt / EPP);
  for (int i = 0; i < 10; i++)
  {
    if (d_cache.stat.grain_heat_distribute[i] > 0)
      fprintf(fp, "grain_heat_distribute[%d]: %0.6f%%\n", i,
              (double)d_cache.stat.grain_heat_distribute[i] /
                  d_cache.stat.up_page_cnt / EPP * 100);
  }
#else
  double hit_ratio = (double)d_cache.stat.cache_hit /
                     (d_cache.stat.cache_hit + d_cache.stat.cache_miss) * 100;
  fprintf(fp, "read iops: %0.2f,write iops: %0.2f, hit rt:%0.2f%%\n",
          read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt,
          hit_ratio);
#endif

  fprintf(fp,
          "d_cache_hit: %lu, miss: %lu, hit_by_collision: %lu, "
          "miss_by_collision: %lu\n",
          d_cache.stat.cache_hit, d_cache.stat.cache_miss,
          d_cache.stat.cache_hit_by_collision,
          d_cache.stat.cache_miss_by_collision);

  if (read_sample_count == 0)
  {
    fprintf(fp, "No read latency samples collected\n");
  }
  else
  {
    uint64_t sum_lat_us = 0;
    for (uint64_t i = 0; i < read_sample_count; i++)
    {
      sum_lat_us += read_latency_samples[i];
    }
    uint64_t avg_rlat_us = sum_lat_us / read_sample_count;
    uint64_t p95_rlat_us =
        calculate_tail_latency(read_latency_samples, read_sample_count, 95.0);
    uint64_t p99_rlat_us =
        calculate_tail_latency(read_latency_samples, read_sample_count, 99.0);
    uint64_t p99_9_rlat_us =
        calculate_tail_latency(read_latency_samples, read_sample_count, 99.9);
    fprintf(fp, "========== Tail Latency Stats (iodepth=1) ==========\n");
    fprintf(fp, "avg_rlat/us: %lu\n", avg_rlat_us);
    fprintf(fp, "p95_rlat/us: %lu\n", p95_rlat_us);
    fprintf(fp, "p99_rlat/us: %lu\n", p99_rlat_us);
    fprintf(fp, "p99.9_rlat/us: %lu\n", p99_9_rlat_us);
  }

  fprintf(fp, "--------------------------------------------------\n\n");

  fclose(fp);
}

static void show_tail_latency_stats()
{
  if (read_sample_count == 0)
  {
    ftl_log("No read latency samples collected\n");
    return;
  }

  // 1. 计算平均读延迟
  uint64_t sum_lat_us = 0;
  for (uint64_t i = 0; i < read_sample_count; i++)
  {
    sum_lat_us += read_latency_samples[i];
  }
  uint64_t avg_rlat_us = sum_lat_us / read_sample_count;

  // 2. 计算尾时延（p95/p99/p99.9）
  uint64_t p95_rlat_us =
      calculate_tail_latency(read_latency_samples, read_sample_count, 95.0);
  uint64_t p99_rlat_us =
      calculate_tail_latency(read_latency_samples, read_sample_count, 99.0);
  uint64_t p99_9_rlat_us =
      calculate_tail_latency(read_latency_samples, read_sample_count, 99.9);

  // 3. 打印尾时延结果（仅保留核心指标）
  ftl_log("========== Tail Latency Stats (iodepth=1) ==========\n");
  ftl_log("avg_rlat/us: %lu p95_rlat/us: %lu p99_rlat/us: %lu p99.9_rlat/us: %lu\n", avg_rlat_us, p95_rlat_us, p99_rlat_us, p99_9_rlat_us);
}

void show_stats()
{
  show_latency_stats();
  ftl_log("finished_r: %lu, finished_w: %lu, wrong_value_cnt: %lu\n",
          finished_r, finished_w, wrong_value_cnt);
  ftl_log("data_rd: %lu, mapping_rd: %lu, mapping_wr: %lu\n",
          d_env.num_rd_data_rd, d_cache.stat.cache_load,
          d_cache.stat.dirty_evict);
  ftl_log("nand_r: %lu, nand_w: %lu, nand_e: %lu\n", ssd_li.stats->nr_nand_read,
          ssd_li.stats->nr_nand_write, ssd_li.stats->nr_nand_erase);
  // for (int i = 0; i <= d_env.max_try; ++i)
  //     ftl_log("r_hash_collision[%d]: %lu\n", i,
  //     d_env.r_hash_collision_cnt[i]);
  // for (int i = 0; i <= d_env.max_try; ++i)
  //     ftl_log("w_hash_collision[%d]: %lu\n", i,
  //     d_env.w_hash_collision_cnt[i]);
  // ftl_log("w_buffer: flush: %lu, w_buffer: rd_hit: %lu, rd_miss: %lu, wr_hit:
  // %lu, wr_miss: %lu\n",
  //         write_buffer.stats->nr_flush, write_buffer.stats->nr_rd_hit,
  //         write_buffer.stats->nr_rd_miss, write_buffer.stats->nr_wr_hit,
  //         write_buffer.stats->nr_wr_miss);
#ifdef HOT_CMT
  ftl_log("read iops: %0.2f,  write iops: %0.2f, update iops: %0.2f, hit rt:%0.2f%%\n",
          read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt,
          update_iops / update_batch_cnt,
          (double)(d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit) /
              (d_cache.stat.cache_hit + d_cache.stat.hot_cmt_hit +
               d_cache.stat.cache_miss) *
              100);
  ftl_log("hot valid entries: %lu, hot valid pages: %lu, max hot pages: %lu\n",
          d_cache.stat.hot_valid_entries, d_cache.stat.hot_valid_entries / EPP,
          d_cache.env.max_cached_hot_tpages);
  double avg_grain_per_page =
      (double)d_cache.stat.up_grain_cnt / d_cache.stat.up_page_cnt;
  double avg_grain_hit =
      (double)d_cache.stat.up_hit_cnt / d_cache.stat.up_grain_cnt;
  ftl_log("avg_grain_per_page: %.2f, avg_grain_hit: %.2f\n", avg_grain_per_page,
          avg_grain_hit);
  ftl_log("hot_hit: %lu, hot_rewrite_entries: %lu, up_page_cnt:%lu, "
          "up_grain_cnt:%lu, equal_up_page:%d \n",
          d_cache.stat.hot_cmt_hit, d_cache.stat.hot_rewrite_entries,
          d_cache.stat.up_page_cnt, d_cache.stat.up_grain_cnt,
          d_cache.stat.up_grain_cnt / EPP);
  for (int i = 0; i < 10; i++)
  {
    if (d_cache.stat.grain_heat_distribute[i] > 0)
      ftl_log("grain_heat_distribute[%d]: %0.6f%%\n", i,
              (double)d_cache.stat.grain_heat_distribute[i] /
                  d_cache.stat.up_page_cnt / EPP * 100);
  }
#else
  ftl_log("read iops: %0.2f,write iops: %0.2f,update iops: %0.2f,hit rt:%0.2f%%\n",
          read_iops / (read_batch_cnt - 1), write_iops / write_batch_cnt,
          update_iops / update_batch_cnt,
          (d_cache.stat.cache_hit) /
              (double)(d_cache.stat.cache_hit + d_cache.stat.cache_miss) * 100);
#endif
  ftl_log("d_cache_hit: %lu, miss: %lu, hit_by_collision: %lu, "
          "miss_by_collision: %lu\n",
          d_cache.stat.cache_hit, d_cache.stat.cache_miss,
          d_cache.stat.cache_hit_by_collision,
          d_cache.stat.cache_miss_by_collision);

  // ftl_log("cmt_nr_cached_pages: %d, cmt_nr_cached_entries %d\n",
  // d_cache.member.nr_cached_tpages, d_cache.member.nr_cached_tentries);
  // ftl_log("hash_sign_collision: %lu\n", d_env.num_rd_data_miss_rd);
  // for (int i = 0; i < 64; ++i) {
  //     ftl_log("lun[%d]: rd: %ld, wr: %ld, er: %ld\n", i,
  //     ssd_li.stats->nr_nand_rd_lun[i], ssd_li.stats->nr_nand_wr_lun[i],
  //     ssd_li.stats->nr_nand_er_lun[i]);
  // }
  show_tail_latency_stats();

  write_test_result_to_file();
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
  int64_t pool_size = 8 * nr_G_workload;
  uint64_t num_update = 4 * nr_G_workload;
  uint64_t num_read = 8 * nr_G_workload / NUM_WORKERS;
  float map_size_frac = 8.0 / 128;

  int seed = 1;
  uint64_t ext_mem_lat = 0;
  char *shortopts = "";

  int ret = 0;
  int option_index = 0;
  while ((ret = getopt_long(argc, argv, shortopts, opts, &option_index)) !=
         -1)
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
  ftl_log("pool_size = %lu, num_update = %lu, num_read = %lu, map_size_frac = "
          "%0.2f, seed = %d, ext_mem_lat = %lu\n",
          pool_size, num_update, num_read, map_size_frac, seed, ext_mem_lat);
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
  ftl_log("hash_table_size: %lu MB, cached: %lu MB\n",
          (uint64_t)d_cache.env.nr_valid_tpages * PAGESIZE / 1024 / 1024,
          (uint64_t)d_cache.env.max_cached_tpages * PAGESIZE / 1024 / 1024);
  fflush(stdout);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_create(&algo_tr, &attr, algo_thread, NULL);
  pthread_attr_t cq_cpl_attr;
  pthread_attr_init(&cq_cpl_attr);
  pthread_create(&finish_tr[0], &cq_cpl_attr, process_cq_cpl, NULL);
  pthread_create(&finish_tr[1], &cq_cpl_attr, process_cq_cpl, NULL);
  shuffle_map = create_shuffle_map(pool_size - 1);
  // test
  ftl_log("start test.\n");
  /*load and update*/
  ftl_log("start loading. iodepth: %d\n", iodepth);
  toggle_ssd_lat(true);
  test_load(palgo, (int)(pool_size * 1));
  sleep(5);
  // D_ENV(palgo)->pb_mgr->show_sblk_state(D_ENV(palgo)->pb_mgr, 1);
  // test_update(palgo, pool_size, num_update, true, seed, 1);
  // uint32_t num_data = D_ENV(palgo)->num_data_gc;
  // uint32_t num_reads = D_ENV(palgo)->num_gc_flash_read;
  // uint32_t read_for_data_check = D_ENV(palgo)->num_rd_data_rd;
  // ftl_log("gc data: %u, gc mapping read: %u, avg read: %.2f\n",
  //         num_data,
  //         num_reads,
  //         (float)num_reads / num_data);
  // ftl_log("read for data check: %u, avg update data check: %0.2f\n", read_for_data_check, (float)(read_for_data_check) / num_update);
  // ftl_log("load finished.\n");
  fflush(stdout);
  sleep(5);

  clean_stats();
  ycsb_run_workload('a', palgo, pool_size, pool_size, NUM_WORKERS, seed);

#ifdef HOT_CMT
  d_cache.hot_cmt_reset(&d_cache);
#endif
  /*seq read*/
  // ftl_log("start seq reading. iodepth: %d\n", iodepth);
  // toggle_ssd_lat(true);
  // test_read(palgo, pool_size, num_read, false, true, seed, NUM_WORKERS);
  // ftl_log("finish seq reading.\n");
  // fflush(stdout);
  // sleep(2);

  /*random read*/
  // ftl_log("start random reading. iodepth: %d\n", iodepth);
  // toggle_ssd_lat(true);
  // test_read(palgo, pool_size, num_read, false, false, seed, NUM_WORKERS);
  // ftl_log("finish random reading.\n");
  // fflush(stdout);
  // sleep(2);

  /*zipfan read*/
  // ftl_log("start zipfian reading. iodepth: %d\n", iodepth);
  // toggle_ssd_lat(true);
  // test_read(palgo, pool_size, num_read, true, false, seed, NUM_WORKERS);
  // ftl_log("finish zipfian reading.\n");
  // fflush(stdout);
  // sleep(2);
  show_stats();
  return 0;
}

//---------------ycsb test functions---------------------//

// YCSB E workload - 扫描线程实现
void *test_rmw_tr(void *args)
{
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
  srand(seed + pthread_self());
  zipf_init(&zs, max - 1, 0.99, -1, seed);
  zipf_disable_hash(&zs);

  while (!(*pargs->pstart))
    ;

  for (uint64_t i = 0; i < num; ++i)
  {
    uint64_t rndkey;
    value_set *r_value = NULL;
    request *r_req = NULL;
    value_set *w_value = NULL;
    request *w_req = NULL;

    if (is_zipf)
      rndkey = zipf_next(&zs) + 1;
    else
      rndkey = rand() % (max - 1) + 1;

    if (rndkey > max)
      rndkey = max;

    r_value = inf_get_valueset(NULL, PAGESIZE); // 接收读返回值
    r_req = g_malloc0(sizeof(request));
    r_req->type = DATAR;
    r_req->key.len = sprintf(r_req->key.key, "%lu", rndkey);
    r_req->h_params = NULL;
    r_req->params = NULL;
    r_req->state = ALGO_REQ_PENDING;
    r_req->value = r_value;
    r_req->end_req = end_request;
    r_req->ptr_nr_ios = &tr_nr_ios;
    tr_nr_ios = 0;
    submit_req(palgo, r_req);

    while (tr_nr_ios > 0)
      ;
    usleep(10);

    char modified_value[8];
    for (int j = 0; j < 7; j++)
    {
      modified_value[j] = '0' + (int)(rand() % 10);
    }
    modified_value[7] = '\0';
    w_value = inf_get_valueset(modified_value, 7);

    w_req = g_malloc0(sizeof(request));
    w_req->type = DATAW;
    w_req->key.len = sprintf(w_req->key.key, "%lu", rndkey);
    w_req->h_params = NULL;
    w_req->params = NULL;
    w_req->value = w_value;
    w_req->state = ALGO_REQ_PENDING;
    w_req->end_req = end_request;
    w_req->ptr_nr_ios = &tr_nr_ios;
    tr_nr_ios = 0;
    submit_req(palgo, w_req);

    // 等待写请求完成（保证本次RMW原子性完成）
    while (tr_nr_ios > 0)
      ;
    usleep(10);
  }

  usleep(2000000); // 等待最后一批请求完成，合理保留
  return NULL;
}

static void *test_scan_tr(void *args)
{
  static int worker_cnt = 0;
  int worker_id = g_atomic_int_add(&worker_cnt, 1);
  prctl(PR_SET_NAME, "scan_worker");

  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    uint64_t scan_len;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } ScanArgs;
  ScanArgs *pargs = (ScanArgs *)args;

  algorithm *palgo = pargs->palgo;
  uint64_t max = pargs->max;
  uint64_t num = pargs->num;
  uint64_t scan_len = pargs->scan_len;
  bool is_zipf = pargs->is_zipf;
  uint32_t seed = pargs->seed + worker_id;
  volatile int tr_nr_ios = 0;
  struct zipf_state zs;

  if (is_zipf)
  {
    zipf_init(&zs, max - 1, 0.99, -1, seed);
    assert(shuffle_map != NULL);
    zipf_use_shuffle_map(&zs, shuffle_map);
  }
  srand(seed);

  while (!(*pargs->pstart))
    ;

  for (uint64_t i = 0; i < num; ++i)
  {
    uint64_t start_key;
    if (is_zipf)
    {
      start_key = zipf_next(&zs) + 1;
    }
    else
    {
      start_key = rand_r(&seed) % (max - scan_len) + 1;
    }

    for (uint64_t s = 0; s < scan_len; ++s)
    {
      uint64_t cur_key = start_key + s;
      if (cur_key > max)
        break;

      value_set *r_value = inf_get_valueset(NULL, PAGESIZE);
      request *r_req = g_malloc0(sizeof(request));
      r_req->type = DATAR;
      r_req->key.len = sprintf(r_req->key.key, "%lu", cur_key);
      r_req->h_params = NULL;
      r_req->params = NULL;
      r_req->state = ALGO_REQ_PENDING;
      r_req->value = r_value;
      r_req->end_req = end_request;
      r_req->ptr_nr_ios = &tr_nr_ios;
      submit_req(palgo, r_req);
    }
  }

  if (worker_id < NUM_WORKERS)
  {
    thread_ended[worker_id] = true;
  }
  usleep(2000000);
  return NULL;
}

void ycsb_run_workload(char workload, algorithm *palgo, uint64_t pool_size,
                       uint64_t num_ops, int nr_workers, int seed)
{
  ycsb_running = true;
  ftl_log("========== Start YCSB Workload %c ==========\n", workload);
  uint64_t ycsb_start_ns = clock_get_ns();
  double ycsb_read_iops = 0, ycsb_write_iops = 0;
  uint64_t ycsb_read_cnt = 0, ycsb_write_cnt = 0;

  // 重置统计量
  uint64_t prev_finished_r = finished_r;
  uint64_t prev_finished_w = finished_w;
  clean_stats();
  // 调用对应负载的独立函数
  switch (tolower(workload))
  {
  case 'a':
    test_ycsb_a(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'b':
    test_ycsb_b(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'c':
    test_ycsb_c(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'd':
    test_ycsb_d(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'e':
    test_ycsb_e(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  case 'f':
    test_ycsb_f(palgo, pool_size, num_ops, nr_workers, seed);
    break;
  default:
    ftl_err("Unsupported YCSB workload: %c (only a-f are supported)\n",
            workload);
    abort();
  }

  ycsb_read_cnt = finished_r - prev_finished_r;
  ycsb_write_cnt = finished_w - prev_finished_w;

  uint64_t final_ycsb_total_ops = g_atomic_pointer_get(&ycsb_total_ops);
  uint64_t final_ycsb_total_lat_ns = g_atomic_pointer_get(&ycsb_total_lat_ns);
  uint64_t final_ycsb_max_lat_us = g_atomic_pointer_get(&ycsb_max_lat_us);
  double final_ycsb_avg_iops =
      ycsb_batch_cnt > 0 ? (ycsb_total_iops / ycsb_batch_cnt) : 0.0;
  uint64_t final_ycsb_avg_lat_us = 0;
  if (ycsb_batch_cnt > 0)
  {
    final_ycsb_avg_lat_us = ycsb_total_batch_avg_lat_us / ycsb_batch_cnt;
  }
  ftl_log("[YCSB GLOBAL] Total ops: %lu, Avg IOPS: %.2f, Avg latency: %lu us, "
          "Max latency: %lu us\n",
          final_ycsb_total_ops, final_ycsb_avg_iops, final_ycsb_avg_lat_us,
          final_ycsb_max_lat_us);

  show_stats();
  ftl_log("========== Finish YCSB Workload %c ==========\n", workload);
  ycsb_running = false;
}

void test_ycsb_a(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed)
{
  uint64_t num_update_total = num_ops * 0.5;
  uint64_t num_read_per_worker = (num_ops * 0.5) / nr_workers;

  // 并行执行：1个update线程 + nr_workers个read线程
  volatile bool start = false;
  pthread_t update_worker;
  pthread_t read_workers[nr_workers];
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns;
  rd_start_ns = parallel_start_ns;

  // 构造update线程参数
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } UpdateArgs;
  UpdateArgs update_args = {.palgo = palgo,
                            .max = pool_size,
                            .num = num_update_total,
                            .is_zipf = true,
                            .seed = seed,
                            .pstart = &start};

  // 构造read线程参数
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = pool_size,
                        .num = num_read_per_worker,
                        .is_zipf = true,
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // 创建线程（等待启动状态）
  ftl_log(
      "YCSB Workload a: Create 1 update thread + %d read threads (parallel)\n",
      nr_workers);
  // pthread_create(&update_worker, NULL, test_update_tr, &update_args);
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // 等待线程就绪，统一启动
  sleep(5);
  ftl_log("YCSB Workload a: Start update + read (parallel)\n", nr_workers);
  start = true;

  // 等待所有线程结束
  // pthread_join(update_worker, NULL);
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_join(read_workers[i], NULL);
  }
  ftl_log("YCSB Workload a: Parallel execution finished\n");
}

void test_ycsb_b(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed)
{
  uint64_t num_update_total = num_ops * 0.05;
  uint64_t num_read_per_worker = (num_ops * 0.95) / nr_workers;

  // 并行执行：1个update线程 + nr_workers个read线程
  volatile bool start = false;
  pthread_t update_worker;
  pthread_t read_workers[nr_workers];
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns;
  rd_start_ns = parallel_start_ns;

  // 构造update线程参数
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } UpdateArgs;
  UpdateArgs update_args = {.palgo = palgo,
                            .max = pool_size,
                            .num = num_update_total,
                            .is_zipf = true,
                            .seed = seed,
                            .pstart = &start};

  // 构造read线程参数
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = pool_size,
                        .num = num_read_per_worker,
                        .is_zipf = true,
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // 创建线程（等待启动状态）
  ftl_log(
      "YCSB Workload b: Create 1 update thread + %d read threads (parallel)\n",
      nr_workers);
  pthread_create(&update_worker, NULL, test_update_tr, &update_args);
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // 等待线程就绪，统一启动
  sleep(5);
  ftl_log("YCSB Workload b: Start update + read (parallel)\n", nr_workers);
  start = true;

  // 等待所有线程结束
  pthread_join(update_worker, NULL);
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_join(read_workers[i], NULL);
  }
  ftl_log("YCSB Workload b: Parallel execution finished\n");
}

void test_ycsb_c(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed)
{
  uint64_t num_read_per_worker = num_ops / nr_workers;
  ftl_log("YCSB Workload c: Start read phase (multi-thread)\n");
  test_read(palgo, pool_size, num_read_per_worker, true, false, seed,
            nr_workers);
  ftl_log("YCSB Workload c: Read phase finished\n");
}

void test_ycsb_d(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed)
{
  uint64_t num_insert_total = num_ops * 0.05; // 5%插入（调用原test_load）
  uint64_t num_read_per_worker =
      (num_ops * 0.95) / nr_workers; // 95%读（多线程）

  // 并行执行核心变量：共享启动信号、统一时间基准
  volatile bool start = false;
  pthread_t insert_worker;            // 1个插入线程（包装test_load）
  pthread_t read_workers[nr_workers]; // nr_workers个读线程
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns; // 插入（写）时间基准对齐
  rd_start_ns = parallel_start_ns; // 读时间基准对齐

  // ========== 构造插入线程参数（传给包装函数） ==========
  typedef struct
  {
    algorithm *palgo;
    uint64_t num;
    volatile bool *pstart;
  } InsertArgs;
  InsertArgs insert_args = {
      .palgo = palgo, .num = num_insert_total, .pstart = &start};

  // ========== 构造读线程参数 ==========
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = (int)(pool_size * 0.95),
                        .num = num_read_per_worker,
                        .is_zipf = false, // D负载读不使用zipf
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // ========== 创建线程（等待启动状态） ==========
  ftl_log("YCSB Workload d: Create 1 insert thread (call test_load) + %d read "
          "threads (parallel)\n",
          nr_workers);
  // 创建插入线程（调用包装函数，内部执行原test_load）
  pthread_create(&insert_worker, NULL, test_load_wrapper_tr, &insert_args);
  // 创建读线程
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // ========== 等待线程就绪，统一启动 ==========
  sleep(5); // 等待所有线程初始化完成
  ftl_log("YCSB Workload d: Start insert (test_load) + read (parallel)\n");
  start = true;

  // ========== 等待所有线程执行完毕 ==========
  // 等待插入线程结束（test_load执行完成）
  pthread_join(insert_worker, NULL);
  ftl_log("YCSB Workload d: Insert thread (test_load) finished\n");
  // 等待所有读线程结束
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_join(read_workers[i], NULL);
  }
  ftl_log("YCSB Workload d: All read threads finished\n");
  ftl_log("YCSB Workload d: Parallel execution finished\n");
}

void test_ycsb_e(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed)
{
  uint64_t num_insert_total = num_ops * 0.05; // 5%插入（调用原test_load）
  uint64_t num_scan_total = num_ops * 0.95;   // 95%扫描（1线程）
  uint64_t scan_len = 100;                    // 扫描长度

  // 并行执行核心变量：共享启动信号、统一时间基准
  volatile bool start = false;
  pthread_t insert_worker; // 1个插入线程（包装test_load）
  pthread_t scan_worker;   // 1个扫描线程
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns; // 插入（写）时间基准对齐

  // ========== 构造插入线程参数（传给包装函数） ==========
  typedef struct
  {
    algorithm *palgo;
    uint64_t num;
    volatile bool *pstart;
  } InsertArgs;
  InsertArgs insert_args = {
      .palgo = palgo, .num = num_insert_total, .pstart = &start};

  // ========== 构造扫描线程参数 ==========
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    uint64_t scan_len;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } ScanArgs;
  ScanArgs scan_args = {.palgo = palgo,
                        .max = (int)(pool_size * 0.95),
                        .num = (uint64_t)(num_scan_total / scan_len),
                        .scan_len = scan_len,
                        .is_zipf = true,
                        .seed = seed,
                        .pstart = &start};

  // ========== 创建线程（等待启动状态） ==========
  ftl_log("YCSB Workload e: Create 1 insert thread (call test_load) + 1 scan "
          "thread (parallel)\n");
  // 创建插入线程（调用包装函数，内部执行原test_load）
  pthread_create(&insert_worker, NULL, test_load_wrapper_tr, &insert_args);
  // 创建扫描线程
  pthread_create(&scan_worker, NULL, test_scan_tr, &scan_args);

  // ========== 等待线程就绪，统一启动 ==========
  sleep(5); // 等待所有线程初始化完成
  ftl_log("YCSB Workload e: Start insert (test_load) + scan (parallel)\n");
  start = true;

  // ========== 等待所有线程执行完毕 ==========
  // 等待插入线程结束（test_load执行完成）
  pthread_join(insert_worker, NULL);
  ftl_log("YCSB Workload e: Insert thread (test_load) finished\n");
  // 等待扫描线程结束
  pthread_join(scan_worker, NULL);
  ftl_log("YCSB Workload e: Scan thread finished\n");
  ftl_log("YCSB Workload e: Parallel execution finished\n");
}

void test_ycsb_f(algorithm *palgo, uint64_t pool_size, uint64_t num_ops,
                 int nr_workers, int seed)
{
  uint64_t num_rmw_total = num_ops * 0.5; // 50% RMW操作（1线程）
  uint64_t num_read_per_worker =
      (num_ops * 0.5) / nr_workers; // 50%读（多线程）

  // 并行执行核心变量：共享启动信号、统一时间基准
  volatile bool start = false;
  pthread_t rmw_worker;               // 1个RMW线程（替换原update_worker）
  pthread_t read_workers[nr_workers]; // nr_workers个读线程
  uint64_t parallel_start_ns = clock_get_ns();
  wr_start_ns = parallel_start_ns; // RMW（写）时间基准对齐
  rd_start_ns = parallel_start_ns; // 读时间基准对齐

  // ========== 构造RMW线程参数（和原UpdateArgs一致） ==========
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    int seed;
    volatile bool *pstart;
  } RmwArgs;
  RmwArgs rmw_args = {.palgo = palgo,
                      .max = pool_size,
                      .num = num_rmw_total, // RMW操作总数
                      .is_zipf = true,
                      .seed = seed,
                      .pstart = &start};

  // ========== 构造读线程参数（不变） ==========
  typedef struct
  {
    algorithm *palgo;
    uint64_t max;
    uint64_t num;
    bool is_zipf;
    bool is_seq;
    int seed;
    volatile bool *pstart;
  } ReadArgs;
  ReadArgs read_args = {.palgo = palgo,
                        .max = pool_size,
                        .num = num_read_per_worker,
                        .is_zipf = true,
                        .is_seq = false,
                        .seed = seed,
                        .pstart = &start};

  // ========== 创建线程（等待启动状态） ==========
  ftl_log("YCSB Workload f: Create 1 RMW thread + %d read threads (parallel)\n",
          nr_workers);
  ftl_log("YCSB Workload f: Start RMW (Read-Modify-Write) + read (parallel)\n");
  start = true;
  // 创建读线程（不变）
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_create(&read_workers[i], NULL, test_read_tr, &read_args);
  }

  // ========== 等待线程就绪，统一启动 ==========
  sleep(5);
  for (int i = 0; i < nr_workers; ++i)
  {
    pthread_join(read_workers[i], NULL);
  }
  // 创建RMW线程（替换原update线程）
  ycsb_rmw_running = true;
  pthread_create(&rmw_worker, NULL, test_rmw_tr, &rmw_args);
  // 等待RMW线程结束
  pthread_join(rmw_worker, NULL);
  ftl_log("YCSB Workload f: RMW thread finished\n");
  ftl_log("YCSB Workload f: All read threads finished\n");
  ftl_log("YCSB Workload f: Parallel execution finished\n");
  ycsb_rmw_running = false;
}
