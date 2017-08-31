/* worklist.c
 * Worklist supports concurrent work_item enqueue and dequeue
 * Two mutexes are used: one for enqueue and the other for dequeue
 * One disadvantage of using an array for worklist is once the worklist is full
 * and no threads are consuming tasks, all threads will be blocked.
 * Solutions can be either using linked lists or dynamicly changing the size.
 *
 * TODO:
 *  - dynamicly allocate new space for worklist when it's (almost) full
 *  - provide a general worklist generation utility
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include "worklist.h"
#define DEFAULT_SIZE 65535

static work_item *worklist = NULL;
static size_t head = 0, tail = 1;
static size_t qsize = 0;
static pthread_mutex_t  head_mutex, tail_mutex;
static pthread_cond_t   nonempty_cond, nonfull_cond;
volatile int _hthp_terminate = 0;

static inline void set_terminate_flag(void) {
    _hthp_terminate = 1;
    __sync_synchronize();
}

static inline void clear_terminate_flag(void) {
    _hthp_terminate = 0;
    __sync_synchronize();
}

/* initialize the built-in worklist array */
int worklist_init(size_t size) {
    if (size == 0)
        size = DEFAULT_SIZE;

    pthread_mutex_init (&head_mutex, NULL);
    pthread_mutex_init (&tail_mutex, NULL);
    pthread_cond_init (&nonempty_cond, NULL);
    pthread_cond_init (&nonfull_cond, NULL);
    head = 0;
    tail = 1;
    clear_terminate_flag();
    qsize = size + 2;   /* including head and tail sentinel nodes */
    worklist = (work_item*) malloc (qsize * sizeof(work_item));
    if (worklist == NULL) {
        return -1;
    } else return 0;
}

void worklist_reset(void) {
    head = 0;
    tail = 1;
    clear_terminate_flag();
    memset (worklist, 0, sizeof(work_item) * qsize);
}

/* destroy built-in worklist and associate sync variables
 * It is not safe to call this function if some threads are still blocked by
 * any mutexes and conds defined in this file.
 */
void worklist_destroy(void) {
    pthread_mutex_destroy (&head_mutex);
    pthread_mutex_destroy (&tail_mutex);
    pthread_cond_destroy (&nonempty_cond);
    pthread_cond_destroy (&nonfull_cond);
    free (worklist);
    worklist = NULL;
}

/* Terminate current round of tasks */
void worklist_terminate(void) {
    set_terminate_flag();
}

int worklist_status(void) {
    return _hthp_terminate;
}

/* API for enqueueing new tasks */
void worklist_append(work_item item) {
    /* If terminate flag is set, wake up all polling threads to exit */
    if (_hthp_terminate) {
        pthread_cond_broadcast (&nonempty_cond);
        return;
    }
    /* If worklist is full, wait on nonfull_cond */
    pthread_mutex_lock (&tail_mutex);
    while ((tail + 1) % qsize == head) {
        pthread_cond_wait (&nonfull_cond, &tail_mutex);
        if (_hthp_terminate) {
            pthread_mutex_unlock (&tail_mutex);
            return;
        }
    }
    /* not full now, append item and signal nonempty_cond */
    worklist[tail] = item;
    tail = (tail + 1) % qsize;
    pthread_mutex_unlock (&tail_mutex);
    pthread_cond_signal (&nonempty_cond);
}

/* API for dequeueing tasks */
work_item worklist_poll(void) {
    work_item item;
    /* If terminate flag is set, wake up all appending threads to exit */
    if (_hthp_terminate) {
        pthread_cond_broadcast (&nonfull_cond);
        return;
    }
    /* If worklist is empty, wait on nonempty_cond */
    pthread_mutex_lock (&head_mutex);
    while ((head + 1) % qsize == tail) {
        pthread_cond_wait (&nonempty_cond, &head_mutex);
        if (_hthp_terminate) {
            pthread_mutex_unlock (&head_mutex);
            return;
        }
    }
    /* not empty now, poll item and signal nonfull_cond (if block any) */
    head = (head + 1) % qsize;
    item = worklist[head];
    pthread_mutex_unlock (&head_mutex);
    pthread_cond_signal (&nonfull_cond);
    return item;
}

/* Check if the worklist is almost full:
 * return nonzero if there are over BUSY_THR% (now fixed to 90%) slots occupied
 * This does not take pending tasks into consideration
 */
int worklist_busy(void) {
    int occupied;
    if (head < tail) {
        occupied = tail - head - 1;
    } else {
        occupied = tail + qsize -1;
    }
    return ( ((occupied / (double)(qsize - 2)) >= 0.9)?1:0 );
}
