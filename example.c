#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include "hthpool.h"

pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t count_stop = PTHREAD_COND_INITIALIZER;
int count = 0;
void* print_empty(void* arg) {
    printf ("Worklist is empty!\n");
    hthpool_hard_stop ();
    return NULL;
}

int main(void) {
    hthpool_register ((task)print_empty, NULL);
    hthpool_init (4);
    hthpool_wait ();
    hthpool_destroy ();
    return 0;
}
