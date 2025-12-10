#define DVALUE
// #define DEBUG_FTL

#define MAX_INF_REQS (65536)

#define MAX_HASH_COLLISION (1024)

#define KVSSD
#define MAXKEYSIZE 32 // in bytes
#define MAX_WRITE_BUF 256

#define K 1024
#define M (1024 * K)
#define G (1024 * M)
#define PIECE 1024
#define GRAINED_UNIT PIECE // 1024
#define PAGESIZE 4096
#define GRAIN_PER_PAGE (PAGESIZE / GRAINED_UNIT) // 4
#define NPCINPAGE (PAGESIZE / PIECE)             // 4

#define _NOP (18481152) // 70.5GB 4KB page (10% OP for 64GB logical space)
#define _PPS (1 << 15)  // 128MB
#define _NOS (_NOP / _PPS)

#define _NOP_NO_OP ((1 << 24)) // 64GB

#define STORE_KEY_FP
#define UPDATE_DATA_CHECK

// #define CMT_USE_NUMA
