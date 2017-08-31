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
#include <pthread.h>
#include <sys/types.h>
#include "worklist.h"
#define HTHPOOL_DEBUG

int _hthp_thread_num;
static int _hthp_WL_SIZE = 4096;
static pthread_t *pool;
static pthread_mutex_t mutex_term_count;
static pthread_cond_t  cond_term, cond_continue;
static pthread_barrier_t barrier_continue;
static int _hthp_stopped_threads = 0;
static int _hthp_to_destroy = 0;
static int _hthp_exited_threads = 0;

/* This is the wrapper function for threads to acquire new item 
 * from the work list, execute the task and then wait for new ones.
 * This function is passed into pthread_create during thread pool initialization
 * Always return NULL
 */
static void* daemon_run(void* arg) {
    for(;;) {
        /* request task from task queue and execute */
        for(;;) {
            if (worklist_status() != 0) {
                pthread_mutex_lock (&mutex_term_count);
                _hthp_stopped_threads++;
                pthread_mutex_unlock (&mutex_term_count);
                if (_hthp_stopped_threads == _hthp_thread_num)
                    pthread_cond_broadcast (&cond_term);
                break;
            }
            work_item item = worklist_poll();
            item.run(item.arg);
        }
        /* After the thread detects `stop` flag,
         * it will stuck here until issued a `continue` cond
         */
        pthread_mutex_lock (&mutex_term_count);
        pthread_cond_wait (&cond_continue, &mutex_term_count);
        _hthp_stopped_threads--;
#ifdef HTHPOOL_DEBUG
        printf ("%d threads remain blocked\n", _hthp_stopped_threads);
#endif
        pthread_mutex_unlock (&mutex_term_count);
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

    _hthp_thread_num = num;
    _hthp_stopped_threads = 0;
    if (pthread_mutex_init (&mutex_term_count, NULL)  ||
        pthread_cond_init (&cond_term, NULL)          ||
        pthread_cond_init (&cond_continue, NULL)      ||
        pthread_barrier_init (&barrier_continue, NULL, _hthp_thread_num)
       )
    {
        perror ("Initialize synchronization variables");
        exit (-1);
    }

    wlret = worklist_init (_hthp_WL_SIZE);
    mret = ( NULL ==
             (pool = (pthread_t*) malloc (sizeof(pthread_t) * _hthp_thread_num))
           );
    for (i = 0; i < _hthp_thread_num; i++) {
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
    worklist_destroy ();

    if (pthread_mutex_destroy (&mutex_term_count)   ||
        pthread_cond_destroy (&cond_term)           ||
        pthread_cond_destroy (&cond_continue)       ||
        pthread_barrier_destroy (&barrier_continue)
       )
    {
        perror ("Destroy synchronization variables");
        exit(-1);
    }
}

/* Wait until all threads are stopped */
void hthpool_wait(void) {
    pthread_mutex_lock (&mutex_term_count);
    pthread_cond_wait (&cond_continue, &mutex_term_count);
#ifdef HTHPOOL_DEBUG
    puts ("All threads stopped");
#endif
    pthread_mutex_unlock (&mutex_term_count);
}

/* ------------------------------------------------------------------------
 * API which can be called by either the main thread or threads in the pool
 * ------------------------------------------------------------------------
 */
void hthpool_submit(work_item item) {
    worklist_append(item);
}

void hthpool_stop(void) {
    worklist_stop();
}
