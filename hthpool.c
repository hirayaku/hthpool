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
#undef HTHPOOL_DEBUG

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
 *  - provide a general worklist utility
 */

#define DEFAULT_SIZE 65533
typedef struct {
    int stop;
    int adding;
    int taking;
} status_t;

static work_item *worklist = NULL;
static size_t head = 0, tail = 1;
static size_t qsize = 0;
static pthread_mutex_t  mutex_head, mutex_tail;
static pthread_cond_t   cond_nonempty, cond_nonfull;
static int adding = 0, taking = 0;
static status_t _wl_status;

static inline void set_stop(volatile status_t* sp) {
    sp->stop = 1;
}
static inline void clear_status(volatile status_t* sp) {
    sp->stop      = 0;
    sp->adding    = 0;
    sp->taking    = 0;
}

/* empty task which does nothing */
static void* _wl_dry_run(void* arg) {
    return NULL;
}
const work_item _wl_empty_item = { (task) _wl_dry_run, NULL };

static inline int
work_item_comp(const work_item *item1, const work_item *item2) {
    return !(item1->arg == item2->arg &&
            item1->run == item2->run);
}


static int thread_num;
static task empty_event, full_event;

/* initialize the built-in worklist array, MT-unsafe
 * arg:
 * @size          size of the worklist, excluding sentinel nodes
 * return:
 * 0              successful
 * -1             error creating synchronization variables
 * -2             cannot allocate space for array
 */
static int worklist_init(size_t size) {
    if (size == 0)
        size = DEFAULT_SIZE;

    if (pthread_mutex_init (&mutex_head, NULL)  ||
        pthread_mutex_init (&mutex_tail, NULL)  ||
        pthread_cond_init (&cond_nonempty, NULL)||
        pthread_cond_init (&cond_nonfull, NULL)
       )
    {
        perror ("Create worklist synchronization variables");
        return -1;
    }

    head    = 0;
    tail    = 1;
    qsize   = size + 2;   /* including head and tail sentinel nodes */
    adding  = 0;
    taking  = 0;
    clear_status (&_wl_status);
    worklist = (work_item*) malloc (qsize * sizeof(work_item));
    if (worklist == NULL) {
        return -2;
    } else return 0;
}

/* reset worklist states, MT-unsafe */
static void worklist_reset(void) {
    head    = 0;
    tail    = 1;
    adding  = 0;
    taking  = 0;
    clear_status (&_wl_status);
    memset (worklist, 0, sizeof(work_item) * qsize);
}

/* destroy built-in worklist and associated sync variables, MT-unsafe
 * It is not safe to call this function if some threads are still blocked by
 * some mutexes and conds defined in this file.
 * No arg, return value.
 */
static void worklist_destroy(void) {
    if (pthread_mutex_destroy (&mutex_head)     ||
        pthread_mutex_destroy (&mutex_tail)     ||
        pthread_cond_destroy (&cond_nonempty)   ||
        pthread_cond_destroy (&cond_nonfull)
       )
    {
        perror ("Destroy worklist synchronization variables");
    }
    free (worklist);
    worklist = NULL;
}

/* Stop current round of tasks */
static void worklist_stop(void) {
    pthread_mutex_lock (&mutex_head);
    pthread_mutex_lock (&mutex_tail);
    set_stop (&_wl_status);
    pthread_mutex_unlock (&mutex_head);
    pthread_mutex_unlock (&mutex_tail);
    pthread_cond_broadcast (&cond_nonfull);
    pthread_cond_broadcast (&cond_nonempty);
}

/* Blocking add work */
static int worklist_add(work_item item) {
    int registered = 0;
    /* If worklist is full, wait on cond_nonfull */
    pthread_mutex_lock (&mutex_tail);
    while ((tail + 1) % qsize == head) {
        if (_wl_status.stop) {
            pthread_mutex_unlock (&mutex_tail);
            return -1;
        }
        if (!registered) {
            registered = 1;
            _wl_status.adding++;
            if (_wl_status.adding == thread_num)
                full_event (NULL);
        }
        pthread_cond_wait (&cond_nonfull, &mutex_tail);
    }
    _wl_status.adding--;
    /* not full now, append item and signal cond_nonempty */
    worklist[tail] = item;
    tail = (tail + 1) % qsize;
    pthread_mutex_unlock (&mutex_tail);
    pthread_cond_signal (&cond_nonempty);
    return 0;
}

/* Blocking take work */
static work_item worklist_take (void) {
    int registered = 0;
    work_item item;
    /* If worklist is empty, wait on cond_nonempty */
    pthread_mutex_lock (&mutex_head);
    while ((head + 1) % qsize == tail) {
        if (!registered) {
            registered = 1;
            _wl_status.taking++;
            if (_wl_status.taking == thread_num) {
                pthread_mutex_unlock (&mutex_head);
                empty_event (NULL);
                pthread_mutex_lock (&mutex_head);
            }
        }
        if (_wl_status.stop) {
            pthread_mutex_unlock (&mutex_head);
            return _wl_empty_item;
        }
        pthread_cond_wait (&cond_nonempty, &mutex_head);
    }
    _wl_status.taking--;
    /* not empty now, poll item and signal cond_nonfull (if block any) */
    head = (head + 1) % qsize;
    item = worklist[head];
    pthread_mutex_unlock (&mutex_head);
    pthread_cond_signal (&cond_nonfull);
    return item;
}

static int _hthp_WL_SIZE = 4096;
static pthread_t *pool;
static pthread_mutex_t      mutex_stop_continue;
static pthread_cond_t       cond_all_stopped, cond_allow_go;
static pthread_barrier_t    barrier_continue;
static int _hthp_stopped_threads = 0;
static int _hthp_blocked_threads = 0;
static int _hthp_stop   = 0,
           _hthp_close  = 0;

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
    /* request task from task queue and execute */
    for(;;) {
        if (_hthp_stop) {
            /* After the thread detects `stop` flag, it will stuck at
             * `cond_allow_go` until issued a `continue` cond
             */
            pthread_mutex_lock (&mutex_stop_continue);
            _hthp_stopped_threads++;
            /* The following wakes up the main thread in `hthpool_wait`,
             * but main thread won't be immediately active:
             * it still acquires `mutex_stop_continue` which is locked now
             */
            if (_hthp_stopped_threads == thread_num) {
                pthread_cond_broadcast (&cond_all_stopped);
            }
            /* Unlock `mutex_stop_continue` and wait for `hthpool_continue` or
             * `hthpool_destroy`.
             */
            while (_hthp_blocked_threads == 0)
                pthread_cond_wait (&cond_allow_go, &mutex_stop_continue);
            /* thread count already reset/set by main thread */
            _hthp_blocked_threads--;
            DBG_PRINT (("%d threads remain blocked\n", _hthp_blocked_threads));
            pthread_mutex_unlock (&mutex_stop_continue);
            if (_hthp_close) {
                DBG_PRINT (("  Thread 0x%lx will be dead.\n",
                            _HTHPOOL_TID (tid)));
                break;
            }
            DBG_PRINT (("  Thread 0x%lx keeps alive.\n", _HTHPOOL_TID (tid)));
            pthread_barrier_wait (&barrier_continue);
        }
        work_item item = worklist_take();
        item.run(item.arg);
    }
    return NULL;
}

/* --------------------------------------------------------------------
 * API which should only be called by the main thread (not in the pool)
 * --------------------------------------------------------------------
 */
/* Must be called before hthpool_init,
 * or, after hthpool_xxxx_stop, hthpool_wait & before hthpool_continue
 */
void hthpool_register(task etask, task ftask) {
    empty_event = (etask == NULL)? (task) _wl_dry_run: etask;
    full_event  = (ftask == NULL)? (task) _wl_dry_run: ftask;
}

/* Initialize a new threadpool
 * return:  int
 *  0       success
 *  -1      #threads or #worklist_size illegal
 *  -2      Error allocating worklist or threadpool
 */
int hthpool_init(int num) {
    int wlret = 0, pret = 0, mret = 0;
    int i;
    if (num < 0)
        return -1;

    thread_num = num;
    _hthp_stopped_threads = 0;
    _hthp_blocked_threads = 0;
    _hthp_close = 0;
    if (pthread_mutex_init (&mutex_stop_continue, NULL)  ||
        pthread_cond_init (&cond_all_stopped, NULL)          ||
        pthread_cond_init (&cond_allow_go, NULL)      ||
        pthread_barrier_init (&barrier_continue, NULL, thread_num)
       )
    {
        perror ("Initialize synchronization variables");
        exit (-1);
    }

    wlret = worklist_init (_hthp_WL_SIZE);
    mret = ( NULL ==
             (pool = (pthread_t*) malloc (sizeof(pthread_t) * thread_num))
           );
    for (i = 0; i < thread_num; i++) {
        pret = pthread_create(pool + i, NULL,
                              daemon_run, NULL);
        if (pret) {
            perror ("Create threads");
            exit (-2);
        }
    }

    if (wlret || mret) {
        worklist_destroy ();
        free (pool);
        return -2;
    }
    return 0;
}

/* Deallocate the worklist, sync vars and join threads
 * return: void
 * exit code:
 *  -1      cannot destroy the synchronization vars
 *  -2      cannot join threads
 */
void hthpool_destroy(void) {
    int i, pret;
    void* ret;
    
    pthread_mutex_lock (&mutex_stop_continue);
    _hthp_close = 1;
    _hthp_blocked_threads = thread_num;
    DBG_PRINT (("Kill'em all!\n"));
    pthread_mutex_unlock (&mutex_stop_continue);
    pthread_cond_broadcast (&cond_allow_go);

    for (i = 0; i < thread_num; i++) {
        pret = pthread_join (pool[i], &ret);
        if (pret) {
            perror ("Join threads");
            exit (-2);
        }
    }
    if (pthread_mutex_destroy (&mutex_stop_continue)   ||
        pthread_cond_destroy (&cond_all_stopped)           ||
        pthread_cond_destroy (&cond_allow_go)       ||
        pthread_barrier_destroy (&barrier_continue)
       )
    {
        perror ("Destroy synchronization variables");
        exit(-1);
    }
    worklist_destroy ();
}

/* Wait until all threads are stopped */
void hthpool_wait(void) {
    pthread_mutex_lock (&mutex_stop_continue);
    /* If all threads in the threadpool already stopped, no need to wait */
    while (_hthp_stopped_threads != thread_num)
        pthread_cond_wait (&cond_all_stopped, &mutex_stop_continue);
    DBG_PRINT (("All threads stopped\n"));
    pthread_mutex_unlock (&mutex_stop_continue);
}

/* Make threadpool running again only after it's been stopped */
void hthpool_continue(void) {
    pthread_mutex_lock (&mutex_stop_continue);
    _hthp_stop = 0;
    _hthp_stopped_threads = 0;
    _hthp_blocked_threads = thread_num;
    worklist_reset ();
    DBG_PRINT (("Threads, continue working!\n"));
    pthread_mutex_unlock (&mutex_stop_continue);
    pthread_cond_broadcast (&cond_allow_go);
}

/* ------------------------------------------------------------------------
 * API which can be called by either the main thread or threads in the pool
 * ------------------------------------------------------------------------
 */
int hthpool_submit(work_item item) {
    return worklist_add(item);
}

void hthpool_hard_stop(void) {
    DBG_PRINT (("Threads, immediately stop working!\n"));
    _hthp_stop = 1;
    worklist_stop ();
}

void hthpool_soft_stop(void) {
    DBG_PRINT (("Threads, please stop working.\n"));
    _hthp_stop = 1;
}

