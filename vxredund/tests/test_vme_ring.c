/*
 * test_vme_ring.c - VME shared-memory ring link on host memory
 *
 * Two areas stand in for the two boards' slave windows. Node A's link uses
 * (local=areaA, remote=areaB), node B's (local=areaB, remote=areaA).
 */
#include <pthread.h>
#include <string.h>
#include "rdn.h"
#include "rdn_osal.h"
#include "rdn_test.h"

#define SLOTS 8u
#define SLOTSZ 256u

static uint32_t area_a[8192], area_b[8192];

static void open_pair(rdn_link_t *a, rdn_link_t *b)
{
    rdn_vme_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.slot_count = SLOTS; c.slot_size = SLOTSZ; c.poll_us = 100;
    c.area_size = sizeof(area_a);
    c.local_base = area_a; c.remote_base = area_b;
    CHECK_EQ(rdn_link_vme_open(&c, a), RDN_OK);
    c.local_base = area_b; c.remote_base = area_a;
    CHECK_EQ(rdn_link_vme_open(&c, b), RDN_OK);
}

static void test_params(void)
{
    rdn_vme_cfg_t c;
    rdn_link_t l;
    memset(&c, 0, sizeof(c));
    c.slot_count = 6; c.slot_size = 256; c.area_size = sizeof(area_a);
    c.local_base = area_a; c.remote_base = area_b;
    CHECK_EQ(rdn_link_vme_open(&c, &l), RDN_E_PARAM);      /* not pow2 */
    c.slot_count = 8; c.area_size = 100;
    CHECK_EQ(rdn_link_vme_open(&c, &l), RDN_E_PARAM);      /* too small */
    CHECK_EQ(rdn_link_vme_area_size(8, 256), 64 + 8 * (8 + 256));
}

static void test_basic_wrap_and_full(void)
{
    rdn_link_t a, b;
    uint8_t tx[SLOTSZ], rx[SLOTSZ];
    int i, n;

    memset(area_a, 0xEE, sizeof(area_a));      /* garbage = cold memory */
    memset(area_b, 0xEE, sizeof(area_b));
    open_pair(&a, &b);
    CHECK_EQ(b.ops->recv(b.ctx, rx, sizeof(rx), 1000), 0);   /* empty */
    for (i = 0; i < 100; i++) {                               /* wraps */
        uint32_t len = (uint32_t)(1 + (i * 37) % SLOTSZ);
        memset(tx, i, len);
        CHECK_EQ(a.ops->send(a.ctx, tx, len), RDN_OK);
        n = b.ops->recv(b.ctx, rx, sizeof(rx), 1000);
        CHECK_EQ(n, (int)len);
        CHECK(memcmp(tx, rx, len) == 0);
    }
    for (i = 0; i < (int)SLOTS; i++) {
        CHECK_EQ(a.ops->send(a.ctx, tx, 10), RDN_OK);
    }
    CHECK_EQ(a.ops->send(a.ctx, tx, 10), RDN_E_FULL);         /* bounded */
    for (i = 0; i < (int)SLOTS; i++) {
        CHECK_EQ(b.ops->recv(b.ctx, rx, sizeof(rx), 1000), 10);
    }
    CHECK_EQ(a.ops->send(a.ctx, tx, SLOTSZ + 1), RDN_E_PARAM);
    /* garbage in a slot (bad seq word) is never delivered */
    CHECK_EQ(a.ops->send(a.ctx, tx, 10), RDN_OK);
    {   /* corrupt the seq word of every slot in B's area */
        uint32_t k;
        for (k = 0; k < SLOTS; k++) area_b[(64 + k * (8 + SLOTSZ)) / 4 + 1] ^= 0x10000u;
    }
    CHECK_EQ(b.ops->recv(b.ctx, rx, sizeof(rx), 1000), 0);
    CHECK_EQ(a.ops->send(a.ctx, tx, 11), RDN_OK);           /* recovers */
    CHECK_EQ(b.ops->recv(b.ctx, rx, sizeof(rx), 1000), 11);
    /* reverse direction is independent */
    CHECK_EQ(b.ops->send(b.ctx, tx, 20), RDN_OK);
    CHECK_EQ(a.ops->recv(a.ctx, rx, sizeof(rx), 1000), 20);
    a.ops->close(a.ctx);
    b.ops->close(b.ctx);
}

static void test_consumer_restart_resync(void)
{
    rdn_link_t a, b;
    uint8_t tx[64], rx[SLOTSZ];
    int i;

    open_pair(&a, &b);
    memset(tx, 0xF0, sizeof(tx));
    for (i = 0; i < 5; i++) {
        CHECK_EQ(a.ops->send(a.ctx, tx, 64), RDN_OK);         /* pending */
    }
    b.ops->close(b.ctx);                  /* B "reboots" (cold)       */
    {
        rdn_vme_cfg_t c;
        memset(&c, 0, sizeof(c));
        c.slot_count = SLOTS; c.slot_size = SLOTSZ; c.poll_us = 100;
        c.area_size = sizeof(area_b);
        c.local_base = area_b; c.remote_base = area_a;
        CHECK_EQ(rdn_link_vme_open(&c, &b), RDN_OK);
    }
    /* producer must not be stuck and new frames must flow */
    for (i = 0; i < 20; i++) {
        memset(tx, i, sizeof(tx));
        CHECK_EQ(a.ops->send(a.ctx, tx, 64), RDN_OK);
        {
            int n = b.ops->recv(b.ctx, rx, sizeof(rx), 1000);
            /* frames pending before the restart (0xF0) may still arrive
             * first; after that delivery must be exact */
            while (i == 0 && n > 0 && rx[0] == 0xF0) {
                n = b.ops->recv(b.ctx, rx, sizeof(rx), 1000);
            }
            CHECK_EQ(n, 64);
            CHECK_EQ(rx[0], i);
        }
    }
    a.ops->close(a.ctx);
    b.ops->close(b.ctx);
}

/* Regression: ring full behind a dead consumer, whose cold restart
 * publishes an index that makes the ring still look exactly full. */
static void test_full_ring_then_consumer_cold_restart(void)
{
    rdn_link_t a, b;
    rdn_vme_cfg_t c;
    uint8_t tx[32], rx[SLOTSZ];
    int i;

    memset(area_a, 0, sizeof(area_a));
    memset(area_b, 0, sizeof(area_b));
    open_pair(&a, &b);
    b.ops->close(b.ctx);                         /* consumer dies at cons=0 */
    for (i = 0; i < (int)SLOTS; i++) {
        CHECK_EQ(a.ops->send(a.ctx, tx, sizeof(tx)), RDN_OK);
    }
    CHECK_EQ(a.ops->send(a.ctx, tx, sizeof(tx)), RDN_E_FULL);
    memset(&c, 0, sizeof(c));
    c.slot_count = SLOTS; c.slot_size = SLOTSZ; c.poll_us = 100;
    c.area_size = sizeof(area_b);
    c.local_base = area_b; c.remote_base = area_a;
    CHECK_EQ(rdn_link_vme_open(&c, &b), RDN_OK); /* cold: publishes cons=0 */
    for (i = 0; i < 50; i++) {
        memset(tx, i, sizeof(tx));
        CHECK_EQ(a.ops->send(a.ctx, tx, sizeof(tx)), RDN_OK);
        CHECK_EQ(b.ops->recv(b.ctx, rx, sizeof(rx), 1000), (int)sizeof(tx));
        CHECK_EQ(rx[0], i);
    }
    a.ops->close(a.ctx);
    b.ops->close(b.ctx);
}

typedef struct thr { rdn_link_t l; int count; int errors; } thr_t;

static void *producer(void *arg)
{
    thr_t *t = (thr_t *)arg;
    uint8_t buf[SLOTSZ];
    int i;
    for (i = 0; i < t->count; i++) {
        uint32_t len = (uint32_t)(8 + (i % 200));
        memset(buf, (uint8_t)i, len);
        memcpy(buf, &i, sizeof(i));
        while (t->l.ops->send(t->l.ctx, buf, len) == RDN_E_FULL) {
            rdn_sleep_us(50);
        }
    }
    return NULL;
}

static void test_threaded_ordering(void)
{
    rdn_link_t a, b;
    thr_t p;
    pthread_t th;
    uint8_t rx[SLOTSZ];
    int expect = 0, bad = 0;

    open_pair(&a, &b);
    p.l = a; p.count = 20000; p.errors = 0;
    pthread_create(&th, NULL, producer, &p);
    while (expect < p.count) {
        int n = b.ops->recv(b.ctx, rx, sizeof(rx), 200000), v, k;
        if (n <= 0) { bad++; break; }
        memcpy(&v, rx, sizeof(v));
        if (v != expect || n != 8 + (expect % 200)) bad++;
        for (k = (int)sizeof(v); k < n; k++) {
            if (rx[k] != (uint8_t)expect) { bad++; break; }
        }
        expect++;
    }
    pthread_join(th, NULL);
    CHECK_EQ(bad, 0);
    CHECK_EQ(expect, 20000);
    a.ops->close(a.ctx);
    b.ops->close(b.ctx);
}

int main(void)
{
    printf("test_vme_ring\n");
    RUN(test_params);
    RUN(test_basic_wrap_and_full);
    RUN(test_consumer_restart_resync);
    RUN(test_full_ring_then_consumer_cold_restart);
    RUN(test_threaded_ordering);
    return test_report("test_vme_ring");
}
