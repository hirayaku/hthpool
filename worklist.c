#include "worklist.h"

/* -----------------------------------------------------------------------
 * API for worklist and worklistattr.
 * For a summary of declarations, see `worklist.h`
 * -----------------------------------------------------------------------
 */
void  worklistattr_init (worklist_attr* attr) {
    attr->trigger = 0;
    attr->concurrency = 0;
    attr->full_event  = WL_EMPTYITEM;
    attr->empty_event = WL_EMPTYITEM;
}

void worklistattr_setconcurrency (worklist_attr *attr,
                                  size_t concurrency)
{
    attr->concurrency = concurrency;
    attr->trigger = 1;
}

void worklistattr_setevent (worklist_attr *attr,
                            work_item empty_event,
                            work_item full_event)
{
    attr->empty_event = empty_event;
    attr->full_event = full_event;
    attr->trigger = 1;
}

static inline void set_stop(worklist_t *wl) {
    wl->status.stop = 1;
}
static inline void clear_status(worklist_t *wl) {
    wl->status.stop     = 0;
    wl->status.adding   = 0;
    wl->status.taking   = 0;
}

/* initialize a worklist with given size and worklist_attr, MT-unsafe
 * arg:
 * @wl            the pointer to the worklist to be initialized
 * @size          size of the worklist, excluding sentinel nodes
 * @attr          worklist_attr, used to configure the initialized worklist
 * return:
 * STAT_OK        successful
 * STAT_SYNC      error creating synchronization variables
 * STAT_ALLOC     cannot allocate space for array
 */
int worklist_init(worklist_t *wl, size_t size, 
                  worklist_attr* attr)
{
    if (size == 0)
        size = DEFAULT_SIZE;

    wl->head    = 0;
    wl->tail    = 1;
    wl->qsize   = size + 2;   /* including head and tail sentinel nodes */
    clear_status (wl);
    if (pthread_mutex_init (&wl->mutex_head, NULL)  ||
        pthread_mutex_init (&wl->mutex_tail, NULL)  ||
        pthread_cond_init (&wl->cond_nonempty, NULL)||
        pthread_cond_init (&wl->cond_nonfull, NULL)
       )
    {
        perror ("Create worklist synchronization variables");
        return STAT_SYNC;
    }
    wl->queue = (work_item*) malloc (wl->qsize * sizeof(work_item));
    if (NULL == attr) {
        wl->attr = NULL;
    } else {
        wl->attr = (worklist_attr*) malloc (sizeof(worklist_attr));
    }

    if (wl->queue == NULL || wl->attr == NULL) {
        pthread_mutex_destroy (&wl->mutex_head);
        pthread_mutex_destroy (&wl->mutex_tail);
        pthread_cond_destroy (&wl->cond_nonempty);
        pthread_cond_destroy (&wl->cond_nonfull);
        return STAT_ALLOC;
    } else {
        memcpy(wl->attr, attr, sizeof(worklist_attr));
        if (!wl->attr->trigger)
            wl->attr = NULL;
    }
    return STAT_OK;
}

/* reset worklist states, MT-unsafe */
void worklist_reset(worklist_t* wl) {
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
void worklist_destroy(worklist_t* wl) {
    free (wl->queue);
    wl->queue = NULL;
    free (wl->attr);
    wl->attr = NULL;
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
void worklist_stop(worklist_t* wl) {
    set_stop (wl);
    pthread_cond_broadcast (&wl->cond_nonfull);
    pthread_cond_broadcast (&wl->cond_nonempty);
}

/* Blocking add work */
int worklist_add(worklist_t* wl, work_item item) {
    int registered = 0;
    // Enter the critical section for worklist tail
    pthread_mutex_lock (&wl->mutex_tail);

    // If worklist is totally full, full_event (if defined) is triggered; 
    // else the current thread shall wait on cond_nonfull
    while ((wl->tail + 1) % wl->qsize == wl->head) {
        if (!registered) {
            registered = 1;
            if (!wl->attr) {
                wl->status.adding++;
                if (wl->status.adding >= wl->attr->concurrency)
                {
                    // These unlocks & locks around the `full_event` are necessary to
                    // 1. make sure no threads executing `worklist_take` will stuck at
                    //    `cond_nonfull`
                    // 2. no deadlock (which may be caused by locking mutex_head w/o
                    //    unlocking mutex_tail here)
                    pthread_mutex_unlock (&wl->mutex_tail);
                    pthread_mutex_lock (&wl->mutex_head);
                    wl->attr->full_event.run (wl->attr->full_event.arg);
                    pthread_mutex_unlock (&wl->mutex_head);
                    pthread_mutex_lock (&wl->mutex_tail);
                }
            }
        }
        if (wl->status.stop) {
            pthread_mutex_unlock (&wl->mutex_tail);
            return STAT_TERM;
        }
        pthread_cond_wait (&wl->cond_nonfull, &wl->mutex_tail);
    }
    if (registered && !wl->attr)
        wl->status.adding--;
    /* not full now, append item and signal cond_nonempty */
    wl->queue[wl->tail] = item;
    wl->tail = (wl->tail + 1) % wl->qsize;
    pthread_mutex_unlock (&wl->mutex_tail);
    pthread_cond_signal (&wl->cond_nonempty);
    return STAT_OK;
}

/* Blocking take work */
work_item worklist_take (worklist_t* wl) {
    int registered = 0;
    work_item item;
    // Enter the critical section for worklist head
    pthread_mutex_lock (&wl->mutex_head);

    // If worklist is totally empty, empty_event (if defined) is triggered; 
    // else the current thread shall wait on cond_nonempty
    while ((wl->head + 1) % wl->qsize == wl->tail) {
        if (!registered) {
            registered = 1;
            if (!wl->attr) {
                wl->status.taking++;
                if (wl->status.taking >= wl->attr->concurrency)
                {
                    // These unlocks & locks around the `empty_event` are necessary to
                    // 1. make sure no threads executing `worklist_add` will stuck at
                    //    `cond_nonempty`
                    // 2. no deadlock (which may be caused by locking mutex_tail w/o
                    //    unlocking mutex_tail here)
                    pthread_mutex_unlock (&wl->mutex_head);
                    pthread_mutex_lock (&wl->mutex_tail);
                    wl->attr->empty_event.run (wl->attr->empty_event.arg);
                    pthread_mutex_unlock (&wl->mutex_tail);
                    pthread_mutex_lock (&wl->mutex_head);
                }
            }
        }
        if (wl->status.stop) {
            pthread_mutex_unlock (&wl->mutex_head);
            return WL_EMPTYITEM;
        }
        pthread_cond_wait (&wl->cond_nonempty, &wl->mutex_head);
    }
    if (registered && !wl->attr)
        wl->status.taking--;
    /* not empty now, poll item and signal cond_nonfull (if block any) */
    wl->head = (wl->head + 1) % wl->qsize;
    item = wl->queue[wl->head];
    pthread_mutex_unlock (&wl->mutex_head);
    pthread_cond_signal (&wl->cond_nonfull);
    return item;
}

