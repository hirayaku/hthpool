#ifndef TYPES_H_
#define TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

/* NOTE: In both ANSI-C and C99, it's undefined behavior to include
 * a function type in an aggregate type. 
 * GNU C extensions seem to support this. But `struct work_item` isn't portable.
 * TODO: detach function type and arguments.
 */
typedef void* (*task)(void*);
struct work_item {
    task run;
    void* arg;
};
typedef struct work_item work_item;

struct worklist_status {
    int stop;
    int empty;
    int full; 
};
typedef struct worklist_status status_t;

#ifdef __cplusplus
}
#endif

#endif
