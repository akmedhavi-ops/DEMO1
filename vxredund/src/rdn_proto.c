/*
 * rdn_proto.c - wire protocol encode / decode / sequence checking
 */
#include <string.h>
#include "rdn_proto.h"

void rdn_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint32_t rdn_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

int rdn_proto_encode(const rdn_msg_hdr_t *h, const uint8_t *payload,
                     uint8_t *out, uint32_t cap)
{
    uint32_t total = RDN_HDR_LEN + (uint32_t)h->len;
    uint32_t crc;

    if ((h->len > RDN_MAX_PAYLOAD) || (total > cap) ||
        ((h->len > 0u) && (payload == NULL))) {
        return RDN_E_PARAM;
    }
    rdn_put_u32(&out[0], RDN_MAGIC);
    out[4] = RDN_PROTO_VERSION;
    out[5] = h->type;
    out[6] = h->src;
    out[7] = h->dst;
    rdn_put_u32(&out[8],  h->cluster);
    rdn_put_u32(&out[12], h->incarnation);
    rdn_put_u32(&out[16], h->epoch);
    rdn_put_u32(&out[20], h->seq);
    rdn_put_u32(&out[24], h->txn);
    rdn_put_u32(&out[28], h->aux);
    rdn_put_u32(&out[32], h->offset);
    put_u16(&out[36], h->region);
    put_u16(&out[38], h->frag);
    put_u16(&out[40], h->len);
    put_u16(&out[42], h->flags);
    if (h->len > 0u) {
        (void)memcpy(&out[RDN_HDR_LEN], payload, h->len);
    }
    crc = rdn_crc32c(0u, out, 44u);
    crc = rdn_crc32c(crc, &out[RDN_HDR_LEN], h->len);
    rdn_put_u32(&out[44], crc);
    return (int)total;
}

int rdn_proto_decode(const uint8_t *buf, uint32_t len, uint32_t cluster,
                     uint8_t expect_src, uint8_t expect_dst,
                     rdn_msg_hdr_t *h, const uint8_t **payload)
{
    uint32_t crc;

    if ((len < RDN_HDR_LEN) || (len > RDN_MAX_FRAME)) {
        return RDN_E_PROTO;
    }
    if (rdn_get_u32(&buf[0]) != RDN_MAGIC) {
        return RDN_E_PROTO;
    }
    h->len = get_u16(&buf[40]);
    if ((RDN_HDR_LEN + (uint32_t)h->len) != len) {
        return RDN_E_PROTO;
    }
    crc = rdn_crc32c(0u, buf, 44u);
    crc = rdn_crc32c(crc, &buf[RDN_HDR_LEN], h->len);
    if (crc != rdn_get_u32(&buf[44])) {
        return RDN_E_CRC;
    }
    h->version = buf[4];
    h->type    = buf[5];
    h->src     = buf[6];
    h->dst     = buf[7];
    if (h->version != RDN_PROTO_VERSION) {
        return RDN_E_PROTO;
    }
    h->cluster     = rdn_get_u32(&buf[8]);
    h->incarnation = rdn_get_u32(&buf[12]);
    h->epoch       = rdn_get_u32(&buf[16]);
    h->seq         = rdn_get_u32(&buf[20]);
    h->txn         = rdn_get_u32(&buf[24]);
    h->aux         = rdn_get_u32(&buf[28]);
    h->offset      = rdn_get_u32(&buf[32]);
    h->region      = get_u16(&buf[36]);
    h->frag        = get_u16(&buf[38]);
    h->flags       = get_u16(&buf[42]);
    /* masquerade / mis-routing defence: cluster + both addresses */
    if ((h->cluster != cluster) || (h->src != expect_src) ||
        (h->dst != expect_dst)) {
        return RDN_E_CONFIG;
    }
    if ((h->type < (uint8_t)RDN_MSG_HB) ||
        (h->type > (uint8_t)RDN_MSG_RELINQUISH)) {
        return RDN_E_PROTO;
    }
    *payload = &buf[RDN_HDR_LEN];
    return RDN_OK;
}

void rdn_hb_encode(const rdn_hb_t *hb, uint8_t out[RDN_HB_LEN])
{
    out[0] = hb->state;
    out[1] = hb->health;
    out[2] = hb->link_mask;
    out[3] = 0u;
    rdn_put_u32(&out[4],  hb->applied_txn);
    rdn_put_u32(&out[8],  hb->uptime_ms);
    rdn_put_u32(&out[12], hb->layout_sig);
}

void rdn_hb_decode(const uint8_t in[RDN_HB_LEN], rdn_hb_t *hb)
{
    hb->state       = in[0];
    hb->health      = in[1];
    hb->link_mask   = in[2];
    hb->reserved    = in[3];
    hb->applied_txn = rdn_get_u32(&in[4]);
    hb->uptime_ms   = rdn_get_u32(&in[8]);
    hb->layout_sig  = rdn_get_u32(&in[12]);
}

/* Accept strictly increasing sequence numbers (modulo 2^32, forward window
 * of 2^31) within one sender incarnation. A new incarnation (peer reboot)
 * resets the window. Sequence numbers start at 1; 0 is never sent. */
int rdn_seq_accept(rdn_seq_rx_t *s, uint32_t incarnation, uint32_t seq,
                   uint32_t *lost)
{
    uint32_t delta;

    if ((!s->valid) || (incarnation != s->incarnation)) {
        s->valid       = 1u;
        s->incarnation = incarnation;
        s->last        = seq;
        return 1;
    }
    delta = seq - s->last;
    if ((delta == 0u) || (delta >= 0x80000000u)) {
        return 0;   /* repetition or old / re-ordered frame */
    }
    if (lost != NULL) {
        *lost += delta - 1u;
    }
    s->last = seq;
    return 1;
}
