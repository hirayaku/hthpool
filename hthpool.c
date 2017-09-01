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
#include <stdarg.h>
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


static int thread_num;
static int _hthp_WL_SIZE = 4096;
static pthread_t *pool;
static pthread_mutex_t      mutex_term_count, mutex_destroy;
static pthread_cond_t       cond_term, cond_continue;
static pthread_barrier_t    barrier_continue;
static int _hthp_stopped_threads = 0;
static int _hthp_blocked_threads = 0;
static int _hthp_close = 0;

static int init_locked_mutex(pthread_mutex_t* restrict mutex,
                             pthread_mutexattr_t *restrict attr)
{
    int pret = pthread_mutex_init (mutex, attr);
    if (pret)
        return pret;
    pret = pthread_mutex_lock (mutex);
    return pret;
}
static int destroy_locked_mutex(pthread_mutex_t* restrict mutex) {
    return pthread_mutex_destroy (mutex);
}
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
    for(;;) {
        /* request task from task queue and execute */
        for(;;) {
            if (worklist_status().stop != 0)
                break;
            work_item item = worklist_poll();
            item.run(item.arg);
        }
        /* After the thread detects `stop` flag, it break out of the loop and
         * will stuck at `cond_continue` until issued a `continue` cond
         */
        pthread_mutex_lock (&mutex_term_count);
        _hthp_stopped_threads++;
        /* The following wakes up the main thread in `hthpool_wait`,
         * but main thread won't be immediately active:
         * it still acquires `mutex_term_count` which is locked now
         */
        if (_hthp_stopped_threads == thread_num)
            pthread_cond_broadcast (&cond_term);
        /* Unlock `mutex_term_count` and wait for `hthpool_continue` or
         * `hthpool_destroy`.
         */
        pthread_cond_wait (&cond_continue, &mutex_term_count);
        /* thread count already reset/set by main thread */
        _hthp_blocked_threads--;
        DBG_PRINT (("%d threads remain blocked\n", _hthp_blocked_threads));
        pthread_mutex_unlock (&mutex_term_count);
        if (_hthp_close) {
            if (_hthp_blocked_threads == 0) {
                pthread_mutex_unlock (&mutex_destroy);
            }
            DBG_PRINT (("  Thread 0x%lx will be dead.\n", _HTHPOOL_TID (tid)));
            break;
        }
        DBG_PRINT(("  Thread 0x%lx keeps alive.\n", _HTHPOOL_TID (tid)));
        /* Don't enter into inner loop until all threads are ready.
         * If no barrier here, chances are some threads enter the loop,
         * stop the threadpool and exit the loop while others haven't
         * entered once. This will mess up the threadpool execution.
         */
        pthread_barrier_wait (&barrier_continue);
    }
    return NULL;
}

/* --------------------------------------------------------------------
 * API which should only be called by the main thread (not in the pool)
 * --------------------------------------------------------------------
 */
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
    _hthp_close = 0;
    if (pthread_mutex_init (&mutex_term_count, NULL)  ||
        init_locked_mutex (&mutex_destroy, NULL)      ||
        pthread_cond_init (&cond_term, NULL)          ||
        pthread_cond_init (&cond_continue, NULL)      ||
        pthread_barrier_init (&barrier_continue, NULL, thread_num)
       )
    {
        perror ("Initialize synchronization variables");
        exit (-1);
    }

    wlret = worklist_init (_hthp_WL_SIZE, thread_num);
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
    worklist_destroy ();
    
    DBG_PRINT (("Kill'em all!\n"));
    _hthp_close = 1;
    _hthp_blocked_threads = thread_num;
    pthread_cond_broadcast (&cond_continue);
    pthread_mutex_lock (&mutex_destroy);
    if (pthread_mutex_destroy (&mutex_term_count)   ||
        destroy_locked_mutex (&mutex_destroy)       ||
        pthread_cond_destroy (&cond_term)           ||
        pthread_cond_destroy (&cond_continue)       ||
        pthread_barrier_destroy (&barrier_continue)
       )
    {
        perror ("Destroy synchronization variables");
        exit(-1);
    }
    for (i = 0; i < thread_num; i++) {
        pret = pthread_join (pool[i], &ret);
        if (pret) {
            perror ("Join threads");
            exit (-2);
        }
    }
}

/* Wait until all threads are stopped */
void hthpool_wait(void) {
    pthread_mutex_lock (&mutex_term_count);
    /* If all threads in the threadpool already stopped, no need to wait */
    if (_hthp_stopped_threads == thread_num) {
        DBG_PRINT (("All threads stopped\n"));
        pthread_mutex_unlock (&mutex_term_count);
        return;
    }
    pthread_cond_wait (&cond_term, &mutex_term_count);
    DBG_PRINT (("All threads stopped\n"));
    pthread_mutex_unlock (&mutex_term_count);
}

/* Make threadpool running again only after it's been stopped */
void hthpool_continue(void) {
    worklist_reset ();
    _hthp_stopped_threads = 0;
    _hthp_blocked_threads = thread_num;
    DBG_PRINT (("Threads, continue working!\n"));
    pthread_cond_broadcast (&cond_continue);
}

/* ------------------------------------------------------------------------
 * API which can be called by either the main thread or threads in the pool
 * ------------------------------------------------------------------------
 */
int hthpool_submit(work_item item) {
    return worklist_append(item);
}

void hthpool_stop(void) {
    DBG_PRINT (("Threads, stop working!\n"));
    work_item item = { (task) worklist_stop, NULL };
    hthpool_submit (item);
    /* worklist_stop(); */
}
