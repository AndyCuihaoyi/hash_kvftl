#include "algo_queue.h"
#include "dftl_utils.h"
#include "../tools/skiplist.h"
#include <assert.h>
#include <stdlib.h>

algo_q *algo_q_create() {
    algo_q *q = malloc(sizeof(algo_q));
    q->head = q->last = NULL;
    q->size = 0;
    return q;
}

void algo_q_free(algo_q *q) {
    while (q->head) {
        algo_q_node *tmp = q->head;
        q->head = q->head->next;
        free(tmp);
    }
    q->last = NULL;
    free(q);
}

void algo_q_insert_sorted(algo_q *q, void *req, void *wbe) {
    /**
     * Requests are placed in @work_queue sorted by their target time.
     * @work_queue is statically allocated and the ordered list is
     * implemented by chaining the indexes of entries with @prev and @next.
     * This implementation is nasty but we do this way over dynamically
     * allocated linked list to minimize the influence of dynamic memory
     * allocation. Also, this O(n) implementation can be improved to O(logn)
     * scheme with e.g., red-black tree but....
     */

    if (!q) {
        abort();
    }

    if (q->head == NULL) {
        ftl_assert(q->last == NULL);
        algo_q_node *entry = malloc(sizeof(algo_q_node));
        if (req) {
            entry->payload = req;
        } else if (wbe) {
            entry->payload = wbe;
        } else {
            abort();
        }
        entry->prev = entry->next = NULL;
        q->head = q->last = entry;
        ftl_assert(q->head->prev == NULL);
    } else {
        uint64_t nsecs_target = 0;
        algo_q_node *curr = q->last;
        algo_q_node *entry = malloc(sizeof(algo_q_node));
        entry->prev = entry->next = NULL;
        if (req) {
            nsecs_target = ((request *)req)->etime + 1000; // 1us
            entry->payload = req;
        } else if (wbe) {
            nsecs_target = ((snode *)wbe)->etime + 1000; // 1us
            entry->payload = wbe;
        } else {
            abort();
        }

        if (req) {
            while (curr != NULL) {
                if (((request *)curr->payload)->etime <= ((request *)req)->etime)
                    break;

                if (((request *)curr->payload)->etime <= nsecs_target)
                    break;

                curr = curr->prev;
            }
        } else if (wbe) {
            while (curr != NULL) {
                if (((snode *)curr->payload)->etime <= ((snode *)wbe)->etime)
                    break;

                if (((snode *)curr->payload)->etime <= nsecs_target)
                    break;

                curr = curr->prev;
            }
        } else {
            abort();
        }

        if (curr == NULL) { /* Head inserted */
            entry->prev = NULL;
            q->head->prev = entry;
            entry->next = q->head;
            q->head = entry;
            ftl_assert(q->head->next != NULL);
        } else if (curr->next == NULL) { /* Tail */
            entry->prev = curr;
            entry->next = curr->next;
            curr->next = entry;
            q->last = entry;
        } else { /* In between */
            entry->prev = curr;
            entry->next = curr->next;
            curr->next->prev = entry;
            curr->next = entry;
        }
    }
    q->size++;
}

void *algo_q_dequeue(algo_q *q) {
    if (!q) {
        abort();
    }

    if (q->head == NULL) {
        return NULL;
    }

    algo_q_node *entry = q->head;
    q->head = q->head->next;
    if (q->head) {
        q->head->prev = NULL;
    }
    void *payload = entry->payload;

    if (q->head == NULL) {
        q->last = NULL;
    }
    q->size--;

    free(entry);
    return payload;
}
