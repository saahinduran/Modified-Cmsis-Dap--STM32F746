#if 0
#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/timeouts.h"
#include <stdint.h>
#include <string.h>
#include "DAP.h"

#define TCP_SERVER_PORT 5000

#ifndef ENABLE_REMOTE_DAP_CLIENT
#define ENABLE_REMOTE_DAP_CLIENT 1
#endif

#ifndef REMOTE_SERVER_IP
#define REMOTE_SERVER_IP "206.81.20.113"
#endif

#ifndef REMOTE_SERVER_PORT
#define REMOTE_SERVER_PORT 9999
#endif

#ifndef REMOTE_RECONNECT_MS
#define REMOTE_RECONNECT_MS 10000U
#endif

#ifndef REMOTE_TCP_POLL_INTERVAL
#define REMOTE_TCP_POLL_INTERVAL 4U
#endif

#ifndef DAP_PACKET_SIZE
#define DAP_PACKET_SIZE 512U
#endif

#define DAP_PKT_SIZE            DAP_PACKET_SIZE
#define DAP_PKT_HDR_SIGNATURE   0x00504144UL
#define DAP_PKT_TYPE_REQUEST    0x01U
#define DAP_PKT_TYPE_RESPONSE   0x02U

struct cmsis_dap_tcp_packet_hdr {
    uint32_t signature;
    uint16_t length;
    uint8_t packet_type;
    uint8_t reserved;
} __attribute__((__packed__));

#define DAP_TOTAL_PKT_SIZE ((uint16_t)(sizeof(struct cmsis_dap_tcp_packet_hdr) + DAP_PKT_SIZE))
#define MSGBUF_CAPACITY    ((uint16_t)(3U * DAP_TOTAL_PKT_SIZE))

struct msgbuf_t {
    uint8_t data[MSGBUF_CAPACITY];
    uint16_t len;
};

enum {
    TCP_CONN_ROLE_SERVER = 0,
    TCP_CONN_ROLE_REMOTE = 1
};

struct tcp_conn_ctx {
    uint8_t role;
    struct msgbuf_t rx;
    uint8_t tx_buf[DAP_TOTAL_PKT_SIZE];
    uint8_t dap_response[DAP_PKT_SIZE];
    struct tcp_pcb *pcb;
};

static struct tcp_conn_ctx s_server_ctx = { .role = TCP_CONN_ROLE_SERVER };
#if ENABLE_REMOTE_DAP_CLIENT
static struct tcp_conn_ctx s_remote_ctx = { .role = TCP_CONN_ROLE_REMOTE };
static uint8_t s_remote_connected = 0;
static uint8_t s_remote_reconnect_pending = 0;
#endif
static struct tcp_pcb *s_active_client = NULL;

/* Forward declarations */
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_conn_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void  tcp_conn_err(void *arg, err_t err);
static err_t tcp_conn_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void  tcp_conn_close(struct tcp_conn_ctx *ctx, struct tcp_pcb *tpcb, uint8_t close_pcb);
#if ENABLE_REMOTE_DAP_CLIENT
static void  tcp_remote_try_connect(void);
static void  tcp_remote_schedule_reconnect(void);
static void  tcp_remote_reconnect_timer(void *arg);
static err_t tcp_remote_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static err_t tcp_remote_poll(void *arg, struct tcp_pcb *tpcb);
#endif

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

    if (buf->len < hdr_size) {
        return 0;
    }

    const uint8_t *raw = buf->data;
    uint32_t signature = read_le32(raw + 0);
    uint16_t length = read_le16(raw + 4);
    uint8_t packet_type = raw[6];

    if (signature != DAP_PKT_HDR_SIGNATURE) {
        return -1;
    }
    if (packet_type != DAP_PKT_TYPE_REQUEST) {
        return -1;
    }
    if (length > DAP_PKT_SIZE) {
        return -1;
    }

    if (buf->len < (uint16_t)(hdr_size + length)) {
        return 0;
    }

    *payload_len = length;
    *payload = raw + hdr_size;
    *total_len = (uint16_t)(hdr_size + length);
    return 1;
}

static err_t send_dap_response(struct tcp_pcb *tpcb,
                               struct tcp_conn_ctx *ctx,
                               const uint8_t *payload,
                               uint16_t len)
{
    uint16_t total_len;

    if (len > DAP_PKT_SIZE) {
        return ERR_VAL;
    }

    write_le32(ctx->tx_buf + 0, DAP_PKT_HDR_SIGNATURE);
    write_le16(ctx->tx_buf + 4, len);
    ctx->tx_buf[6] = DAP_PKT_TYPE_RESPONSE;
    ctx->tx_buf[7] = 0;

    memcpy(ctx->tx_buf + sizeof(struct cmsis_dap_tcp_packet_hdr), payload, len);
    total_len = (uint16_t)(sizeof(struct cmsis_dap_tcp_packet_hdr) + len);

    err_t werr = tcp_write(tpcb, ctx->tx_buf, total_len, TCP_WRITE_FLAG_COPY);
    if (werr != ERR_OK) {
        return werr;
    }
    return tcp_output(tpcb);
}

static void tcp_setup_conn(struct tcp_pcb *pcb, struct tcp_conn_ctx *ctx)
{
    msgbuf_init(&ctx->rx);
    ctx->pcb = pcb;
    tcp_arg(pcb, ctx);
    tcp_recv(pcb, tcp_conn_recv);
    tcp_err(pcb, tcp_conn_err);
    tcp_sent(pcb, tcp_conn_sent);
#if ENABLE_REMOTE_DAP_CLIENT
    if (ctx->role == TCP_CONN_ROLE_REMOTE) {
        tcp_poll(pcb, tcp_remote_poll, REMOTE_TCP_POLL_INTERVAL);
    } else {
        tcp_poll(pcb, NULL, 0);
    }
#else
    tcp_poll(pcb, NULL, 0);
#endif
}

void tcp_server_init(void)
{
    struct tcp_pcb *pcb;
    err_t ret;

    pcb = tcp_new();
    if (pcb == NULL) {
        //printf("tcp_server_init: tcp_new failed\n");
        return;
    }

    ret = tcp_bind(pcb, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (ret != ERR_OK) {
        //printf("tcp_server_init: tcp_bind failed: %d\n", ret);
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        //printf("tcp_server_init: tcp_listen failed\n");
        return;
    }

    tcp_accept(pcb, tcp_server_accept);

#if ENABLE_REMOTE_DAP_CLIENT
    tcp_remote_try_connect();
#endif
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    if (s_active_client != NULL) {
        tcp_close(newpcb);
        return ERR_OK;
    }

    s_active_client = newpcb;
    tcp_setup_conn(newpcb, &s_server_ctx);
    return ERR_OK;
}

static err_t tcp_conn_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct tcp_conn_ctx *ctx = (struct tcp_conn_ctx *)arg;

    if (err != ERR_OK || p == NULL) {
        if (p != NULL) pbuf_free(p);
        if (err == ERR_OK && p == NULL) {
            tcp_conn_close(ctx, tpcb, 1);
        }
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    if ((ctx == NULL) || (msgbuf_add_pbuf(&ctx->rx, p) != ERR_OK)) {
        pbuf_free(p);
        tcp_conn_close(ctx, tpcb, 1);
        return ERR_OK;
    }
    pbuf_free(p);

    while (1) {
        uint16_t payload_len;
        uint16_t total_len;
        const uint8_t *payload;
        int parse_ret = msgbuf_parse(&ctx->rx, &payload_len, &payload, &total_len);

        if (parse_ret == 0) {
            break;
        }
        if (parse_ret < 0) {
            tcp_conn_close(ctx, tpcb, 1);
            return ERR_OK;
        }

        uint32_t dap_ret = DAP_ProcessCommand(payload, ctx->dap_response);
        uint16_t response_len = (uint16_t)(dap_ret & 0xFFFFU);

        if (send_dap_response(tpcb, ctx, ctx->dap_response, response_len) != ERR_OK) {
            tcp_conn_close(ctx, tpcb, 1);
            return ERR_OK;
        }

        msgbuf_consume(&ctx->rx, total_len);
    }

    return ERR_OK;
}

static err_t tcp_conn_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);
    return ERR_OK;
}

static void tcp_conn_err(void *arg, err_t err)
{
    struct tcp_conn_ctx *ctx = (struct tcp_conn_ctx *)arg;
    LWIP_UNUSED_ARG(err);

    if (ctx == NULL) {
        return;
    }

    ctx->pcb = NULL;
    msgbuf_init(&ctx->rx);

    if (ctx->role == TCP_CONN_ROLE_SERVER) {
        s_active_client = NULL;
    }
#if ENABLE_REMOTE_DAP_CLIENT
    if (ctx->role == TCP_CONN_ROLE_REMOTE) {
        s_remote_connected = 0;
        tcp_remote_schedule_reconnect();
    }
#endif
}

static void tcp_conn_close(struct tcp_conn_ctx *ctx, struct tcp_pcb *tpcb, uint8_t close_pcb)
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

#if ENABLE_REMOTE_DAP_CLIENT
        if (ctx->role == TCP_CONN_ROLE_REMOTE) {
            s_remote_connected = 0;
            tcp_remote_schedule_reconnect();
        }
#endif
    }
}

#if ENABLE_REMOTE_DAP_CLIENT
static void tcp_remote_schedule_reconnect(void)
{
    if (s_remote_reconnect_pending == 0U) {
        s_remote_reconnect_pending = 1U;
        sys_timeout(REMOTE_RECONNECT_MS, tcp_remote_reconnect_timer, NULL);
    }
}

static void tcp_remote_reconnect_timer(void *arg)
{
    LWIP_UNUSED_ARG(arg);
    s_remote_reconnect_pending = 0U;
    tcp_remote_try_connect();
}

static void tcp_remote_try_connect(void)
{
    ip_addr_t remote_ip;
    struct tcp_pcb *pcb;
    err_t ret;

    if ((s_remote_connected != 0U) || (s_remote_ctx.pcb != NULL)) {
        return;
    }

    if (!ipaddr_aton(REMOTE_SERVER_IP, &remote_ip)) {
        tcp_remote_schedule_reconnect();
        return;
    }

    pcb = tcp_new();
    if (pcb == NULL) {
        tcp_remote_schedule_reconnect();
        return;
    }

    tcp_setup_conn(pcb, &s_remote_ctx);
    ret = tcp_connect(pcb, &remote_ip, REMOTE_SERVER_PORT, tcp_remote_connected_cb);
    if (ret != ERR_OK) {
        tcp_abort(pcb);
        s_remote_ctx.pcb = NULL;
        tcp_remote_schedule_reconnect();
    }
}

static err_t tcp_remote_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    struct tcp_conn_ctx *ctx = (struct tcp_conn_ctx *)arg;

    if ((ctx == NULL) || (err != ERR_OK)) {
        if (ctx != NULL) {
            ctx->pcb = NULL;
            msgbuf_init(&ctx->rx);
        }
        tcp_remote_schedule_reconnect();
        return ERR_OK;
    }

    ctx->pcb = tpcb;
    s_remote_connected = 1U;
    s_remote_reconnect_pending = 0U;
    sys_untimeout(tcp_remote_reconnect_timer, NULL);
    return ERR_OK;
}

static err_t tcp_remote_poll(void *arg, struct tcp_pcb *tpcb)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);
    return ERR_OK;
}
#endif
#endif
