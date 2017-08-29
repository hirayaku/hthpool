#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/types.h>
#include "worklist.h"

int _hthp_thread_num;
int _hthp_terminated_threads = 0;
static int _hthp_WL_SIZE = 4096;
static pthread_t *pool;

/* This is the wrapper function for threads to acquire new item 
 * from the work list, execute the task and then wait for new ones.
 * This function is passed into pthread_create during thread pool initialization
 * Always return NULL
 */
static void* daemon_run(void* arg) {
    /* request task from task queue and execute */
    for(;;) {
        work_item item = worklist_poll();
        item.run(item.arg);
    }
    return NULL;
}

/* Initialize a new threadpool
 * return:  int
 *  0       success
 *  -1      #threads or #worklist_size illegal
 *  -2      cannot allocate worklist queue
 *  -3      cannot allocate threadpool
 */
int hthpool_init(int num) {
    int wlret = 0;
    int i;
    if (num < 0)
        return -1;

    _hthp_thread_num = num;
    _hthp_terminated_threads = 0;
    wlret = worklist_init (_hthp_WL_SIZE);
    if (wlret)
        return wlret;

    pool = (pthread_t*) malloc (sizeof(pthread_t) * thread_num);
    if (pool = NULL) {
        return -3;
    }
    for (i = 0; i < thread_num; i++) {
    }
}

void hthpool_terminate(void);

void hthpool_submit(work_item item);

