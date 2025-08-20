#ifndef FIO_ZIPF_H
#define FIO_ZIPF_H

#include <inttypes.h>
#include "rand.h"

struct zipf_state
{
	uint64_t nranges;
	double theta;
	double zeta2;
	double zetan;
	double pareto_pow;
	struct frand_state rand;
	uint64_t rand_off;
	bool disable_hash;
	uint64_t *shuffle_map;
};

void zipf_init(struct zipf_state *zs, uint64_t nranges, double theta,
			   double center, unsigned int seed);
uint64_t zipf_next(struct zipf_state *zs);
void zipf_disable_hash(struct zipf_state *zs);
uint64_t *create_shuffle_map(uint64_t nranges);
void zipf_use_shuffle_map(struct zipf_state *dst_zs, uint64_t *shuffle_map);
#endif