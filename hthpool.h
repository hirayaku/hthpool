#ifndef HTHPOOL_H_
#define HTHPOOL_H_
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Register events to execute when the threadpool is totally empty or full.
     * It must be called before hthpool_init, or, after hthpool_wait &
     * before hthpool_continue.
     * Here `totally empty` means the worklist queue is empty and all threads
     * keep taking work items from the queue. The empty_task would be
     * executed by the last thread trying to take the task (before it stucks).
     * Similarly, `totally full` means the queue is full and all threads keep
     * adding new work items into the queue. The `full_task` will be executed
     * by the last thread trying to add the task (before it stucks).
     */
    extern void hthpool_register(task empty_task, task full_task);

    /* Intialize the threadpool with `size` worker threads
     * return:  int
     *  0       success
     *  -1      #threads or #worklist_size illegal
     *  -2      Error allocating worklist or threadpool
     */
    extern int  hthpool_init(int size);

    /* Join threads, deallocate the worklist & destroy sync vars
     * It must be called after `hthpool_wait`
     * return: void
     * exit code:
     *  -1      cannot destroy the synchronization vars
     *  -2      cannot join threads
     */
    extern void hthpool_destroy(void);

    /* It can be called by either the main thread or worker thread
     * Submit new work items into the queue.
     */
    extern int  hthpool_submit(work_item);

    /* It can be called by either the main thread or worker thread
     * Stop worker threads (but not join them);
     *  - Worker threads which are executing tasks may be interrupted and
     *  the work may get corrupted.
     *  - If worklist is totally empty or full so that worker threads all
     *  get stuck, hard_stop will wake them up and force them to return.
     */
    extern void hthpool_hard_stop(void);

    /* It can be called by either the main thread or worker thread
     * Stop worker threads (but not join them);
     *  - Worker threads will finish the current task and then stop.
     *  - However, if worklist is totally empty or full, worker threads
     *  will remain stuck even if soft_stop is called.
     */
    extern void hthpool_soft_stop(void);

    /* Main thread waits until all threads are stopped
     * (either caused by hard_stop or soft_stop)
     */
    extern void hthpool_wait(void);

    /* Main thread makes the worker threads continue working
     * after they are stopped.
     * It must be called after `hthpool_wait`.
     */
    extern void hthpool_continue(void);

#ifdef __cplusplus
}
#endif
#endif
