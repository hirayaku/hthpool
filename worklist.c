/* worklist.c
 * Worklist supports concurrent work_item enqueue and dequeue
 * Two mutexes are used: one for enqueue and the other for dequeue
 * One disadvantage of using an array for worklist is once the worklist is full
 * and no threads are consuming tasks, all threads will be blocked.
 * Solutions can be either using linked lists or dynamicly changing the size.
 *
 * TODO:
 *  - dynamicly allocate new space for worklist when it's (almost) full
 *  - provide a general worklist utility
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <pthread.h>
#include <sys/types.h>
#include "common.h"
#define DEFAULT_SIZE 65533
#define DEFAULT_CONCURRENCY INT_MAX

static work_item *worklist = NULL;
static size_t head = 0, tail = 1;
static size_t qsize = 0;
static pthread_mutex_t  mutex_head, mutex_tail;
static pthread_cond_t   cond_nonempty, cond_nonfull;
pthread_cond_t          cond_ext_empty, cond_ext_full;
static int appending = 0, polling = 0;
static int MAX_CONCURRENCY = 0;
static volatile status_t status;

static inline void set_stop_flag(void) {
    status.stop = 1;
}

static inline void clear_status(void) {
    status.stop = 0;
    status.empty = 0;
    status.full = 0;
}

/* empty task which does nothing */
static void* dry_run(void* arg) {
    return NULL;
}
const work_item _hthp_empty_task = { (task) dry_run, NULL };

static inline int work_item_comp(const work_item *item1, const work_item *item2) {
    return !(item1->arg == item2->arg &&
            item1->run == item2->run);
}

/* initialize the built-in worklist array */
int worklist_init(size_t size, size_t max_concurrency) {
    if (size == 0)
        size = DEFAULT_SIZE;
    if (max_concurrency == 0) {
        MAX_CONCURRENCY = DEFAULT_CONCURRENCY;
    } else MAX_CONCURRENCY = max_concurrency;

    pthread_mutex_init (&mutex_head, NULL);
    pthread_mutex_init (&mutex_tail, NULL);
    pthread_cond_init (&cond_nonempty, NULL);
    pthread_cond_init (&cond_nonfull, NULL);
    pthread_cond_init (&cond_ext_empty, NULL);
    pthread_cond_init (&cond_ext_full, NULL);
    head = 0;
    tail = 1;
    qsize       = size + 2;   /* including head and tail sentinel nodes */
    appending   = 0;
    polling     = 0;
    clear_status ();
    worklist = (work_item*) malloc (qsize * sizeof(work_item));
    if (worklist == NULL) {
        return -1;
    } else return 0;
}

/* reset worklist states, MT-unsafe */
void worklist_reset(void) {
    head = 0;
    tail = 1;
    appending   = 0;
    polling     = 0;
    clear_status ();
    memset (worklist, 0, sizeof(work_item) * qsize);
}

/* destroy built-in worklist and associated sync variables
 * It is not safe to call this function if some threads are still blocked by
 * some mutexes and conds defined in this file.
 */
void worklist_destroy(void) {
    pthread_mutex_destroy (&mutex_head);
    pthread_mutex_destroy (&mutex_tail);
    pthread_cond_destroy (&cond_nonempty);
    pthread_cond_destroy (&cond_nonfull);
    pthread_cond_destroy (&cond_ext_empty);
    pthread_cond_destroy (&cond_ext_full);
    free (worklist);
    worklist = NULL;
}

/* Terminate current round of tasks */
void worklist_stop(void) {
    pthread_mutex_lock (&mutex_head);
    pthread_mutex_lock (&mutex_tail);
    set_stop_flag();
    pthread_mutex_unlock (&mutex_head);
    pthread_mutex_unlock (&mutex_tail);
    pthread_cond_broadcast (&cond_nonfull);
    pthread_cond_broadcast (&cond_nonempty);
}

status_t worklist_status(void) {
    return status;
}

/* API for enqueueing new tasks */
int worklist_append(work_item item) {
    assert (work_item_comp (&item, &_hthp_empty_task));
    /* If worklist is full, wait on cond_nonfull */
    pthread_mutex_lock (&mutex_tail);
    appending++;
    while ((tail + 1) % qsize == head) {
        if (status.stop) {
            pthread_mutex_unlock (&mutex_tail);
            return -1;
        }
        if (appending == MAX_CONCURRENCY) {
            appending--;
            pthread_mutex_unlock (&mutex_tail);
            pthread_cond_broadcast (&cond_ext_full);
            return -2;
        }
        pthread_cond_wait (&cond_nonfull, &mutex_tail);
    }
    /* not full now, append item and signal cond_nonempty */
    worklist[tail] = item;
    tail = (tail + 1) % qsize;
    appending--;
    pthread_mutex_unlock (&mutex_tail);
    pthread_cond_signal (&cond_nonempty);
    return 0;
}

/* API for dequeueing tasks */
work_item worklist_poll(void) {
    work_item item = _hthp_empty_task;
    /* If worklist is empty, wait on cond_nonempty */
    pthread_mutex_lock (&mutex_head);
    polling++;
    while ((head + 1) % qsize == tail) {
        if (status.stop) {
            pthread_mutex_unlock (&mutex_head);
            return item;
        }
        if (polling == MAX_CONCURRENCY) {
            polling--;
            pthread_mutex_unlock (&mutex_head);
            pthread_cond_broadcast (&cond_ext_empty);
            return item;
        }
        pthread_cond_wait (&cond_nonempty, &mutex_head);
    }
    /* not empty now, poll item and signal cond_nonfull (if block any) */
    head = (head + 1) % qsize;
    item = worklist[head];
    polling--;
    pthread_mutex_unlock (&mutex_head);
    pthread_cond_signal (&cond_nonfull);
    return item;
}

/* Check if the worklist is almost full:
 * return nonzero if there are over BUSY_THR% (now fixed to 90%) slots occupied
 * This does not take pending tasks into consideration
 */
int worklist_busy(void) {
    int occupied;
    if (head < tail) {
        occupied = tail - head - 1; } else {
        occupied = tail + qsize -1;
    }
    return ( ((occupied / (double)(qsize - 2)) >= 0.9)?1:0 );
}
