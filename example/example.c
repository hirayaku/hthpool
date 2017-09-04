#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include "../hthpool.h"

pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t count_stop = PTHREAD_COND_INITIALIZER;
int count = 0;
void* print_empty(void* arg) {
    printf ("Worklist is empty!\n");
    hthpool_hard_stop (*(hthpool*)arg);
    return NULL;
}
void* print_info(void* arg) {
    printf ("Worklist not empty now!\n");
    return NULL;
}

int main(void) {
    hthpool pool;
    work_item empty_event = { (task) print_empty, &pool };
    pool = hthpool_init (4, empty_event, _wl_empty_item);
    hthpool_wait (pool);
    hthpool_destroy (pool);
    return 0;
}
