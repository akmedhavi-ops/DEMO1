/*
 * rdn_osal.h - operating system abstraction layer
 *
 * Implemented by osal/rdn_osal_vxworks.c (VxWorks 6.9 / 7 kernel mode) and
 * osal/rdn_osal_posix.c (Linux / host verification).
 */
#ifndef RDN_OSAL_H
#define RDN_OSAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDN_WAIT_FOREVER 0xFFFFFFFFu

typedef struct rdn_mutex rdn_mutex_t;
typedef struct rdn_sem   rdn_sem_t;
typedef struct rdn_task  rdn_task_t;
typedef void (*rdn_task_fn)(void *arg);

enum { RDN_LOG_ERR = 0, RDN_LOG_WARN = 1, RDN_LOG_INFO = 2, RDN_LOG_DEBUG = 3 };

void        *rdn_osal_alloc(size_t n);            /* zero-filled            */
void         rdn_osal_free(void *p);

rdn_mutex_t *rdn_mutex_create(void);              /* priority inheritance   */
void         rdn_mutex_destroy(rdn_mutex_t *m);
void         rdn_mutex_lock(rdn_mutex_t *m);
void         rdn_mutex_unlock(rdn_mutex_t *m);

rdn_sem_t   *rdn_sem_create(void);                /* binary, initially empty*/
void         rdn_sem_destroy(rdn_sem_t *s);
void         rdn_sem_give(rdn_sem_t *s);
int          rdn_sem_take(rdn_sem_t *s, uint32_t timeout_us); /* 0 / RDN_E_TIMEOUT */

rdn_task_t  *rdn_task_spawn(const char *name, int prio, uint32_t stack,
                            rdn_task_fn fn, void *arg);
void         rdn_task_join(rdn_task_t *t);        /* waits for fn to return */

uint64_t     rdn_time_us(void);                   /* monotonic              */
uint32_t     rdn_time_resolution_us(void);
void         rdn_sleep_us(uint32_t us);
uint32_t     rdn_osal_entropy(void);

extern int   rdn_log_level;
void         rdn_log(int level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Full memory barrier. On PowerPC "sync" orders stores to cache-inhibited
 * (VME master window) space as well as cacheable memory. */
#if defined(__PPC__) || defined(__powerpc__) || defined(_ARCH_PPC)
#define RDN_MEM_BARRIER() __asm__ __volatile__("sync" ::: "memory")
#elif defined(__GNUC__)
#define RDN_MEM_BARRIER() __sync_synchronize()
#else
#error "define RDN_MEM_BARRIER for this compiler"
#endif

/* Word-sized flags shared between tasks without a lock. The GCC __sync
 * builtins exist in every GNU toolchain shipped with VxWorks 6.9 (gcc 4.3)
 * and VxWorks 7, and are understood by ThreadSanitizer on the host. */
#define RDN_ATOMIC_LOAD(p)      __sync_fetch_and_add((p), 0u)
#define RDN_ATOMIC_STORE(p, v)  do { (void)__sync_lock_test_and_set((p), (v)); \
                                     __sync_synchronize(); } while (0)

#ifdef __cplusplus
}
#endif
#endif /* RDN_OSAL_H */
