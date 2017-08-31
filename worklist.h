#ifndef WORKLIST_H_
#define WORKLIST_H_
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern work_item _hthp_empty_task;
#define EMPTY_TASK (_hthp_empty_task)

/* cond variables prepared for upper-level applications
 * `cond_ext_empty` will be broadcast when the worklist is totally empty
 * `cond_ext_full` will be broadcast when the worklist is totally full
 */
extern pthread_cond_t cond_ext_empty, cond_ext_full;

/* Allocate space for worklist and initialize states
 * @size            the capacity of the worklist
 *                  capacity = size when size > 0
 *                  capacity = DEFAULT_SIZE when size == 0
 * @concurrency     max #threads that are allowed to access
 *                  the worklist simultaneously (append/poll)
 *                  #threads = concurrency when concurrency > 0
 *                  #threads = INT_MAX when concurrency == 0
 */
extern int      worklist_init(size_t size, size_t concurrency);

extern void     worklist_reset(void);
extern void     worklist_destroy(void);

/* Stop executing items of current worklist.
 * Don't affect ongoing tasks; ongoing tasks means those
 * which have already been polled or are being executed
 */
extern void     worklist_stop(void);

extern status_t worklist_status(void);

/* Enqueue new tasks into the worklist
 *  - If worklist is full and some (not all) threads are enqueueing tasks,
 *    these threads will be blocked until some tasks are dequeued.
 *  - If worklist is full but all threads just keep enqueueing tasks,
 *    the last task to be appended will be thrown and the corresponding
 *    thread will return with nonzero error code.
 *  - If worklist is stopped, the thread will directly return with nonzero
 *    error code.
 */
extern int      worklist_append(work_item item);

/* Dequeue tasks from the worklist
 *  - If worklist is empty and some (not all) threads are dequeueing tasks,
 *    these threads will be blocked until new tasks are enqueued.
 *  - If worklist is full but all threads just keep dequeueing tasks,
 *    the last thread to poll will directly return (with an empty task)
 *  - If worklist is stopped, the thread will directly return with an empty
 *    task.
 */
extern work_item worklist_poll(void);

/* A naive way to check if the worklist is busy,
 * return nonzero value if busy, zero if not
 */
extern int      worklist_busy(void);

#ifdef __cplusplus
}
#endif

#endif
