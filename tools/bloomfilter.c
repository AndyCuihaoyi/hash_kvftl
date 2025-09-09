#include "bloomfilter.h"
// #include"../../bench/bench.h"
// #include "../../include/sha256-arm.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "murmurhash.h"
#define FORCE_INLINE inline
#define MURMUR_HASH
void BITSET(char *input, char offset)
{
    char test = 1;
    test <<= offset;
    (*input) |= test;
}
bool BITGET(char input, char offset)
{
    char test = 1;
    test <<= offset;
    return input & test;
}

uint32_t hashfunction(uint32_t key)
{
    key = ~key + (key << 15); // key = (key << 15) - key - 1;
    key = key ^ (key >> 12);
    key = key + (key << 2);
    key = key ^ (key >> 4);
    key = key * 2057; // key = (key + (key << 3)) + (key << 11);
    key = key ^ (key >> 16);
    return key;
}

BF *bf_init(int entry, float fpr)
{
    if (entry <= 0 || fpr <= 0 || fpr > 1)
        return NULL;
    BF *res = (BF *)malloc(sizeof(BF));
    res->valid_entry = 0;
    res->nr_entry = entry;
    res->m = ceil((res->nr_entry * log(fpr)) / log(1.0 / (pow(2.0, log(2.0)))));
    res->nr_hash = round(log(2.0) * (float)res->m / res->nr_entry);
    if (res->nr_hash < 1)
        res->nr_hash = 1;
    int targetsize = res->m / 8;
    if (res->m % 8)
        targetsize++;
    res->body = (char *)malloc(targetsize);
    if (!res->body)
    {
        free(res);
        return NULL;
    }
    memset(res->body, 0, targetsize);
    res->fpr = fpr;
    res->targetsize = targetsize;
    return res;
}

void bf_reset(BF *bf)
{
    bf->valid_entry = 0;
    memset(bf->body, 0, bf->targetsize);
}

/*calculate fpr based on memory size(Byte)*/
float bf_fpr_from_memory(int entry, uint32_t memory)
{
    int n = entry;
    int m = memory * 8;
    int k = round(log(2.0) * (double)m / n);
    float p = pow(1 - exp((double)-k / (m / n)), k);
    return p;
}

void bf_set(BF *input, lpa_t lpa)
{
    if (input == NULL)
    {
        abort();
    }
    uint32_t h;
    int block;
    int offset;
    for (uint32_t i = 0; i < input->nr_hash; i++)
    {
        uint32_t out;
#ifdef MURMUR_HASH
        MurmurHash3_x86_32((const char *)&lpa, sizeof(lpa), i, &out);
        h = out % input->m;
#else
        h = hashfunction(lpa);
        h %= input->m;
#endif
        block = h / 8;
        offset = h % 8;

        BITSET(&input->body[block], offset);
    }
}

/*existence check*/
bool bf_check(BF *input, lpa_t lpa)
{
    uint32_t h;
    int block, offset;
    if (input == NULL)
        return true;

    for (uint32_t i = 0; i < input->nr_hash; i++)
    {
        uint32_t out;
#ifdef MURMUR_HASH
        MurmurHash3_x86_32((const char *)&lpa, sizeof(lpa), i, &out);
        h = out % input->m;
#else
        h = hashfunction(lpa);
        h %= input->m;
#endif
        block = h / 8;
        offset = h % 8;

        if (!BITGET(input->body[block], offset))
        {
            return false;
        }
    }
    return true;
}

void bf_free(BF *input)
{
    if (!input)
        return;
    free(input->body);
    free(input);
}
