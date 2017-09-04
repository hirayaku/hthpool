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

struct worklist_attr {
    int     trigger;
    size_t  concurrency;
    work_item empty_event, full_event;
};
typedef struct worklist_attr worklist_attr;

struct worklist {
    work_item* queue;
    size_t head, tail;
    size_t qsize;
    status_t status;
    pthread_mutex_t  mutex_head, mutex_tail;
    pthread_cond_t   cond_nonempty, cond_nonfull;
    worklist_attr* attr;
};
typedef struct worklist _hthp_worklist;

static void worklistattr_init (worklist_attr *attr);
static inline void worklistattr_setconcurrency (worklist_attr *attr,
                                                size_t concurrency);
static inline void worklistattr_setevent (worklist_attr *attr,
                                          work_item empty_event,
                                          work_item full_event);

static int  worklist_init (_hthp_worklist* wl, size_t size,
                           worklist_attr *attr);
static void worklist_reset (_hthp_worklist* wl);
static void worklist_destroy (_hthp_worklist* wl);
static void worklist_stop (_hthp_worklist* wl);
static int worklist_add(_hthp_worklist* wl, work_item item);
static work_item worklist_take (_hthp_worklist* wl);

/* empty task which does nothing */
static void* _wl_dry_run(void* arg) {
    return NULL;
}
const work_item _wl_empty_item = { (task) _wl_dry_run, NULL };

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

/* -----------------------------------------------------------------------
 * API for worklist and worklistattr
 * -----------------------------------------------------------------------
 */
static void  worklistattr_init (worklist_attr* attr) {
    attr->trigger = 0;
    attr->concurrency = 0;
    attr->full_event  = _wl_empty_item;
    attr->empty_event = _wl_empty_item;
}

static inline void worklistattr_setconcurrency (worklist_attr *attr,
                                                size_t concurrency)
{
    attr->concurrency = concurrency;
    attr->trigger = 1;
}

static inline void worklistattr_setevent (worklist_attr *attr,
                                          work_item empty_event,
                                          work_item full_event)
{
    attr->empty_event = empty_event;
    attr->full_event = full_event;
    attr->trigger = 1;
}

static inline void set_stop(_hthp_worklist *wl) {
    wl->status.stop = 1;
}
static inline void clear_status(_hthp_worklist *wl) {
    wl->status.stop     = 0;
    wl->status.adding   = 0;
    wl->status.taking   = 0;
}

/* initialize a worklist, MT-unsafe
 * arg:
 * @wl            the worklist to be initialized
 * @size          size of the worklist, excluding sentinel nodes
 * return:
 * 0              successful
 * -1             error creating synchronization variables
 * -2             cannot allocate space for array
 */
static int worklist_init(_hthp_worklist *wl, size_t size, 
                         worklist_attr* attr)
{
    if (size == 0)
        size = DEFAULT_SIZE;

    if (pthread_mutex_init (&wl->mutex_head, NULL)  ||
        pthread_mutex_init (&wl->mutex_tail, NULL)  ||
        pthread_cond_init (&wl->cond_nonempty, NULL)||
        pthread_cond_init (&wl->cond_nonfull, NULL)
       )
    {
        perror ("Create worklist synchronization variables");
        return -1;
    }

    wl->head    = 0;
    wl->tail    = 1;
    wl->qsize   = size + 2;   /* including head and tail sentinel nodes */
    clear_status (wl);
    wl->queue = (work_item*) malloc (wl->qsize * sizeof(work_item));
    wl->attr = (worklist_attr*) malloc (sizeof(worklist_attr));
    if (wl->queue == NULL || wl->attr == NULL)
        return -2;
    memcpy(wl->attr, attr, sizeof(worklist_attr));
    return 0;
}

/* reset worklist states, MT-unsafe */
static void worklist_reset(_hthp_worklist* wl) {
    wl->head    = 0;
    wl->tail    = 1;
    clear_status (wl);
    memset (wl->queue, 0, sizeof(work_item) * wl->qsize);
}

/* destroy built-in worklist and associated sync variables, MT-unsafe
 * It is not safe to call this function if some threads are still blocked by
 * some mutexes and conds defined in this file.
 * No arg, return value.
 */
static void worklist_destroy(_hthp_worklist* wl) {
    free (wl->queue);
    wl->queue = NULL;
    free (wl->attr);
    if (pthread_mutex_destroy (&wl->mutex_head)     ||
        pthread_mutex_destroy (&wl->mutex_tail)     ||
        pthread_cond_destroy (&wl->cond_nonempty)   ||
        pthread_cond_destroy (&wl->cond_nonfull)
       )
    {
        perror ("Destroy worklist synchronization variables");
    }
}

/* Stop current round of tasks */
static void worklist_stop(_hthp_worklist* wl) {
    pthread_mutex_lock (&wl->mutex_head);
    pthread_mutex_lock (&wl->mutex_tail);
    set_stop (wl);
    pthread_mutex_unlock (&wl->mutex_head);
    pthread_mutex_unlock (&wl->mutex_tail);
    pthread_cond_broadcast (&wl->cond_nonfull);
    pthread_cond_broadcast (&wl->cond_nonempty);
}

/* Blocking add work */
static int worklist_add(_hthp_worklist* wl, work_item item) {
    int registered = 0;
    /* If worklist is full, wait on cond_nonfull */
    pthread_mutex_lock (&wl->mutex_tail);
    while ((wl->tail + 1) % wl->qsize == wl->head) {
        if (!registered) {
            registered = 1;
            wl->status.adding++;
            if (wl->attr && wl->attr->trigger &&
                (wl->status.adding == wl->attr->concurrency))
            {
                pthread_mutex_unlock (&wl->mutex_tail);
                wl->attr->full_event.run (wl->attr->full_event.arg);
                pthread_mutex_lock (&wl->mutex_tail);
            }
        }
        if (wl->status.stop) {
            pthread_mutex_unlock (&wl->mutex_tail);
            return -1;
        }
        pthread_cond_wait (&wl->cond_nonfull, &wl->mutex_tail);
    }
    if (registered)
        wl->status.adding--;
    /* not full now, append item and signal cond_nonempty */
    wl->queue[wl->tail] = item;
    wl->tail = (wl->tail + 1) % wl->qsize;
    pthread_mutex_unlock (&wl->mutex_tail);
    pthread_cond_signal (&wl->cond_nonempty);
    return 0;
}

/* Blocking take work */
static work_item worklist_take (_hthp_worklist* wl) {
    int registered = 0;
    work_item item;
    /* If worklist is empty, wait on cond_nonempty */
    pthread_mutex_lock (&wl->mutex_head);
    while ((wl->head + 1) % wl->qsize == wl->tail) {
        if (!registered) {
            registered = 1;
            wl->status.taking++;
            if (wl->attr && wl->attr->trigger &&
                wl->status.taking == wl->attr->concurrency)
            {
                pthread_mutex_unlock (&wl->mutex_head);
                wl->attr->empty_event.run (wl->attr->empty_event.arg);
                pthread_mutex_lock (&wl->mutex_head);
            }
        }
        if (wl->status.stop) {
            pthread_mutex_unlock (&wl->mutex_head);
            return _wl_empty_item;
        }
        pthread_cond_wait (&wl->cond_nonempty, &wl->mutex_head);
    }
    if (registered)
        wl->status.taking--;
    /* not empty now, poll item and signal cond_nonfull (if block any) */
    wl->head = (wl->head + 1) % wl->qsize;
    item = wl->queue[wl->head];
    pthread_mutex_unlock (&wl->mutex_head);
    pthread_cond_signal (&wl->cond_nonfull);
    return item;
}

