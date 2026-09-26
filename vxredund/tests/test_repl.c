/*
 * test_repl.c - replication engine: deltas, atomicity, gap and corruption
 * detection, full resynchronisation, layout mismatch.
 */
#include <string.h>
#include "rdn_repl.h"
#include "rdn_test.h"

#define R1 8u
#define R2 5000u

typedef struct frame {
    rdn_msg_hdr_t h;
    uint8_t pl[RDN_MAX_PAYLOAD];
} frame_t;

typedef struct capture {
    frame_t f[64];
    int n;
} capture_t;

static int cap_emit(void *ctx, const rdn_msg_hdr_t *h, const uint8_t *pl)
{
    capture_t *c = (capture_t *)ctx;
    if (c->n >= 64) return -1;
    c->f[c->n].h = *h;
    memcpy(c->f[c->n].pl, pl, h->len);
    c->n++;
    return 0;
}

typedef struct side {
    rdn_repl_t r;
    uint8_t a[R1], b[R2];
    uint8_t mem[2 * (R1 + R2)];
} side_t;

static void side_init(side_t *s, uint32_t block)
{
    memset(s, 0, sizeof(*s));
    rdn_repl_init(&s->r, block);
    CHECK_EQ(rdn_repl_add(&s->r, 1, s->a, R1, s->mem, s->mem + R1), RDN_OK);
    CHECK_EQ(rdn_repl_add(&s->r, 2, s->b, R2, s->mem + 2 * R1, s->mem + 2 * R1 + R2), RDN_OK);
}

/* deliver frames [from, to) except index skip; returns last result */
static int deliver(side_t *sb, capture_t *c, int skip)
{
    int i, rc = RDN_REPL_RX_OK;
    for (i = 0; i < c->n; i++) {
        if (i == skip) continue;
        if (c->f[i].h.type == RDN_MSG_REPL_DATA) {
            int r = rdn_repl_rx_data(&sb->r, &c->f[i].h, c->f[i].pl);
            if (r != RDN_REPL_RX_OK) rc = r;
        } else {
            int r = rdn_repl_rx_commit(&sb->r, &c->f[i].h, c->f[i].pl);
            if (r != RDN_REPL_RX_OK || rc == RDN_REPL_RX_OK) rc = r;
        }
    }
    return rc;
}

static int same(const side_t *x, const side_t *y)
{
    return memcmp(x->a, y->a, R1) == 0 && memcmp(x->b, y->b, R2) == 0;
}

static capture_t cap;

static int commit(side_t *sa, int full)
{
    uint32_t txn = 0;
    cap.n = 0;
    CHECK_EQ(rdn_repl_build(&sa->r, full, RDN_MAX_PAYLOAD, cap_emit, &cap, &txn), RDN_OK);
    return (int)txn;
}

static side_t A, B;

static void test_full_then_deltas(void)
{
    int i;
    side_init(&A, 64); side_init(&B, 64);
    for (i = 0; i < (int)R2; i++) A.b[i] = (uint8_t)i;
    A.a[0] = 42;
    commit(&A, 1);
    /* full snapshot: ceil(5000/1424)=4 frames for b, 1 for a, + commit */
    CHECK_EQ(cap.n, 1 + 4 + 1);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_SYNCED);
    CHECK(B.r.synced);
    CHECK(same(&A, &B));

    A.b[100] = 1; A.b[4000] = 2;       /* two separate dirty blocks */
    commit(&A, 0);
    CHECK_EQ(cap.n, 2 + 1);
    CHECK_EQ(cap.f[0].h.len, 64);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_APPLIED);
    CHECK(same(&A, &B));
    CHECK_EQ(B.r.applied_txn, A.r.txn);

    commit(&A, 0);                     /* no change: commit only */
    CHECK_EQ(cap.n, 1);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_APPLIED);

    for (i = 0; i < 3000; i++) A.b[i] ^= 0xFF;   /* large contiguous delta */
    commit(&A, 0);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_APPLIED);
    CHECK(same(&A, &B));
}

static void test_lost_fragment_is_atomic(void)
{
    uint8_t before[R2];
    memcpy(before, B.b, R2);
    A.b[10] = 0x11; A.b[2000] = 0x22; A.b[4999] = 0x33;
    commit(&A, 0);
    CHECK_EQ(cap.n, 4);
    CHECK_EQ(deliver(&B, &cap, 1), RDN_REPL_RX_LOST);   /* drop middle frag */
    CHECK(memcmp(before, B.b, R2) == 0);                /* nothing applied */
    CHECK(!B.r.synced);
    /* further deltas are ignored until a full snapshot arrives */
    A.b[0] = 0x44;
    commit(&A, 0);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_OK);
    CHECK(memcmp(before, B.b, R2) == 0);
    commit(&A, 1);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_SYNCED);
    CHECK(same(&A, &B));
}

static void test_lost_commit_detected(void)
{
    A.a[1] = 9;
    commit(&A, 0);
    CHECK_EQ(deliver(&B, &cap, cap.n - 1), RDN_REPL_RX_OK);  /* commit lost */
    A.a[2] = 9;
    commit(&A, 0);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_LOST);        /* base gap */
    commit(&A, 1);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_SYNCED);
    CHECK(same(&A, &B));
}

static void test_whole_txn_lost_detected(void)
{
    A.a[3] = 1;
    commit(&A, 0);                      /* never delivered */
    A.a[4] = 1;
    commit(&A, 0);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_LOST);
    commit(&A, 1);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_SYNCED);
}

static void test_payload_corruption_detected_by_state_crc(void)
{
    uint8_t before[R2];
    memcpy(before, B.b, R2);
    A.b[500] = 0x77;
    commit(&A, 0);
    cap.f[0].pl[3] ^= 0x01;             /* corruption that passed link CRC */
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_LOST);
    CHECK(memcmp(before, B.b, R2) == 0);
    commit(&A, 1);
    CHECK_EQ(deliver(&B, &cap, -1), RDN_REPL_RX_SYNCED);
    CHECK(same(&A, &B));
}

static void test_layout_mismatch(void)
{
    side_t *C = (side_t *)calloc(1, sizeof(side_t));
    rdn_repl_init(&C->r, 64);
    CHECK_EQ(rdn_repl_add(&C->r, 1, C->a, R1, C->mem, C->mem + R1), RDN_OK);
    CHECK_EQ(rdn_repl_add(&C->r, 2, C->b, R2 - 4, C->mem + 2 * R1, C->mem + 2 * R1 + R2), RDN_OK);
    commit(&A, 1);
    CHECK_EQ(deliver(C, &cap, -1), RDN_REPL_RX_LAYOUT);
    CHECK(!C->r.synced);
    free(C);
}

static void test_registration_rules(void)
{
    rdn_repl_t r;
    uint8_t buf[16], mem[32];
    rdn_repl_init(&r, 64);
    CHECK_EQ(rdn_repl_add(&r, 1, buf, 16, mem, mem + 16), RDN_OK);
    CHECK_EQ(rdn_repl_add(&r, 1, buf, 16, mem, mem + 16), RDN_E_PARAM); /* dup id */
    CHECK_EQ(rdn_repl_add(&r, 2, NULL, 16, mem, mem + 16), RDN_E_PARAM);
    CHECK_EQ(rdn_repl_add(&r, 3, buf, 0, mem, mem + 16), RDN_E_PARAM);
}

static void test_randomised(void)
{
    uint32_t rng = 7;
    int it, i, drops = 0, prev_drop = 0;
    side_init(&A, 32); side_init(&B, 32);
    commit(&A, 1);
    deliver(&B, &cap, -1);
    for (it = 0; it < 2000; it++) {
        int nchg = (int)(rng % 20), rc, skip = -1;
        for (i = 0; i < nchg; i++) {
            rng = rng * 1103515245u + 12345u;
            A.b[(rng >> 8) % R2] = (uint8_t)rng;
        }
        rng = rng * 1103515245u + 12345u;
        commit(&A, !B.r.synced);
        if ((rng >> 16) % 50 == 0) { skip = (int)((rng >> 4) % (uint32_t)cap.n); drops++; }
        rc = deliver(&B, &cap, skip);
        if (skip < 0 && !prev_drop) {
            CHECK(rc == RDN_REPL_RX_APPLIED || rc == RDN_REPL_RX_SYNCED ||
                  (rc == RDN_REPL_RX_OK && !B.r.synced));
        }
        prev_drop = (skip >= 0);
        /* invariant: replica always equals SOME committed state; when
         * synced and nothing dropped it equals the active's state */
        if (B.r.synced && skip < 0) {
            CHECK(same(&A, &B));
        }
    }
    CHECK(drops > 10);
}

int main(void)
{
    printf("test_repl\n");
    RUN(test_full_then_deltas);
    RUN(test_lost_fragment_is_atomic);
    RUN(test_lost_commit_detected);
    RUN(test_whole_txn_lost_detected);
    RUN(test_payload_corruption_detected_by_state_crc);
    RUN(test_layout_mismatch);
    RUN(test_registration_rules);
    RUN(test_randomised);
    return test_report("test_repl");
}
