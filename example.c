#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include "hthpool.h"

int main(void) {
    int rounds = 4;
    pthread_mutex_t mutex_null = PTHREAD_MUTEX_INITIALIZER;
    hthpool_init (4);
    pthread_mutex_lock (&mutex_null);
    while (rounds >= 0) {
        rounds--;
        pthread_cond_wait (&cond_ext_empty, &mutex_null);
        puts ("Thread pool is always empty");
        pthread_mutex_unlock (&mutex_null);
        sleep (1);
    }
    hthpool_stop ();
    hthpool_wait ();
    hthpool_destroy ();
    return 0;
}
