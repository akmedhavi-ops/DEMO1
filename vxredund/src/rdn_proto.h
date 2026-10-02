/*
 * rdn_proto.h - wire protocol (internal)
 *
 * All multi-byte fields are big-endian and serialised byte by byte, so the
 * format is independent of compiler struct layout and host endianness
 * (PowerPC target, x86 host tests).
 *
 *  off size field        off size field
 *   0   4  magic 'RDN1'    28   4  aux  (base txn / applied txn)
 *   4   1  version         32   4  offset
 *   5   1  type            36   2  region
 *   6   1  src node        38   2  frag
 *   7   1  dst node        40   2  payload length
 *   8   4  cluster id      42   2  flags
 *  12   4  incarnation     44   4  CRC-32C (header[0..44) + payload)
 *  16   4  epoch
 *  20   4  seq (per link)
 *  24   4  txn
 *
 * Defences against the EN 50159 threats: see docs/SAFETY.md.
 */
#ifndef RDN_PROTO_H
#define RDN_PROTO_H

#include <stdint.h>
#include "rdn.h"

#define RDN_MAGIC          0x52444E31u /* "RDN1" */
#define RDN_PROTO_VERSION  1u
#define RDN_HDR_LEN        48u
#define RDN_MAX_PAYLOAD    (RDN_MAX_FRAME - RDN_HDR_LEN)

typedef enum {
    RDN_MSG_HB          = 1,
    RDN_MSG_REPL_DATA   = 2,
    RDN_MSG_REPL_COMMIT = 3,
    RDN_MSG_ACK         = 4,
    RDN_MSG_SYNC_REQ    = 5,
    RDN_MSG_RELINQUISH  = 6
} rdn_msg_type_t;

#define RDN_FLAG_FULL  0x0001u   /* REPL_*: part of a full snapshot */

typedef struct rdn_msg_hdr {
    uint8_t  version;
    uint8_t  type;
    uint8_t  src;
    uint8_t  dst;
    uint32_t cluster;
    uint32_t incarnation;
    uint32_t epoch;
    uint32_t seq;
    uint32_t txn;
    uint32_t aux;
    uint32_t offset;
    uint16_t region;
    uint16_t frag;
    uint16_t len;
    uint16_t flags;
} rdn_msg_hdr_t;

/* heartbeat payload */
#define RDN_HB_LEN 16u
typedef struct rdn_hb {
    uint8_t  state;
    uint8_t  health;       /* 0 = healthy                                 */
    uint8_t  link_mask;    /* links on which sender currently hears peer  */
    uint8_t  reserved;
    uint32_t applied_txn;  /* standby: last applied; active: last commit  */
    uint32_t uptime_ms;
    uint32_t layout_sig;   /* region layout signature                     */
} rdn_hb_t;

/* commit payload */
#define RDN_COMMIT_LEN 8u

uint32_t rdn_crc32c(uint32_t crc, const void *data, uint32_t len);

int  rdn_proto_encode(const rdn_msg_hdr_t *h, const uint8_t *payload,
                      uint8_t *out, uint32_t cap);
/* Validates magic, version, length, CRC, cluster and source node.
 * On success *payload points into buf. */
int  rdn_proto_decode(const uint8_t *buf, uint32_t len, uint32_t cluster,
                      uint8_t expect_src, uint8_t expect_dst,
                      rdn_msg_hdr_t *h, const uint8_t **payload);

void rdn_hb_encode(const rdn_hb_t *hb, uint8_t out[RDN_HB_LEN]);
void rdn_hb_decode(const uint8_t in[RDN_HB_LEN], rdn_hb_t *hb);

void     rdn_put_u32(uint8_t *p, uint32_t v);
uint32_t rdn_get_u32(const uint8_t *p);

/* Sequence acceptance (repetition / re-sequencing defence).
 * Returns 1 = accept, 0 = reject (replay/old). *lost += gap size. */
typedef struct rdn_seq_rx {
    uint32_t incarnation;
    uint32_t last;
    uint8_t  valid;
} rdn_seq_rx_t;
int rdn_seq_accept(rdn_seq_rx_t *s, uint32_t incarnation, uint32_t seq,
                   uint32_t *lost);

#endif /* RDN_PROTO_H */
