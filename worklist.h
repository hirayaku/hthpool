#ifndef WORKLIST_H_
#define WORKLIST_H_

#ifdef __cplusplus
extern "C" {
#endif
/* NOTE: In both ANSI-C and C99, it's undefined behavior to include a function type in an aggregate type. 
 * GNU C extensions seem to support this. But then `struct work_item` is not portable.
 * TODO: detach function type and arguments.
 */
typedef void* (*task)(void*);
typedef struct work_item {
    task run;
    void* arg;
} work_item;

int     worklist_init(size_t size);
void    worklist_destroy(void);
void    worklist_terminate(void);
void    worklist_append(work_item item);
work_item worklist_poll(void);
int     worklist_busy(void);

#ifdef __cplusplus
}
#endif

#endif
