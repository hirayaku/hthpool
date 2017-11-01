/* NOTE:
 * In my machine using gcc, pthread barrier is not available when -std=c99
 * is used, -std=gnu99/_GNU_SOURCE macro will hopefully enable it, but causes
 * portability problems.
 * Using clang won't cause such problems. But in macOS, the problem exists.
 * The reason is, pthread_barrier is an optional extension to POSIX standard.
 */
#if defined(__GNUC__)
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include "common.h"
#include "worklist.h"
#define HTHPOOL_DEBUG

#ifdef HTHPOOL_DEBUG
typedef union {
    pthread_t pthread_id;
    unsigned long numeric;
} _hthp_tid;
/* Debug print function. Credit to
 * @unwind in StackOverflow "How to wrap printf() into a function or macro?"
 * This macro should be used with double parenthesis:
 * DBG_PRINT(("This is the debug info!"))
 */
# define DBG_PRINT(x) printf x
# define _HTHPOOL_TID(x) ( (x.numeric >> 12) & 0xfff )
#else
# define DBG_PRINT(x)
# define _HTHPOOL_TID(x)
#endif

/* worklist implementation
 * Worklist is basically a  concurrent work_item queue
 * Two mutexes are used: one for enqueue and the other for dequeue
 * One disadvantage of using an array for worklist is once the worklist is full
 * and no threads are consuming tasks, all threads will be blocked.
 * Solutions can be either using linked lists or dynamicly changing the size.
 *
 * TODO:
 *  - dynamicly allocate new space for worklist when it's (almost) full
 */

#define DEFAULT_SIZE 65533
struct status {
    int stop;
    int adding;
    int taking;
};
typedef struct status status_t;
typedef struct worklist _hthp_worklist;

#define WL_SIZE 4094
struct hthpool {
    _hthp_worklist* wl;
    pthread_t* pool;
    int thread_num;
    int stopped_threads, blocked_threads;
    int stop, close;
    work_item empty_event, full_event;
    pthread_mutex_t      mutex_stop_continue;
    pthread_cond_t       cond_all_stopped, cond_allow_go;
    pthread_barrier_t    barrier_continue;
};

/* This is the wrapper function for threads to acquire new item
 * from the work list, execute the task and then wait for new ones.
 * This function is passed into pthread_create during thread pool initialization
 * Always return NULL
 */
static void* daemon_run(void* arg) {
#ifdef HTHPOOL_DEBUG
    _hthp_tid tid;
    tid.pthread_id = pthread_self ();
#endif
    DBG_PRINT (("Thread 0x%lx starts\n", _HTHPOOL_TID (tid)));
    struct hthpool* pool_state = (struct hthpool*) arg;
    /* request task from task queue and execute */
    for(;;) {
        if (pool_state->stop) {
            /* After the thread detects `stop` flag, it will stuck at
             * `cond_allow_go` until issued a `continue` cond
             */
            pthread_mutex_lock (&pool_state->mutex_stop_continue);
            pool_state->stopped_threads++;
            /* The following wakes up the main thread in `hthpool_wait`,
             * but main thread won't be immediately active:
             * it still acquires `pool_state->mutex_stop_continue`
             * which is locked now
             */
            if (pool_state->stopped_threads == pool_state->thread_num) {
                pthread_cond_broadcast (&pool_state->cond_all_stopped);
            }
            /* Unlock `pool_state->mutex_stop_continue`
             * and wait for `hthpool_continue` or `hthpool_destroy`.
             */
            while (pool_state->blocked_threads == 0)
                pthread_cond_wait (&pool_state->cond_allow_go,
                                   &pool_state->mutex_stop_continue);
            /* thread count already reset/set by main thread */
            pool_state->blocked_threads--;
            DBG_PRINT (("%d threads remain blocked\n",
                        pool_state->blocked_threads));
            pthread_mutex_unlock (&pool_state->mutex_stop_continue);
            if (pool_state->close) {
                DBG_PRINT (("  Thread 0x%lx will be dead.\n",
                            _HTHPOOL_TID (tid)));
                break;
            }
            DBG_PRINT (("  Thread 0x%lx keeps alive.\n", _HTHPOOL_TID (tid)));
            pthread_barrier_wait (&pool_state->barrier_continue);
        }
        work_item item = worklist_take(pool_state->wl);
        item.run(item.arg);
    }
    return NULL;
}

/* --------------------------------------------------------------------
 * API which should only be called by the main thread (not in the pool)
 * --------------------------------------------------------------------
 */
/* Must be called before hthpool_init
 */
void hthpool_register(struct hthpool* pool_state,
                      work_item etask, work_item ftask) {
    pool_state->empty_event = etask;
    pool_state->full_event  = ftask;
}

/* Initialize a new threadpool
 */
struct hthpool* hthpool_init(int num, work_item etask, work_item ftask) {
    int wlret = 0, pret = 0, mret = 0;
    int i;
    struct hthpool* pool_state;
    if (num < 0)
        return NULL;

    pool_state = (struct hthpool*) malloc (sizeof(struct hthpool));
    if (pool_state == NULL)
        exit (EXIT_FAILURE);
    hthpool_register (pool_state, etask, ftask);

    pool_state->wl = (_hthp_worklist*) malloc (sizeof(_hthp_worklist));
    if (pool_state->wl == NULL)
        exit (EXIT_FAILURE);
    worklist_attr attr;
    worklistattr_init (&attr);
    worklistattr_setconcurrency (&attr, num);
    worklistattr_setevent (&attr,
                           pool_state->empty_event,
                           pool_state->full_event);
    wlret = worklist_init (pool_state->wl, WL_SIZE, &attr);

    pool_state->thread_num = num;
    pool_state->stopped_threads = 0;
    pool_state->blocked_threads = 0;
    pool_state->close = 0;
    if (pthread_mutex_init (&pool_state->mutex_stop_continue, NULL) ||
        pthread_cond_init (&pool_state->cond_all_stopped, NULL)     ||
        pthread_cond_init (&pool_state->cond_allow_go, NULL)        ||
        pthread_barrier_init (&pool_state->barrier_continue, NULL, num)
       )
    {
        perror ("Initialize synchronization variables");
        exit (EXIT_FAILURE);
    }

    mret = ( NULL ==
             (pool_state->pool =
              (pthread_t*) malloc (sizeof(pthread_t) * num))
           );
    if (wlret || mret) {
        worklist_destroy (pool_state->wl);
        free (pool_state->pool);
        exit (EXIT_FAILURE);
    }

    for (i = 0; i < num; i++) {
        pret = pthread_create(pool_state->pool + i, NULL,
                              daemon_run, (void*)pool_state);
        if (pret) {
            perror ("Create threads");
            exit (EXIT_FAILURE);
        }
    }

    return pool_state;
}

/* Deallocate the worklist, sync vars and join threads
 * return: void
 * exit code:
 *  -1      cannot destroy the synchronization vars
 *  -2      cannot join threads
 */
void hthpool_destroy(struct hthpool* pool_state) {
    int i, pret;
    void* ret;
    
    pthread_mutex_lock (&pool_state->mutex_stop_continue);
    pool_state->close = 1;
    pool_state->blocked_threads = pool_state->thread_num;
    DBG_PRINT (("Kill'em all!\n"));
    pthread_mutex_unlock (&pool_state->mutex_stop_continue);
    pthread_cond_broadcast (&pool_state->cond_allow_go);

    for (i = 0; i < pool_state->thread_num; i++) {
        pret = pthread_join (pool_state->pool[i], &ret);
        if (pret) {
            perror ("Join threads");
            exit (-2);
        }
    }
    free (pool_state->pool);
    if (pthread_mutex_destroy (&pool_state->mutex_stop_continue)    ||
        pthread_cond_destroy (&pool_state->cond_all_stopped)        ||
        pthread_cond_destroy (&pool_state->cond_allow_go)           ||
        pthread_barrier_destroy (&pool_state->barrier_continue)
       )
    {
        perror ("Destroy synchronization variables");
        exit(-1);
    }
    worklist_destroy (pool_state->wl);
    free (pool_state->wl);
    free (pool_state);
}

/* Wait until all threads are stopped */
void hthpool_wait(struct hthpool* pool_state) {
    pthread_mutex_lock (&pool_state->mutex_stop_continue);
    /* If all threads in the threadpool already stopped, no need to wait */
    while (pool_state->stopped_threads != pool_state->thread_num)
        pthread_cond_wait (&pool_state->cond_all_stopped,
                           &pool_state->mutex_stop_continue);
    DBG_PRINT (("All threads stopped\n"));
    pthread_mutex_unlock (&pool_state->mutex_stop_continue);
}

/* Make threadpool running again only after it's been stopped */
void hthpool_continue(struct hthpool* pool_state) {
    pthread_mutex_lock (&pool_state->mutex_stop_continue);
    pool_state->stop = 0;
    pool_state->stopped_threads = 0;
    pool_state->blocked_threads = pool_state->thread_num;
    worklist_reset (pool_state->wl);
    DBG_PRINT (("Threads, continue working!\n"));
    pthread_mutex_unlock (&pool_state->mutex_stop_continue);
    pthread_cond_broadcast (&pool_state->cond_allow_go);
}

/* ------------------------------------------------------------------------
 * API which can be called by either the main thread or threads in the pool
 * ------------------------------------------------------------------------
 */
int hthpool_submit(struct hthpool* pool_state, work_item item) {
    return worklist_add(pool_state->wl, item);
}

void hthpool_hard_stop(struct hthpool* pool_state) {
    DBG_PRINT (("Threads, immediately stop working!\n"));
    pool_state->stop = 1;
    worklist_stop (pool_state->wl);
}

void hthpool_soft_stop(struct hthpool* pool_state) {
    DBG_PRINT (("Threads, please stop working.\n"));
    pool_state->stop = 1;
}

