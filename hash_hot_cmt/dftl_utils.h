#ifndef DFTL_UTILS_H
#define DFTL_UTILS_H

#include "../tools/sha256.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "dftl_types.h"
#include "dftl_settings.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define HASH_KEY_INITIAL 0
#define HASH_KEY_NONE 1
#define HASH_KEY_SAME 2
#define HASH_KEY_DIFF 3

#define FP_MAX -1
#define G_IDX(x) ((x) / GRAIN_PER_PAGE)
#define G_OFFSET(x) ((x) % GRAIN_PER_PAGE)
#define PPA_TO_PGA(_ppa_, _offset_) (((_ppa_) * GRAIN_PER_PAGE) + (_offset_))
#define IS_INFLIGHT(x) ((x) != NULL)
#define IS_INITIAL_PPA(x) ((x) == UINT32_MAX)
#define barrier() __asm__ __volatile__("" ::: "memory")
#define clock_get_ns()                            \
    ({                                            \
        struct timespec ts;                       \
        barrier();                                \
        clock_gettime(CLOCK_MONOTONIC, &ts);      \
        barrier();                                \
        ((uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec); \
    })

typedef enum
{
    READ,
    WRITE
} rw_t;

extern uint64_t timer_start_ns;

#define ftl_log(fmt, ...)                                                                               \
    do                                                                                                  \
    {                                                                                                   \
        uint64_t time_ns = clock_get_ns();                                                              \
        fprintf(stdout, "[%.6lf] [LOG] " fmt, (double)(time_ns - timer_start_ns) / 1e9, ##__VA_ARGS__); \
    } while (0)

#define ftl_err(fmt, ...)                                                                                                           \
    do                                                                                                                              \
    {                                                                                                                               \
        uint64_t time_ns = clock_get_ns();                                                                                          \
        fprintf(stderr, "[%.6lf] [ERROR] %s:%d:" fmt, (double)(time_ns - timer_start_ns) / 1e9, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)

#ifdef DEBUG_FTL
#define ftl_debug(fmt, ...)                                                                               \
    do                                                                                                    \
    {                                                                                                     \
        uint64_t time_ns = clock_get_ns();                                                                \
        fprintf(stdout, "[%.6lf] [DEBUG] " fmt, (double)(time_ns - timer_start_ns) / 1e9, ##__VA_ARGS__); \
    } while (0)

#define ftl_assert(expr) \
    do                   \
    {                    \
        assert(expr);    \
    } while (0)
#else
#define ftl_debug(fmt, ...) \
    do                      \
    {                       \
    } while (0)

#define ftl_assert(expr) \
    do                   \
    {                    \
    } while (0)
#endif // DEBUG_FTL

#define WARNING_NOTFOUND
static inline void warn_notfound(char *f, int l)
{
#ifdef WARNING_NOTFOUND
    printf("[WARNING] Read Target Data Not Found, at %s:%d\n", f, l);
#endif
}

// dftl_utils.c

int hash_collision_logging(int cnt, rw_t type);
void toggle_ssd_lat(bool on);
void busy_wait_ns(long nanoseconds);

#define LOG_INNER_TIME 0
#if LOG_INNER_TIME
void req_inner_time_log(request *req, enum inner_time_t type, bool on);
#else
#define req_inner_time_log(req, type, on) \
    do                                    \
    {                                     \
    } while (0)
#endif

#endif // DFTL_UTILS_H