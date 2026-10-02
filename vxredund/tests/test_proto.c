/*
 * test_proto.c - wire format, integrity and sequence checks
 * (EN 50159 threats: corruption, masquerade, repetition, re-sequencing)
 */
#include <string.h>
#include "rdn_proto.h"
#include "rdn_test.h"

static void mk_hdr(rdn_msg_hdr_t *h, uint16_t len)
{
    memset(h, 0, sizeof(*h));
    h->type = RDN_MSG_REPL_DATA;
    h->src = 1; h->dst = 2;
    h->cluster = 0xC0FFEE01u;
    h->incarnation = 0x11223344u;
    h->epoch = 7; h->seq = 99; h->txn = 12345; h->aux = 12344;
    h->offset = 0x00ABCDEFu; h->region = 3; h->frag = 4; h->len = len;
    h->flags = RDN_FLAG_FULL;
}

static void test_crc_check_value(void)
{
    CHECK_EQ(rdn_crc32c(0u, "123456789", 9u), 0xE3069283u);
    /* chaining equals one-shot */
    CHECK_EQ(rdn_crc32c(rdn_crc32c(0u, "1234", 4u), "56789", 5u), 0xE3069283u);
}

static void test_roundtrip(void)
{
    uint8_t pl[100], buf[RDN_MAX_FRAME];
    rdn_msg_hdr_t h, d;
    const uint8_t *p = NULL;
    int len, i;

    for (i = 0; i < 100; i++) pl[i] = (uint8_t)(i * 7);
    mk_hdr(&h, 100);
    len = rdn_proto_encode(&h, pl, buf, sizeof(buf));
    CHECK_EQ(len, RDN_HDR_LEN + 100);
    CHECK_EQ(buf[0], 'R'); CHECK_EQ(buf[3], '1');
    CHECK_EQ(rdn_proto_decode(buf, (uint32_t)len, h.cluster, 1, 2, &d, &p), RDN_OK);
    CHECK_EQ(d.type, h.type); CHECK_EQ(d.epoch, 7); CHECK_EQ(d.seq, 99);
    CHECK_EQ(d.txn, 12345); CHECK_EQ(d.aux, 12344); CHECK_EQ(d.offset, 0x00ABCDEF);
    CHECK_EQ(d.region, 3); CHECK_EQ(d.frag, 4); CHECK_EQ(d.len, 100);
    CHECK_EQ(d.flags, RDN_FLAG_FULL); CHECK_EQ(d.incarnation, 0x11223344u);
    CHECK(p == &buf[RDN_HDR_LEN]);
    CHECK(memcmp(p, pl, 100) == 0);
}

static void test_every_bit_flip_detected(void)
{
    uint8_t pl[100], buf[RDN_MAX_FRAME];
    rdn_msg_hdr_t h, d;
    const uint8_t *p;
    int len, bit, undetected = 0;

    memset(pl, 0x5A, sizeof(pl));
    mk_hdr(&h, 100);
    len = rdn_proto_encode(&h, pl, buf, sizeof(buf));
    for (bit = 0; bit < len * 8; bit++) {
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        if (rdn_proto_decode(buf, (uint32_t)len, h.cluster, 1, 2, &d, &p) == RDN_OK) {
            undetected++;
        }
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    CHECK_EQ(undetected, 0);
}

static void test_double_bit_flips_detected(void)
{
    uint8_t buf[RDN_MAX_FRAME], pl[64];
    rdn_msg_hdr_t h, d;
    const uint8_t *p;
    int len, a, b, undetected = 0;

    memset(pl, 0xA5, sizeof(pl));
    mk_hdr(&h, 64);
    len = rdn_proto_encode(&h, pl, buf, sizeof(buf));
    for (a = 0; a < len * 8; a += 3) {
        for (b = a + 1; b < len * 8; b += 5) {
            buf[a / 8] ^= (uint8_t)(1u << (a % 8));
            buf[b / 8] ^= (uint8_t)(1u << (b % 8));
            if (rdn_proto_decode(buf, (uint32_t)len, h.cluster, 1, 2, &d, &p) == RDN_OK) {
                undetected++;
            }
            buf[a / 8] ^= (uint8_t)(1u << (a % 8));
            buf[b / 8] ^= (uint8_t)(1u << (b % 8));
        }
    }
    CHECK_EQ(undetected, 0);
}

static void test_length_and_identity(void)
{
    uint8_t buf[RDN_MAX_FRAME], pl[10] = {0};
    rdn_msg_hdr_t h, d;
    const uint8_t *p;
    int len;

    mk_hdr(&h, 10);
    len = rdn_proto_encode(&h, pl, buf, sizeof(buf));
    CHECK(rdn_proto_decode(buf, (uint32_t)len - 1u, h.cluster, 1, 2, &d, &p) != RDN_OK);
    CHECK(rdn_proto_decode(buf, (uint32_t)len + 1u, h.cluster, 1, 2, &d, &p) != RDN_OK);
    CHECK(rdn_proto_decode(buf, 10u, h.cluster, 1, 2, &d, &p) != RDN_OK);
    CHECK_EQ(rdn_proto_decode(buf, (uint32_t)len, h.cluster + 1u, 1, 2, &d, &p), RDN_E_CONFIG);
    CHECK_EQ(rdn_proto_decode(buf, (uint32_t)len, h.cluster, 3, 2, &d, &p), RDN_E_CONFIG);
    CHECK_EQ(rdn_proto_decode(buf, (uint32_t)len, h.cluster, 1, 1, &d, &p), RDN_E_CONFIG);
    /* oversized payload refused by encoder */
    h.len = (uint16_t)(RDN_MAX_PAYLOAD + 1u);
    CHECK_EQ(rdn_proto_encode(&h, buf, buf, sizeof(buf)), RDN_E_PARAM);
    /* unknown type rejected even with valid CRC */
    mk_hdr(&h, 0);
    h.type = 77;
    len = rdn_proto_encode(&h, NULL, buf, sizeof(buf));
    CHECK_EQ(rdn_proto_decode(buf, (uint32_t)len, h.cluster, 1, 2, &d, &p), RDN_E_PROTO);
}

static void test_sequence_window(void)
{
    rdn_seq_rx_t s;
    uint32_t lost = 0;

    memset(&s, 0, sizeof(s));
    CHECK(rdn_seq_accept(&s, 5, 10, &lost));      /* first frame */
    CHECK(rdn_seq_accept(&s, 5, 11, &lost));
    CHECK(!rdn_seq_accept(&s, 5, 11, &lost));     /* repetition */
    CHECK(!rdn_seq_accept(&s, 5, 9, &lost));      /* old / reordered */
    CHECK(rdn_seq_accept(&s, 5, 15, &lost));      /* gap of 3 */
    CHECK_EQ(lost, 3);
    CHECK(rdn_seq_accept(&s, 6, 1, &lost));       /* new incarnation */
    CHECK_EQ(s.last, 1);
    s.last = 0xFFFFFFFEu;
    CHECK(rdn_seq_accept(&s, 6, 0xFFFFFFFFu, &lost));
    CHECK(rdn_seq_accept(&s, 6, 1u, &lost));       /* wrap (0 skipped) */
    CHECK(!rdn_seq_accept(&s, 6, 0x80000001u, &lost)); /* too far = old */
}

static void test_heartbeat_codec(void)
{
    rdn_hb_t a, b;
    uint8_t buf[RDN_HB_LEN];
    memset(&a, 0, sizeof(a));
    a.state = RDN_ST_STANDBY_HOT; a.health = 0; a.link_mask = 3;
    a.applied_txn = 0xDEADBEEFu; a.uptime_ms = 123456u; a.layout_sig = 0x01020304u;
    rdn_hb_encode(&a, buf);
    rdn_hb_decode(buf, &b);
    CHECK_EQ(b.state, a.state); CHECK_EQ(b.link_mask, 3);
    CHECK_EQ(b.applied_txn, a.applied_txn); CHECK_EQ(b.uptime_ms, a.uptime_ms);
    CHECK_EQ(b.layout_sig, a.layout_sig);
}

int main(void)
{
    printf("test_proto\n");
    RUN(test_crc_check_value);
    RUN(test_roundtrip);
    RUN(test_every_bit_flip_detected);
    RUN(test_double_bit_flips_detected);
    RUN(test_length_and_identity);
    RUN(test_sequence_window);
    RUN(test_heartbeat_codec);
    return test_report("test_proto");
}
