#ifndef __ALGO_Q_H__
#define __ALGO_Q_H__

#include <unistd.h>
#include <stdint.h>

typedef struct algo_queue_node {
    struct algo_queue_node *prev, *next;
    void *payload;
} algo_q_node;

typedef struct algo_queue {
    algo_q_node *head, *last;
    uint64_t size;
} algo_q;

algo_q *algo_q_create();
void algo_q_insert_sorted(algo_q *q, void *req, void *wbe);
void *algo_q_dequeue(algo_q *q);
void algo_q_free(algo_q *q);

#endif // __ALGO_Q_H__