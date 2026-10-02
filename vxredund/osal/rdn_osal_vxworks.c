/*
 * rdn_osal_vxworks.c - OSAL for VxWorks 6.9 and VxWorks 7 (kernel mode / DKM)
 *
 * Time base
 *   Default: 64-bit system tick (tick64Get), resolution = 1 tick. With the
 *   usual sysClkRateSet(1000) that is 1 ms, and the timing bound
 *   (docs/TIMING.md) carries a 2q term for it.
 *   -DRDN_VX_USE_TIMEBASE: read the PowerPC time base register instead.
 *   Set RDN_VX_TB_HZ to the time base frequency of the board. On MVME5500
 *   (MPC7457, 133 MHz MPX bus) the time base normally runs at bus/4; verify
 *   against your BSP (e.g. the value used by sysTimestampFreq()).
 *
 * Tasks
 *   Created with VX_FP_TASK (callbacks may use floating point). A join is
 *   emulated with a binary semaphore given when the entry function returns.
 */
#include <vxWorks.h>
#include <semLib.h>
#include <taskLib.h>
#include <tickLib.h>
#include <sysLib.h>
#include <errnoLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdn.h"
#include "rdn_osal.h"

#include <vxLib.h>
#if defined(_ARCH_PPC) || defined(__PPC__)
#include <arch/ppc/vxPpcLib.h>    /* vxTimeBaseGet() */
#endif

#if defined(RDN_VX_USE_TIMEBASE)
#if !(defined(_ARCH_PPC) || defined(__PPC__))
#error "RDN_VX_USE_TIMEBASE requires a PowerPC target"
#endif
#ifndef RDN_VX_TB_HZ
#define RDN_VX_TB_HZ 33333333u    /* MVME5500: 133.33 MHz / 4 - VERIFY */
#endif
#endif

int rdn_log_level = RDN_LOG_WARN;

struct rdn_mutex { SEM_ID id; };
struct rdn_sem   { SEM_ID id; };
struct rdn_task  { TASK_ID tid; SEM_ID done; rdn_task_fn fn; void *arg; };

void *rdn_osal_alloc(size_t n) { return calloc(1u, n); }
void  rdn_osal_free(void *p)   { free(p); }

rdn_mutex_t *rdn_mutex_create(void)
{
    rdn_mutex_t *m = (rdn_mutex_t *)calloc(1u, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->id = semMCreate(SEM_Q_PRIORITY | SEM_INVERSION_SAFE | SEM_DELETE_SAFE);
    if (m->id == SEM_ID_NULL) {
        free(m);
        return NULL;
    }
    return m;
}

void rdn_mutex_destroy(rdn_mutex_t *m) { (void)semDelete(m->id); free(m); }
void rdn_mutex_lock(rdn_mutex_t *m)    { (void)semTake(m->id, WAIT_FOREVER); }
void rdn_mutex_unlock(rdn_mutex_t *m)  { (void)semGive(m->id); }

rdn_sem_t *rdn_sem_create(void)
{
    rdn_sem_t *s = (rdn_sem_t *)calloc(1u, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->id = semBCreate(SEM_Q_PRIORITY, SEM_EMPTY);
    if (s->id == SEM_ID_NULL) {
        free(s);
        return NULL;
    }
    return s;
}

void rdn_sem_destroy(rdn_sem_t *s) { (void)semDelete(s->id); free(s); }
void rdn_sem_give(rdn_sem_t *s)    { (void)semGive(s->id); }

/* Round up to whole ticks, never 0 (0 would mean NO_WAIT). */
static _Vx_ticks_t us_to_ticks(uint32_t us)
{
    uint64_t rate  = (uint64_t)sysClkRateGet();
    uint64_t ticks = ((uint64_t)us * rate + 999999u) / 1000000u;
    if (ticks == 0u) {
        ticks = 1u;
    }
    if (ticks > 0x7FFFFFFFu) {
        ticks = 0x7FFFFFFFu;
    }
    return (_Vx_ticks_t)ticks;
}

int rdn_sem_take(rdn_sem_t *s, uint32_t timeout_us)
{
    _Vx_ticks_t t = (timeout_us == RDN_WAIT_FOREVER) ? WAIT_FOREVER
                                                     : us_to_ticks(timeout_us);
    return (semTake(s->id, t) == OK) ? RDN_OK : RDN_E_TIMEOUT;
}

static int task_entry(_Vx_usr_arg_t arg)
{
    rdn_task_t *t = (rdn_task_t *)arg;
    t->fn(t->arg);
    (void)semGive(t->done);
    return 0;
}

rdn_task_t *rdn_task_spawn(const char *name, int prio, uint32_t stack,
                           rdn_task_fn fn, void *arg)
{
    rdn_task_t *t = (rdn_task_t *)calloc(1u, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->fn   = fn;
    t->arg  = arg;
    t->done = semBCreate(SEM_Q_FIFO, SEM_EMPTY);
    if (t->done == SEM_ID_NULL) {
        free(t);
        return NULL;
    }
    t->tid = taskSpawn((char *)name, prio, VX_FP_TASK, (size_t)stack,
                       (FUNCPTR)task_entry, (_Vx_usr_arg_t)t,
                       0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (t->tid == TASK_ID_ERROR) {
        (void)semDelete(t->done);
        free(t);
        return NULL;
    }
    return t;
}

void rdn_task_join(rdn_task_t *t)
{
    (void)semTake(t->done, WAIT_FOREVER);
    (void)semDelete(t->done);
    free(t);
}

uint64_t rdn_time_us(void)
{
#if defined(RDN_VX_USE_TIMEBASE)
    UINT32 hi, lo, hi2;
    uint64_t tb;
    do {                                   /* consistent 64-bit read */
        vxTimeBaseGet(&hi, &lo);
        vxTimeBaseGet(&hi2, &lo);
    } while (hi != hi2);
    tb = ((uint64_t)hi << 32) | lo;
    return (tb / (RDN_VX_TB_HZ / 1000000u));
#else
    uint64_t ticks = (uint64_t)tick64Get();
    return (ticks * 1000000u) / (uint64_t)sysClkRateGet();
#endif
}

uint32_t rdn_time_resolution_us(void)
{
#if defined(RDN_VX_USE_TIMEBASE)
    return 1u;
#else
    return (uint32_t)((1000000u + (uint32_t)sysClkRateGet() - 1u) /
                      (uint32_t)sysClkRateGet());
#endif
}

void rdn_sleep_us(uint32_t us)
{
    (void)taskDelay(us_to_ticks(us));
}

uint32_t rdn_osal_entropy(void)
{
    /* Tick count at start-up is nearly constant between boots; mix in the
     * time base (free-running, boot-time jitter) and the task id. */
    UINT32 hi = 0u, lo = 0u;
#if defined(_ARCH_PPC) || defined(__PPC__)
    vxTimeBaseGet(&hi, &lo);
#endif
    return (uint32_t)lo ^ ((uint32_t)hi << 16) ^ (uint32_t)tick64Get() ^
           (uint32_t)(ULONG)taskIdSelf();
}

void rdn_log(int level, const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    if (level > rdn_log_level) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* printf from task context; do not call from ISRs */
    (void)printf("%s", buf);
}
