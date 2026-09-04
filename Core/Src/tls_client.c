/**
 * @file    tls_client.c
 * @brief   TLS-encrypted CMSIS-DAP remote client using lwIP ALTCP + mbedTLS.
 *
 * This module is the TLS-mode equivalent of remoteServer.c.  It:
 *   1. Starts a plain TCP server on DAP_TCP_SERVER_PORT for local LAN access.
 *   2. Opens an outbound TLS-encrypted connection to the remote relay server
 *      at REMOTE_SERVER_IP:REMOTE_SERVER_PORT.
 *   3. Processes DAP packets received from either connection and returns
 *      DAP responses.
 *   4. Automatically reconnects the TLS link on disconnection.
 *
 * Only compiled when DAP_SERVER_MODE == DAP_SERVER_MODE_TLS.
 */

#include "dap_server_config.h"
#if (DAP_SERVER_MODE == DAP_SERVER_MODE_TLS)

#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/timeouts.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "DAP.h"
#include "tls_certs.h"

/* ---- DAP packet framing (same as remoteServer.c) ----------------------- */

#define TCP_SERVER_PORT         DAP_TCP_SERVER_PORT

#define DAP_PKT_SIZE            DAP_TCP_PKT_SIZE
#define DAP_PKT_HDR_SIGNATURE   0x00504144UL
#define DAP_PKT_TYPE_REQUEST    0x01U
#define DAP_PKT_TYPE_RESPONSE   0x02U

struct cmsis_dap_tcp_packet_hdr {
    uint32_t signature;
    uint16_t length;
    uint8_t  packet_type;
    uint8_t  reserved;
} __attribute__((__packed__));

#define DAP_TOTAL_PKT_SIZE  ((uint16_t)(sizeof(struct cmsis_dap_tcp_packet_hdr) + DAP_PKT_SIZE))
#define MSGBUF_CAPACITY     ((uint16_t)(3U * DAP_TOTAL_PKT_SIZE))

struct msgbuf_t {
    uint8_t  data[MSGBUF_CAPACITY];
    uint16_t len;
};

/* ---- Connection context ------------------------------------------------ */

enum {
    TCP_CONN_ROLE_SERVER = 0,
    TCP_CONN_ROLE_REMOTE
};

/**
 * LAN server context uses raw tcp_pcb (no TLS overhead for local LAN).
 */
struct tcp_lan_ctx {
    uint8_t          role;
    struct msgbuf_t  rx;
    uint8_t          tx_buf[DAP_TOTAL_PKT_SIZE];
    uint8_t          dap_response[DAP_PKT_SIZE];
    struct tcp_pcb  *pcb;
};

/**
 * Remote TLS context uses altcp_pcb (TLS-wrapped TCP).
 */
struct tls_remote_ctx {
    uint8_t          role;
    struct msgbuf_t  rx;
    uint8_t          tx_buf[DAP_TOTAL_PKT_SIZE];
    uint8_t          dap_response[DAP_PKT_SIZE];
    struct altcp_pcb *pcb;
};

static struct tcp_lan_ctx    s_server_ctx = { .role = TCP_CONN_ROLE_SERVER };
static struct tls_remote_ctx s_remote_ctx = { .role = TCP_CONN_ROLE_REMOTE };
static uint8_t s_remote_connected = 0;
static uint8_t s_remote_reconnect_pending = 0;
static struct tcp_pcb *s_active_client = NULL;

/* TLS configuration (created once, shared across reconnects) */
static struct altcp_tls_config *s_tls_config = NULL;

/* ---- Forward declarations ---------------------------------------------- */

/* LAN server (plain TCP) */
static err_t lan_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t lan_conn_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void  lan_conn_err(void *arg, err_t err);
static err_t lan_conn_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void  lan_conn_close(struct tcp_lan_ctx *ctx, struct tcp_pcb *tpcb, uint8_t close_pcb);

/* Remote TLS client */
static void  tls_remote_try_connect(void);
static void  tls_remote_schedule_reconnect(void);
static void  tls_remote_reconnect_timer(void *arg);
static err_t tls_remote_connected_cb(void *arg, struct altcp_pcb *conn, err_t err);
static err_t tls_remote_recv(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err);
static err_t tls_remote_sent(void *arg, struct altcp_pcb *conn, u16_t len);
static void  tls_remote_err(void *arg, err_t err);
static err_t tls_remote_poll(void *arg, struct altcp_pcb *conn);
static void  tls_remote_close(struct tls_remote_ctx *ctx, uint8_t close_pcb);

/* ---- Helpers (same as remoteServer.c) ---------------------------------- */

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static void msgbuf_init(struct msgbuf_t *buf)
{
    buf->len = 0;
}

static err_t msgbuf_add_pbuf(struct msgbuf_t *buf, const struct pbuf *p)
{
    uint16_t free_space = (uint16_t)(sizeof(buf->data) - buf->len);
    if (p->tot_len > free_space) {
        return ERR_MEM;
    }
    pbuf_copy_partial((struct pbuf *)p, buf->data + buf->len, p->tot_len, 0);
    buf->len = (uint16_t)(buf->len + p->tot_len);
    return ERR_OK;
}

static void msgbuf_consume(struct msgbuf_t *buf, uint16_t n)
{
    if (n >= buf->len) {
        buf->len = 0;
        return;
    }
    memmove(buf->data, buf->data + n, buf->len - n);
    buf->len = (uint16_t)(buf->len - n);
}

static int msgbuf_parse(const struct msgbuf_t *buf,
                        uint16_t *payload_len,
                        const uint8_t **payload,
                        uint16_t *total_len)
{
    const uint16_t hdr_size = (uint16_t)sizeof(struct cmsis_dap_tcp_packet_hdr);

    if (buf->len < hdr_size)
        return 0;

    const uint8_t *raw = buf->data;
    uint32_t signature = read_le32(raw + 0);
    uint16_t length    = read_le16(raw + 4);
    uint8_t  pkt_type  = raw[6];

    if (signature != DAP_PKT_HDR_SIGNATURE)
        return -1;
    if (pkt_type != DAP_PKT_TYPE_REQUEST)
        return -1;
    if (length > DAP_PKT_SIZE)
        return -1;

    if (buf->len < (uint16_t)(hdr_size + length))
        return 0;

    *payload_len = length;
    *payload     = raw + hdr_size;
    *total_len   = (uint16_t)(hdr_size + length);
    return 1;
}

/* ======================================================================== */
/*  LAN TCP Server (plain, no TLS — same as remoteServer.c server part)     */
/* ======================================================================== */

static err_t lan_send_dap_response(struct tcp_pcb *tpcb,
                                   struct tcp_lan_ctx *ctx,
                                   const uint8_t *payload,
                                   uint16_t len)
{
    uint16_t total_len;
    if (len > DAP_PKT_SIZE) return ERR_VAL;

    write_le32(ctx->tx_buf + 0, DAP_PKT_HDR_SIGNATURE);
    write_le16(ctx->tx_buf + 4, len);
    ctx->tx_buf[6] = DAP_PKT_TYPE_RESPONSE;
    ctx->tx_buf[7] = 0;

    memcpy(ctx->tx_buf + sizeof(struct cmsis_dap_tcp_packet_hdr), payload, len);
    total_len = (uint16_t)(sizeof(struct cmsis_dap_tcp_packet_hdr) + len);

    err_t werr = tcp_write(tpcb, ctx->tx_buf, total_len, TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) return werr;
    return tcp_output(tpcb);
}

static void lan_setup_conn(struct tcp_pcb *pcb, struct tcp_lan_ctx *ctx)
{
    msgbuf_init(&ctx->rx);
    ctx->pcb = pcb;
    tcp_arg(pcb, ctx);
    tcp_recv(pcb, lan_conn_recv);
    tcp_err(pcb, lan_conn_err);
    tcp_sent(pcb, lan_conn_sent);
    tcp_poll(pcb, NULL, 0);
}

static err_t lan_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    if (s_active_client != NULL) {
        tcp_close(newpcb);
        return ERR_OK;
    }

    s_active_client = newpcb;
    lan_setup_conn(newpcb, &s_server_ctx);
    return ERR_OK;
}

static err_t lan_conn_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct tcp_lan_ctx *ctx = (struct tcp_lan_ctx *)arg;

    if (err != ERR_OK || p == NULL) {
        if (p != NULL) pbuf_free(p);
        if (err == ERR_OK && p == NULL) {
            lan_conn_close(ctx, tpcb, 1);
        }
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    if ((ctx == NULL) || (msgbuf_add_pbuf(&ctx->rx, p) != ERR_OK)) {
        pbuf_free(p);
        lan_conn_close(ctx, tpcb, 1);
        return ERR_OK;
    }
    pbuf_free(p);

    while (1) {
        uint16_t payload_len, total_len;
        const uint8_t *payload;
        int parse_ret = msgbuf_parse(&ctx->rx, &payload_len, &payload, &total_len);

        if (parse_ret == 0) break;
        if (parse_ret < 0) {
            lan_conn_close(ctx, tpcb, 1);
            return ERR_OK;
        }

        uint32_t dap_ret = DAP_ProcessCommand(payload, ctx->dap_response);
        uint16_t response_len = (uint16_t)(dap_ret & 0xFFFFU);

        if (lan_send_dap_response(tpcb, ctx, ctx->dap_response, response_len) != ERR_OK) {
            lan_conn_close(ctx, tpcb, 1);
            return ERR_OK;
        }

        msgbuf_consume(&ctx->rx, total_len);
    }

    return ERR_OK;
}

static err_t lan_conn_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);
    return ERR_OK;
}

static void lan_conn_err(void *arg, err_t err)
{
    struct tcp_lan_ctx *ctx = (struct tcp_lan_ctx *)arg;
    LWIP_UNUSED_ARG(err);

    if (ctx == NULL) return;

    ctx->pcb = NULL;
    msgbuf_init(&ctx->rx);

    if (ctx->role == TCP_CONN_ROLE_SERVER) {
        s_active_client = NULL;
    }
}

static void lan_conn_close(struct tcp_lan_ctx *ctx, struct tcp_pcb *tpcb, uint8_t close_pcb)
{
    if (tpcb != NULL) {
        tcp_arg(tpcb, NULL);
        tcp_sent(tpcb, NULL);
        tcp_recv(tpcb, NULL);
        tcp_err(tpcb, NULL);
        tcp_poll(tpcb, NULL, 0);

        if (close_pcb != 0U) {
            err_t cerr = tcp_close(tpcb);
            if (cerr != ERR_OK) {
                tcp_abort(tpcb);
            }
        }
    }

    if (ctx != NULL) {
        ctx->pcb = NULL;
        msgbuf_init(&ctx->rx);

        if (ctx->role == TCP_CONN_ROLE_SERVER) {
            s_active_client = NULL;
        }
    }
}

/* ======================================================================== */
/*  Remote TLS Client (outbound encrypted connection to relay server)       */
/* ======================================================================== */

static err_t tls_send_dap_response(struct tls_remote_ctx *ctx,
                                   const uint8_t *payload,
                                   uint16_t len)
{
    uint16_t total_len;
    if (len > DAP_PKT_SIZE || ctx->pcb == NULL) return ERR_VAL;

    write_le32(ctx->tx_buf + 0, DAP_PKT_HDR_SIGNATURE);
    write_le16(ctx->tx_buf + 4, len);
    ctx->tx_buf[6] = DAP_PKT_TYPE_RESPONSE;
    ctx->tx_buf[7] = 0;

    memcpy(ctx->tx_buf + sizeof(struct cmsis_dap_tcp_packet_hdr), payload, len);
    total_len = (uint16_t)(sizeof(struct cmsis_dap_tcp_packet_hdr) + len);

    err_t werr = altcp_write(ctx->pcb, ctx->tx_buf, total_len, TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) return werr;
    return altcp_output(ctx->pcb);
}

static void tls_remote_schedule_reconnect(void)
{
    if (s_remote_reconnect_pending == 0U) {
        s_remote_reconnect_pending = 1U;
        sys_timeout(REMOTE_RECONNECT_MS, tls_remote_reconnect_timer, NULL);
    }
}

static void tls_remote_reconnect_timer(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    s_remote_reconnect_pending = 0U;
    tls_remote_try_connect();
}

static void tls_remote_try_connect(void)
{
    ip_addr_t remote_ip;
    struct altcp_pcb *tls_pcb;

    if ((s_remote_connected != 0U) || (s_remote_ctx.pcb != NULL)) {
        return;
    }

    if (!ipaddr_aton(REMOTE_SERVER_IP, &remote_ip)) {
        printf("TLS: Invalid remote IP\r\n");
        tls_remote_schedule_reconnect();
        return;
    }

    /* Create TLS-wrapped connection */
    tls_pcb = altcp_tls_new(s_tls_config, IPADDR_TYPE_V4);
    if (tls_pcb == NULL) {
        printf("TLS: altcp_tls_new failed\r\n");
        tls_remote_schedule_reconnect();
        return;
    }

    /* Set up callbacks */
    msgbuf_init(&s_remote_ctx.rx);
    s_remote_ctx.pcb = tls_pcb;
    altcp_arg(tls_pcb, &s_remote_ctx);
    altcp_recv(tls_pcb, (altcp_recv_fn)tls_remote_recv);
    altcp_sent(tls_pcb, (altcp_sent_fn)tls_remote_sent);
    altcp_err(tls_pcb, (altcp_err_fn)tls_remote_err);
    altcp_poll(tls_pcb, (altcp_poll_fn)tls_remote_poll, REMOTE_TCP_POLL_INTERVAL);

    printf("TLS: Connecting to %s:%d ...\r\n", REMOTE_SERVER_IP, REMOTE_SERVER_PORT);

    err_t ret = altcp_connect(tls_pcb, &remote_ip, REMOTE_SERVER_PORT,
                              (altcp_connected_fn)tls_remote_connected_cb);
    if (ret != ERR_OK) {
        printf("TLS: altcp_connect failed: %d\r\n", ret);
        altcp_abort(tls_pcb);
        s_remote_ctx.pcb = NULL;
        tls_remote_schedule_reconnect();
    }
}

static err_t tls_remote_connected_cb(void *arg, struct altcp_pcb *conn, err_t err)
{
    struct tls_remote_ctx *ctx = (struct tls_remote_ctx *)arg;

    if ((ctx == NULL) || (err != ERR_OK)) {
        printf("TLS: Connection/handshake failed (err=%d)\r\n", err);
        if (ctx != NULL) {
            ctx->pcb = NULL;
            msgbuf_init(&ctx->rx);
        }
        tls_remote_schedule_reconnect();
        return ERR_OK;
    }

    ctx->pcb = conn;
    s_remote_connected = 1U;
    s_remote_reconnect_pending = 0U;
    sys_untimeout(tls_remote_reconnect_timer, NULL);
    printf("TLS: Connected and handshake complete!\r\n");
    return ERR_OK;
}

static err_t tls_remote_recv(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err)
{
    struct tls_remote_ctx *ctx = (struct tls_remote_ctx *)arg;

    if (err != ERR_OK || p == NULL) {
        if (p != NULL) pbuf_free(p);
        if (err == ERR_OK && p == NULL) {
            printf("TLS: Remote closed connection\r\n");
            tls_remote_close(ctx, 1);
        }
        return ERR_OK;
    }

    altcp_recved(conn, p->tot_len);

    if ((ctx == NULL) || (msgbuf_add_pbuf(&ctx->rx, p) != ERR_OK)) {
        pbuf_free(p);
        tls_remote_close(ctx, 1);
        return ERR_OK;
    }
    pbuf_free(p);

    /* Process complete DAP messages */
    while (1) {
        uint16_t payload_len, total_len;
        const uint8_t *payload;
        int parse_ret = msgbuf_parse(&ctx->rx, &payload_len, &payload, &total_len);

        if (parse_ret == 0) break;
        if (parse_ret < 0) {
            printf("TLS: Invalid packet, closing\r\n");
            tls_remote_close(ctx, 1);
            return ERR_OK;
        }

        uint32_t dap_ret = DAP_ProcessCommand(payload, ctx->dap_response);
        uint16_t response_len = (uint16_t)(dap_ret & 0xFFFFU);

        if (tls_send_dap_response(ctx, ctx->dap_response, response_len) != ERR_OK) {
            tls_remote_close(ctx, 1);
            return ERR_OK;
        }

        msgbuf_consume(&ctx->rx, total_len);
    }

    return ERR_OK;
}

static err_t tls_remote_sent(void *arg, struct altcp_pcb *conn, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(conn);
    LWIP_UNUSED_ARG(len);
    return ERR_OK;
}

static void tls_remote_err(void *arg, err_t err)
{
    struct tls_remote_ctx *ctx = (struct tls_remote_ctx *)arg;
    LWIP_UNUSED_ARG(err);

    printf("TLS: Connection error (err=%d)\r\n", err);

    if (ctx == NULL) return;

    ctx->pcb = NULL;
    msgbuf_init(&ctx->rx);
    s_remote_connected = 0;
    tls_remote_schedule_reconnect();
}

static err_t tls_remote_poll(void *arg, struct altcp_pcb *conn)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(conn);
    return ERR_OK;
}

static void tls_remote_close(struct tls_remote_ctx *ctx, uint8_t close_pcb)
{
    if (ctx != NULL && ctx->pcb != NULL) {
        altcp_arg(ctx->pcb, NULL);
        altcp_recv(ctx->pcb, NULL);
        altcp_sent(ctx->pcb, NULL);
        altcp_err(ctx->pcb, NULL);
        altcp_poll(ctx->pcb, NULL, 0);

        if (close_pcb != 0U) {
            err_t cerr = altcp_close(ctx->pcb);
            if (cerr != ERR_OK) {
                altcp_abort(ctx->pcb);
            }
        }
    }

    if (ctx != NULL) {
        ctx->pcb = NULL;
        msgbuf_init(&ctx->rx);
        s_remote_connected = 0;
        tls_remote_schedule_reconnect();
    }
}

/* ======================================================================== */
/*  Public API                                                              */
/* ======================================================================== */

void tcp_server_init(void)
{
    struct tcp_pcb *pcb;
    err_t ret;

    /* ---- Create TLS configuration (once) ---- */
    if (s_tls_config == NULL) {
        s_tls_config = altcp_tls_create_config_client(
            tls_ca_cert_pem, tls_ca_cert_pem_len);
        if (s_tls_config == NULL) {
            printf("TLS: Failed to create TLS config!\r\n");
            /* Continue anyway — LAN server still works */
        } else {
            printf("TLS: Configuration created successfully\r\n");
        }
    }

    /* ---- Start local LAN TCP server (plain, no TLS) ---- */
    pcb = tcp_new();
    if (pcb == NULL) {
        printf("TLS: tcp_new failed for LAN server\r\n");
        return;
    }

    ret = tcp_bind(pcb, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (ret != ERR_OK) {
        printf("TLS: tcp_bind failed: %d\r\n", ret);
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        printf("TLS: tcp_listen failed\r\n");
        return;
    }

    tcp_accept(pcb, lan_server_accept);
    printf("TLS: LAN TCP server listening on port %d\r\n", TCP_SERVER_PORT);

    /* ---- Start outbound TLS connection ---- */
    if (s_tls_config != NULL) {
        tls_remote_try_connect();
    }
}

#endif /* DAP_SERVER_MODE == DAP_SERVER_MODE_TLS */
