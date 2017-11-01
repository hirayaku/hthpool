#ifndef WORKLIST_H_ 
#define WORKLIST_H_
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct worklist_attr {
    int     trigger;
    size_t  concurrency;
    work_item empty_event, full_event;
} worklist_attr;

typedef struct worklist {
    work_item* queue;
    size_t head, tail;
    size_t qsize;
    status_t status;
    pthread_mutex_t  mutex_head, mutex_tail;
    pthread_cond_t   cond_nonempty, cond_nonfull;
    worklist_attr* attr;
} worklist_t;

/* empty task which literally does nothing */
static void* _wl_dry_run(void* arg) {
    return NULL;
}
const work_item WL_EMPTYITEM = { (task) _wl_dry_run, NULL };

/* init a worklist_attr data structure, default:
 * trigger = 0; concurrency = 0; empty_event = full_event = WL_EMPTYITEM
 */
extern void worklistattr_init (worklist_attr *attr);

/* set maximum number of concurrent access threads for the worklist */
extern void worklistattr_setconcurrency (worklist_attr *attr,
                                         size_t concurrency);

/* set triggered event when the worklist is totally empty or full */
extern void worklistattr_setevent (worklist_attr *attr,
                                   work_item empty_event,
                                   work_item full_event);

/* init a new worklist with specified size and attribute */
extern int  worklist_init (worklist_t* wl, size_t size,
                           worklist_attr *attr);

/* reset worklist data */
extern void worklist_reset (worklist_t* wl);

/* destroy worklist, release all resources */
extern void worklist_destroy (worklist_t* wl);

/* stop all ongoing & future tasks (add/take) on the worklist */
extern void worklist_stop (worklist_t* wl);

/* blocking add/take if the worklist is totally full/empty */
extern int worklist_add(worklist_t* wl, work_item item);
extern work_item worklist_take (worklist_t* wl);

#ifdef __cplusplus
}
#endif
