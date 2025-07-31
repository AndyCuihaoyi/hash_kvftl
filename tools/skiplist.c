#include "skiplist.h"
#include "valueset.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

extern S_KEYT key_max, key_min;
#define demand

#define KEYLEN(a) (a.len + sizeof(ppa_t))

static inline int KEYCMP(KEYT a, KEYT b) {
    if (!a.len && !b.len)
        return 0;
    else if (a.len == 0)
        return -1;
    else if (b.len == 0)
        return 1;

    int r = memcmp(a.key, b.key, a.len > b.len ? b.len : a.len);
    if (r != 0 || a.len == b.len) {
        return r;
    }
    return a.len < b.len ? -1 : 1;
}

static inline char KEYTEST(KEYT a, KEYT b) {
    if (a.len != b.len)
        return 0;
    return memcmp(a.key, b.key, a.len) ? 0 : 1;
}

skiplist *skiplist_init() {
    skiplist *point = (skiplist *)malloc(sizeof(skiplist));
    point->level = 1;
    point->header = (snode *)malloc(sizeof(snode));
    point->header->list = (snode **)malloc(sizeof(snode *) * (MAX_L + 1));
    for (int i = 0; i < MAX_L; i++)
        point->header->list[i] = point->header;
    // back;
    point->header->back = point->header;

#if defined(KVSSD)
    point->all_length = 0;
    point->header->key = key_max;
#else
    point->header->key = UINT_MAX;
    point->start = UINT_MAX;
    point->end = 0;
#endif
    point->header->value = NULL;
    point->size = 0;
    return point;
}

snode *skiplist_find(skiplist *list, S_KEYT key) {
    if (!list)
        return NULL;
    if (list->size == 0)
        return NULL;
    snode *x = list->header;
    for (int i = list->level; i >= 1; i--) {
#if defined(KVSSD)
        while (KEYCMP(x->list[i]->key, key) < 0)
#else
        while (x->list[i]->key < key)
#endif
            x = x->list[i];
    }

#if defined(KVSSD)
    if (KEYTEST(x->list[1]->key, key))
#else
    if (x->list[1]->key == key)
#endif
        return x->list[1];
    return NULL;
}

snode *skiplist_find_lowerbound(skiplist *list, S_KEYT key) {
    if (!list)
        return NULL;
    if (list->size == 0)
        return NULL;
    snode *x = list->header;
    for (int i = list->level; i >= 1; i--) {
#if defined(KVSSD)
        while (KEYCMP(x->list[i]->key, key) < 0)
#else
        while (x->list[i]->key < key)
#endif
            x = x->list[i];
    }

    return x->list[1];
}

static int getLevel() {
    int level = 1;
    int temp = rand();
    while (temp % PROB == 1) {
        temp = rand();
        level++;
        if (level + 1 >= MAX_L)
            break;
    }
    return level;
}

// extern bool testflag;
snode *skiplist_insert(skiplist *list, S_KEYT key, value_set *value,
                       bool deletef) {
    snode *update[MAX_L + 1];
    snode *x = list->header;
    for (int i = list->level; i >= 1; i--) {
#if defined(KVSSD)
        while (KEYCMP(x->list[i]->key, key) < 0)
#else
        while (x->list[i]->key < key)
#endif
            x = x->list[i];
        update[i] = x;
    }
    x = x->list[1];
    if (value != NULL) {
        value->length =
            (value->length / PIECE) + (value->length % PIECE ? 1 : 0);
    }
#if defined(KVSSD)
    if (KEYTEST(key, x->key))
#else
    if (key == x->key)
#endif
    {
        list->data_size -= (x->value->length * PIECE);
        list->data_size += (value->length * PIECE);
        if (x->value)
            inf_free_valueset(&x->value);

        x->value = value;
        x->isvalid = deletef;
        return x;
    } else {
        int level = getLevel();
        if (level > list->level) {
            for (int i = list->level + 1; i <= level; i++) {
                update[i] = list->header;
            }
            list->level = level;
        }
        x = (snode *)malloc(sizeof(snode));
        x->list = (snode **)malloc(sizeof(snode *) * (level + 1));

        x->key = key;
        x->isvalid = deletef;

        x->ppa = UINT_MAX;
        x->value = value;

#ifdef KVSSD
        list->all_length += KEYLEN(key);
#endif

#ifdef demand
        x->lpa = UINT32_MAX;
        x->hash_params = NULL;
        x->params = NULL;
#endif

        for (int i = 1; i <= level; i++) {
            x->list[i] = update[i]->list[i];
            update[i]->list[i] = x;
        }

        // new back
        x->back = x->list[1]->back;
        x->list[1]->back = x;

        x->level = level;
        list->size++;
        list->data_size += (value->length * PIECE);
    }
    return x;
}

snode *skiplist_at(skiplist *list, int idx) {
    snode *header = list->header;
    for (int i = 0; i < idx; i++) {
        header = header->list[1];
    }
    return header;
}

int skiplist_delete(skiplist *list, S_KEYT key) {
    if (list->size == 0)
        return -1;
    snode *update[MAX_L + 1];
    snode *x = list->header;
    for (int i = list->level; i >= 1; i--) {
#if defined(KVSSD)
        while (KEYCMP(x->list[i]->key, key) < 0)
#else
        while (x->list[i]->key < key)
#endif
            x = x->list[i];
        update[i] = x;
    }
    x = x->list[1];

#if defined(KVSSD)
    if (KEYCMP(x->key, key) != 0)
#else
    if (x->key != key)
#endif
        return -2;

    for (int i = x->level; i >= 1; i--) {
        update[i]->list[i] = x->list[i];
        if (update[i] == update[i]->list[i])
            list->level--;
    }
    free(x->list);
    free(x);
    list->size--;
    return 0;
}

sk_iter *skiplist_get_iterator(skiplist *list) {
    sk_iter *res = (sk_iter *)malloc(sizeof(sk_iter));
    res->list = list;
    res->now = list->header;
    return res;
}

snode *skiplist_get_next(sk_iter *iter) {
    if (iter->now->list[1] == iter->list->header) { // end
        return NULL;
    } else {
        iter->now = iter->now->list[1];
        return iter->now;
    }
}

// for test
void skiplist_dump(skiplist *list) {
    sk_iter *iter = skiplist_get_iterator(list);
    snode *now;
    while ((now = skiplist_get_next(iter)) != NULL) {
        for (uint32_t i = 1; i <= now->level; i++) {
#if defined(KVSSD)
#else
            printf("%u ", now->key);
#endif
        }
        printf("\n");
    }
    free(iter);
}

void skiplist_clear(skiplist *list) {
    snode *now = list->header->list[1];
    snode *next = now->list[1];
    while (now != list->header) {

        if (now->value) {
            inf_free_valueset(
                &now->value); // not only length<PAGESIZE also length==PAGESIZE,
                             // just free req from inf
        }
        // free(now->key.key);
        // now->key.key = NULL;
        free(now->list);
        now->list = NULL;
        free(now);
        now = next;
        next = now->list[1];
    }
    list->size = 0;
    list->level = 0;
    for (int i = 0; i < MAX_L; i++)
        list->header->list[i] = list->header;
#if defined(KVSSD)
    list->header->key = key_max;
#else
    list->header->key = INT_MAX;
#endif
}

void skiplist_free(skiplist *list) {
    if (list == NULL)
        return;
    skiplist_clear(list);
    free(list->header->list);
    free(list->header);
    free(list);
    return;
}