/*
 * rdn_osal_posix.c - OSAL for POSIX hosts (Linux), used for verification.
 *
 * Priorities are ignored unless RDN_POSIX_RT is defined and the process is
 * permitted to use SCHED_FIFO; the VxWorks priority p maps to 99 - p/3.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "rdn.h"
#include "rdn_osal.h"

int rdn_log_level = RDN_LOG_WARN;

struct rdn_mutex { pthread_mutex_t m; };
struct rdn_sem   { pthread_mutex_t m; pthread_cond_t c; int count; };
struct rdn_task  { pthread_t th; rdn_task_fn fn; void *arg; };

void *rdn_osal_alloc(size_t n) { return calloc(1u, n); }
void  rdn_osal_free(void *p)   { free(p); }

rdn_mutex_t *rdn_mutex_create(void)
{
    rdn_mutex_t *m = (rdn_mutex_t *)calloc(1u, sizeof(*m));
    pthread_mutexattr_t a;

    if (m == NULL) {
        return NULL;
    }
    (void)pthread_mutexattr_init(&a);
#if defined(_POSIX_THREAD_PRIO_INHERIT) && (_POSIX_THREAD_PRIO_INHERIT > 0)
    (void)pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
#endif
    if (pthread_mutex_init(&m->m, &a) != 0) {
        free(m);
        m = NULL;
    }
    (void)pthread_mutexattr_destroy(&a);
    return m;
}

void rdn_mutex_destroy(rdn_mutex_t *m) { (void)pthread_mutex_destroy(&m->m); free(m); }
void rdn_mutex_lock(rdn_mutex_t *m)    { (void)pthread_mutex_lock(&m->m); }
void rdn_mutex_unlock(rdn_mutex_t *m)  { (void)pthread_mutex_unlock(&m->m); }

rdn_sem_t *rdn_sem_create(void)
{
    rdn_sem_t *s = (rdn_sem_t *)calloc(1u, sizeof(*s));
    pthread_condattr_t ca;

    if (s == NULL) {
        return NULL;
    }
    (void)pthread_condattr_init(&ca);
    (void)pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    (void)pthread_mutex_init(&s->m, NULL);
    (void)pthread_cond_init(&s->c, &ca);
    (void)pthread_condattr_destroy(&ca);
    return s;
}

void rdn_sem_destroy(rdn_sem_t *s)
{
    (void)pthread_cond_destroy(&s->c);
    (void)pthread_mutex_destroy(&s->m);
    free(s);
}

void rdn_sem_give(rdn_sem_t *s)
{
    (void)pthread_mutex_lock(&s->m);
    s->count = 1;                        /* binary semaphore */
    (void)pthread_cond_signal(&s->c);
    (void)pthread_mutex_unlock(&s->m);
}

int rdn_sem_take(rdn_sem_t *s, uint32_t timeout_us)
{
    struct timespec ts;
    int rc = 0;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += (time_t)(timeout_us / 1000000u);
    ts.tv_nsec += (long)(timeout_us % 1000000u) * 1000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    (void)pthread_mutex_lock(&s->m);
    while ((s->count == 0) && (rc == 0)) {
        if (timeout_us == RDN_WAIT_FOREVER) {
            rc = pthread_cond_wait(&s->c, &s->m);
        } else {
            rc = pthread_cond_timedwait(&s->c, &s->m, &ts);
        }
    }
    if (s->count != 0) {
        s->count = 0;
        rc = 0;
    }
    (void)pthread_mutex_unlock(&s->m);
    return (rc == 0) ? RDN_OK : RDN_E_TIMEOUT;
}

static void *task_entry(void *arg)
{
    rdn_task_t *t = (rdn_task_t *)arg;
    t->fn(t->arg);
    return NULL;
}

rdn_task_t *rdn_task_spawn(const char *name, int prio, uint32_t stack,
                           rdn_task_fn fn, void *arg)
{
    rdn_task_t *t = (rdn_task_t *)calloc(1u, sizeof(*t));
    pthread_attr_t a;

    (void)name;
    if (t == NULL) {
        return NULL;
    }
    t->fn  = fn;
    t->arg = arg;
    (void)pthread_attr_init(&a);
    if (stack < 65536u) {
        stack = 65536u;
    }
    (void)pthread_attr_setstacksize(&a, stack);
#ifdef RDN_POSIX_RT
    {
        struct sched_param sp;
        sp.sched_priority = 99 - (prio / 3);
        (void)pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
        (void)pthread_attr_setschedpolicy(&a, SCHED_FIFO);
        (void)pthread_attr_setschedparam(&a, &sp);
    }
#else
    (void)prio;
#endif
    if (pthread_create(&t->th, &a, task_entry, t) != 0) {
        (void)pthread_attr_destroy(&a);
        free(t);
        return NULL;
    }
    (void)pthread_attr_destroy(&a);
    return t;
}

void rdn_task_join(rdn_task_t *t)
{
    (void)pthread_join(t->th, NULL);
    free(t);
}

uint64_t rdn_time_us(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000L);
}

uint32_t rdn_time_resolution_us(void)
{
    return 1u;
}

void rdn_sleep_us(uint32_t us)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    while ((nanosleep(&ts, &ts) != 0) && (errno == EINTR)) {
        /* resume */
    }
}

uint32_t rdn_osal_entropy(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    return (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 12) ^
           ((uint32_t)getpid() << 20);
}

void rdn_log(int level, const char *fmt, ...)
{
    va_list ap;
    if (level > rdn_log_level) {
        return;
    }
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
}
