#include "airport.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void checked(int error, const char *operation) {
    if (error) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        abort();
    }
}
const char *sync_backend(void) {
#ifdef USE_CONDVAR
    return "mutex+condition_variable";
#else
    return "mutex+semaphore";
#endif
}
int notification_init(Notification *n) {
    int e = pthread_mutex_init(&n->mutex, NULL);
    if (e) return e;
    n->pending = 0;
#ifdef USE_CONDVAR
    e = pthread_cond_init(&n->condition, NULL);
#else
    e = sem_init(&n->semaphore, 0, 0) == 0 ? 0 : errno;
#endif
    if (e) pthread_mutex_destroy(&n->mutex);
    return e;
}
void notification_destroy(Notification *n) {
#ifdef USE_CONDVAR
    checked(pthread_cond_destroy(&n->condition), "pthread_cond_destroy");
#else
    if (sem_destroy(&n->semaphore)) checked(errno, "sem_destroy");
#endif
    checked(pthread_mutex_destroy(&n->mutex), "pthread_mutex_destroy");
}

void notification_send(Notification *n) {
    checked(pthread_mutex_lock(&n->mutex), "notification lock");
    if (!n->pending) {
        n->pending = 1;
#ifdef USE_CONDVAR
        checked(pthread_cond_signal(&n->condition), "pthread_cond_signal");
#else
        if (sem_post(&n->semaphore)) checked(errno, "sem_post");
#endif
    }
    checked(pthread_mutex_unlock(&n->mutex), "notification unlock");
}
void notification_wait(Notification *n) {
#ifdef USE_CONDVAR
    checked(pthread_mutex_lock(&n->mutex), "notification lock");
    while (!n->pending)
        checked(pthread_cond_wait(&n->condition, &n->mutex), "pthread_cond_wait");
#else
    int e;
    do { e = sem_wait(&n->semaphore); } while (e && errno == EINTR);
    if (e) checked(errno, "sem_wait");
    checked(pthread_mutex_lock(&n->mutex), "notification lock");
#endif
    n->pending = 0;
    checked(pthread_mutex_unlock(&n->mutex), "notification unlock");
}
