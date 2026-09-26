/*
 * rdn_link_udp.c - UDP link over a dedicated (Gigabit) Ethernet segment.
 *
 * Intended for a point-to-point cable (or private VLAN) between the two
 * boards, bound to the interface address of that port (on MVME5500 the
 * 82544 Gigabit port, typically "gei0" - check your BSP). Frames whose
 * source address is not the configured peer are discarded.
 */
#if defined(__vxworks) || defined(__VXWORKS__)
#include <vxWorks.h>
#include <sockLib.h>
#include <inetLib.h>
#include <selectLib.h>
#include <ioLib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#define RDN_SOCK_BUF(p) ((char *)(p))
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define RDN_SOCK_BUF(p) ((void *)(p))
#endif
#include <string.h>

#include "rdn.h"
#include "rdn_osal.h"

typedef struct udp_ctx {
    int                fd;
    struct sockaddr_in peer;
} udp_ctx_t;

static int udp_send(void *ctx, const uint8_t *buf, uint32_t len)
{
    udp_ctx_t *u = (udp_ctx_t *)ctx;
    int n = (int)sendto(u->fd, RDN_SOCK_BUF(buf), (size_t)len, 0,
                        (struct sockaddr *)&u->peer, (int)sizeof(u->peer));
    return (n == (int)len) ? RDN_OK : RDN_E_IO;
}

static int udp_recv(void *ctx, uint8_t *buf, uint32_t cap, uint32_t timeout_us)
{
    udp_ctx_t *u = (udp_ctx_t *)ctx;
    struct sockaddr_in from;
    fd_set rs;
    struct timeval tv;
    int n;
#if defined(__vxworks) || defined(__VXWORKS__)
    int flen = (int)sizeof(from);
#else
    socklen_t flen = (socklen_t)sizeof(from);
#endif

    FD_ZERO(&rs);
    FD_SET(u->fd, &rs);
    tv.tv_sec  = (long)(timeout_us / 1000000u);
    tv.tv_usec = (long)(timeout_us % 1000000u);
    n = select(u->fd + 1, &rs, NULL, NULL, &tv);
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        return RDN_E_IO;
    }
    n = (int)recvfrom(u->fd, RDN_SOCK_BUF(buf), (size_t)cap, 0,
                      (struct sockaddr *)&from, &flen);
    if (n < 0) {
        return RDN_E_IO;
    }
    if ((from.sin_addr.s_addr != u->peer.sin_addr.s_addr) ||
        (from.sin_port != u->peer.sin_port)) {
        return 0;   /* not from our peer: ignore */
    }
    return n;
}

static void udp_close(void *ctx)
{
    udp_ctx_t *u = (udp_ctx_t *)ctx;
    (void)close(u->fd);
    rdn_osal_free(u);
}

static const rdn_link_ops_t udp_ops = { "udp", udp_send, udp_recv, udp_close };

int rdn_link_udp_open(const rdn_udp_cfg_t *cfg, rdn_link_t *out)
{
    udp_ctx_t *u;
    struct sockaddr_in local;
    int one = 1;

    if ((cfg == NULL) || (out == NULL) || (cfg->peer_ip == NULL) ||
        (cfg->local_port == 0u) || (cfg->peer_port == 0u)) {
        return RDN_E_PARAM;
    }
    u = (udp_ctx_t *)rdn_osal_alloc(sizeof(*u));
    if (u == NULL) {
        return RDN_E_NOMEM;
    }
    u->fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (u->fd < 0) {
        rdn_osal_free(u);
        return RDN_E_IO;
    }
    (void)setsockopt(u->fd, SOL_SOCKET, SO_REUSEADDR,
                     RDN_SOCK_BUF(&one), (int)sizeof(one));
    if (cfg->tos != 0) {
        int tos = cfg->tos;
        (void)setsockopt(u->fd, IPPROTO_IP, IP_TOS,
                         RDN_SOCK_BUF(&tos), (int)sizeof(tos));
    }
    (void)memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(cfg->local_port);
    local.sin_addr.s_addr = (cfg->local_ip != NULL) ?
                            inet_addr(RDN_SOCK_BUF(cfg->local_ip)) :
                            htonl(INADDR_ANY);
    if (bind(u->fd, (struct sockaddr *)&local, (int)sizeof(local)) != 0) {
        (void)close(u->fd);
        rdn_osal_free(u);
        return RDN_E_IO;
    }
    (void)memset(&u->peer, 0, sizeof(u->peer));
    u->peer.sin_family      = AF_INET;
    u->peer.sin_port        = htons(cfg->peer_port);
    u->peer.sin_addr.s_addr = inet_addr(RDN_SOCK_BUF(cfg->peer_ip));
    out->ops = &udp_ops;
    out->ctx = u;
    return RDN_OK;
}
