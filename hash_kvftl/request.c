#include "request.h"
#include "cache.h"
#include "dftl_types.h"
#include "dftl_utils.h"
#include "../tools/sha256.h"
#include "../tools/murmurhash.h"
#include "../tools/valueset.h"
#include "write_buffer.h"
#include "demand.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>

uint32_t hashing_key(char *key, uint8_t len)
{
    uint32_t hash_key;
    MurmurHash3_x86_32(key, len, 1, &hash_key);
    return hash_key;
}

fp_t hashing_key_fp(char *key, uint8_t len)
{
    fp_t hash_key_fp;
    MurmurHash3_x86_32(key, len, 8, &hash_key_fp);
    return hash_key_fp;
}

// uint32_t hashing_key(char *key, uint8_t len) {
//     char *string;
//     Sha256Context ctx;
//     SHA256_HASH hash;
//     int bytes_arr[8];
//     uint32_t hashkey;

//     string = key;

//     Sha256Initialise(&ctx);
//     Sha256Update(&ctx, (unsigned char *)string, len);
//     Sha256Finalise(&ctx, &hash);

//     for (int i = 0; i < 8; i++) {
//         bytes_arr[i] =
//             ((hash.bytes[i * 4] << 24) | (hash.bytes[i * 4 + 1] << 16) |
//              (hash.bytes[i * 4 + 2] << 8) | (hash.bytes[i * 4 + 3]));
//     }

//     hashkey = bytes_arr[0];
//     for (int i = 1; i < 8; i++) {
//         hashkey ^= bytes_arr[i];
//     }

//     return hashkey;
// }

// fp_t hashing_key_fp(char *key, uint8_t len) {
//     char *string;
//     Sha256Context ctx;
//     SHA256_HASH hash;
//     int bytes_arr[8];
//     fp_t hashkey;

//     string = key;

//     Sha256Initialise(&ctx);
//     Sha256Update(&ctx, (unsigned char *)string, len);
//     Sha256Finalise(&ctx, &hash);

//     for (int i = 0; i < 8; i++) {
//         bytes_arr[i] =
//             ((hash.bytes[i * 4]) | (hash.bytes[i * 4 + 1] << 8) |
//              (hash.bytes[i * 4 + 2] << 16) | (hash.bytes[i * 4 + 3] << 24));
//     }

//     hashkey = bytes_arr[0];
//     for (int i = 1; i < 8; i++) {
//         hashkey ^= bytes_arr[i];
//     }

//     return (fp_t)(hashkey % (1ULL << (sizeof(fp_t) * 8)));
// }

/* make hash params if request has no valid hash params */
hash_params *make_hash_params(request *const req)
{
    struct hash_params *h_params =
        (struct hash_params *)malloc(sizeof(struct hash_params));
    h_params->hash = hashing_key(req->key.key, req->key.len);
#ifdef STORE_KEY_FP
    h_params->key_fp = hashing_key_fp(req->key.key, req->key.len);
#endif
    h_params->cnt = 0;
    h_params->find = HASH_KEY_INITIAL;
    return h_params;
}

inflight_params *get_iparams(request *const req, snode *wb_entry)
{
    struct inflight_params *i_params;
    if (req)
    {
        if (!req->params)
            req->params = malloc(sizeof(struct inflight_params));
        i_params = (struct inflight_params *)req->params;
    }
    else if (wb_entry)
    {
        if (!wb_entry->params)
            wb_entry->params = malloc(sizeof(struct inflight_params));
        i_params = (struct inflight_params *)wb_entry->params;
    }
    else
    {
        abort();
    }
    return i_params;
}

void free_iparams(request *const req, snode *wb_entry)
{
    if (req && req->params)
    {
        free(req->params);
        req->params = NULL;
    }
    else if (wb_entry && wb_entry->params)
    {
        free(wb_entry->params);
        wb_entry->params = NULL;
    }
}