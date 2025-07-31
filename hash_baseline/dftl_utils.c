#include "dftl_utils.h"
#include "dftl_types.h"
#include "dftl_cache.h"
#include "../lower/ssd.h"
#include "demand.h"
#include <pthread.h>

extern demand_cache d_cache;
extern struct ssd ssd_lower;
extern struct demand_env d_env;

uint64_t timer_start_ns;

int hash_collision_logging(int cnt, rw_t type) {
	if (cnt > MAX_HASH_COLLISION) {
		return 1;
	}

	switch (type) {
	case READ:
	    d_env.r_hash_collision_cnt[cnt]++;
		break;
	case WRITE:
		d_env.w_hash_collision_cnt[cnt]++;
		break;
	default:
		printf("[ERROR] No R/W type found, at %s:%d\n", __FILE__, __LINE__);
		abort();
	}
	return 0;
}

static inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtscp" : "=a" (lo), "=d" (hi) : : "%rcx");
    return ((uint64_t)hi << 32) | lo;
}

void busy_wait_ns(long nanoseconds) {
    if (nanoseconds <= 0) {
        return;
    }
    uint64_t start, end, cycles;
    double ns_per_cycle;
    // struct timespec res;

    // clock_gettime(CLOCK_MONOTONIC, &res);
    // ns_per_cycle = ((uint64_t)res.tv_sec * 1e9 + res.tv_nsec) / rdtsc();
    ns_per_cycle = 0.26370652481847;	// TODO: need configure by self
    cycles = nanoseconds / ns_per_cycle;

    start = rdtsc();
    do {
        end = rdtsc();
    } while ((end - start) < cycles);
}



void toggle_ssd_lat(bool on) {
	if (!on) {
		ssd_lower.sp.pg_wr_lat = 0;
		ssd_lower.sp.pg_rd_lat = 0;
		ssd_lower.sp.blk_er_lat = 0;
		ssd_lower.sp.ch_xfer_lat = 0;
		ftl_log("SSD Latency is turned off!\n");
	} else {
		ssd_lower.sp.pg_wr_lat = NAND_WRITE_LATENCY;
		ssd_lower.sp.pg_rd_lat = NAND_READ_LATENCY;
		ssd_lower.sp.blk_er_lat = NAND_ERASE_LATENCY;
		ssd_lower.sp.ch_xfer_lat = CHANNEL_XFER_LATENCY;
		ftl_log("SSD Latency is turned on!\n");
	}
}

#if LOG_INNER_TIME
void req_inner_time_log(request *req, enum inner_time_t type, bool on) {
	ftl_assert(type >= 0 && type < INNER_TIMER_SIZE);
	pthread_spin_lock(&req->timer_lock);
	if (on) {
		ftl_assert(req->inner_timer[type].is_start == false);
		req->inner_timer[type].start = clock_get_ns();
		req->inner_timer[type].is_start = true;
	} else {
		ftl_assert(req->inner_timer[type].is_start == true);
		req->inner_timer[type].is_start = false;
		req->inner_timer[type].elapsed += clock_get_ns() - req->inner_timer[type].start;
	}
	pthread_spin_unlock(&req->timer_lock);
}
#endif
