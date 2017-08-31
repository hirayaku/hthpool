#ifndef HTHPOOL_H_
#define HTHPOOL_H_
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

    extern pthread_cond_t cond_ext_empty, cond_ext_full;
    extern int  hthpool_init(int);
    extern void hthpool_destroy(void);
    extern void hthpool_wait(void);
    extern int  hthpool_submit(work_item);
    extern void hthpool_stop(void);
    extern void hthpool_continue(void);

#ifdef __cplusplus
}
#endif
#endif
