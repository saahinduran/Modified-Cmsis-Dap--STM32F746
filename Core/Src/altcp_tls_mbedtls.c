/**
 * @file    altcp_tls_mbedtls.c
 * @brief   lwIP ALTCP TLS layer implementation using mbedTLS.
 *
 * This file implements the bridge between lwIP's ALTCP abstraction layer
 * and the mbedTLS library, enabling transparent TLS encryption over TCP.
 *
 * Based on the lwIP contrib altcp_tls_mbedtls port, adapted for
 * bare-metal (NO_SYS=1) STM32 usage.
 *
 * Only compiled when DAP_SERVER_MODE == DAP_SERVER_MODE_TLS.
 */

#include "dap_server_config.h"
#if (DAP_SERVER_MODE == DAP_SERVER_MODE_TLS)

#include "lwip/opt.h"

#if LWIP_ALTCP && LWIP_ALTCP_TLS

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/altcp_tcp.h"
#include "lwip/priv/altcp_priv.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"

#include <string.h>

/* ---- Internal state structures ----------------------------------------- */

/** TLS configuration (shared across connections using the same CA cert). */
struct altcp_tls_config {
    mbedtls_ssl_config        ssl_conf;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  ctr_drbg;
    mbedtls_x509_crt          ca_cert;
};

/** Per-connection TLS state. */
typedef struct altcp_tls_state_s {
    struct altcp_tls_config  *conf;
    mbedtls_ssl_context       ssl;
    /* Flags */
    uint8_t                   handshake_done;
    uint8_t                   rx_pending;       /* Data buffered in mbedTLS */
    /* Receive buffer: pbuf chain from TCP layer, consumed by mbedTLS */
    struct pbuf              *rx_buf;
    uint16_t                  rx_buf_offset;
    /* Bytes written to TCP but not yet ACKed */
    uint16_t                  tx_unacked;
} altcp_tls_state_t;

/* Forward declarations for the altcp_functions vtable */
static void  altcp_mbedtls_set_poll(struct altcp_pcb *conn, u8_t interval);
static void  altcp_mbedtls_recved(struct altcp_pcb *conn, u16_t len);
static err_t altcp_mbedtls_connect(struct altcp_pcb *conn, const ip_addr_t *ipaddr,
                                   u16_t port, altcp_connected_fn connected);
static err_t altcp_mbedtls_close(struct altcp_pcb *conn);
static void  altcp_mbedtls_abort(struct altcp_pcb *conn);
static err_t altcp_mbedtls_write(struct altcp_pcb *conn, const void *dataptr,
                                 u16_t len, u8_t apiflags);
static err_t altcp_mbedtls_output(struct altcp_pcb *conn);
static void  altcp_mbedtls_dealloc(struct altcp_pcb *conn);

/* ---- mbedTLS BIO callbacks (glue to lwIP TCP) -------------------------- */

/**
 * mbedTLS "send" BIO callback — writes ciphertext to the inner TCP pcb.
 */
static int altcp_mbedtls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    struct altcp_pcb *conn = (struct altcp_pcb *)ctx;
    struct altcp_pcb *inner = conn->inner_conn;
    err_t err;

    if (inner == NULL) {
        return MBEDTLS_ERR_SSL_CONN_EOF;
    }

    /* Clamp to available send buffer */
    u16_t avail = altcp_sndbuf(inner);
    if (avail == 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (len > avail) {
        len = avail;
    }

    err = altcp_write(inner, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        if (err == ERR_MEM) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    altcp_output(inner);
    return (int)len;
}

/**
 * mbedTLS "recv" BIO callback — reads ciphertext from buffered pbufs.
 */
static int altcp_mbedtls_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    struct altcp_pcb *conn = (struct altcp_pcb *)ctx;
    altcp_tls_state_t *state = (altcp_tls_state_t *)conn->state;

    if (state == NULL || state->rx_buf == NULL) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    /* Copy from pbuf chain */
    u16_t copy_len = (u16_t)((len > state->rx_buf->tot_len - state->rx_buf_offset)
                             ? (state->rx_buf->tot_len - state->rx_buf_offset)
                             : len);
    if (copy_len == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    pbuf_copy_partial(state->rx_buf, buf, copy_len, state->rx_buf_offset);
    state->rx_buf_offset += copy_len;

    /* Free consumed pbufs */
    if (state->rx_buf_offset >= state->rx_buf->tot_len) {
        /* Acknowledge received data to TCP */
        if (conn->inner_conn) {
            altcp_recved(conn->inner_conn, state->rx_buf->tot_len);
        }
        pbuf_free(state->rx_buf);
        state->rx_buf = NULL;
        state->rx_buf_offset = 0;
    }

    return (int)copy_len;
}

/* ---- Inner connection callbacks ---------------------------------------- */

/**
 * Called when TCP connection to the server is established.
 * Start the TLS handshake.
 */
static err_t altcp_mbedtls_inner_connected(void *arg, struct altcp_pcb *inner_conn, err_t err)
{
    struct altcp_pcb *conn = (struct altcp_pcb *)arg;
    altcp_tls_state_t *state;

    LWIP_UNUSED_ARG(inner_conn);

    if (conn == NULL || conn->state == NULL) {
        return ERR_VAL;
    }
    if (err != ERR_OK) {
        /* TCP connection failed */
        if (conn->connected) {
            return conn->connected(conn->arg, conn, err);
        }
        return ERR_OK;
    }

    state = (altcp_tls_state_t *)conn->state;

    /* Kick the TLS handshake */
    int ret = mbedtls_ssl_handshake(&state->ssl);
    if (ret == 0) {
        /* Handshake completed immediately (unlikely but possible) */
        state->handshake_done = 1;
        if (conn->connected) {
            return conn->connected(conn->arg, conn, ERR_OK);
        }
    } else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
               ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        /* Fatal handshake error */
        if (conn->connected) {
            return conn->connected(conn->arg, conn, ERR_CLSD);
        }
        return ERR_ABRT;
    }
    /* Otherwise WANT_READ/WANT_WRITE — handshake will continue when data arrives */

    return ERR_OK;
}

/**
 * Called when ciphertext data arrives from the TCP layer.
 */
static err_t altcp_mbedtls_inner_recv(void *arg, struct altcp_pcb *inner_conn, struct pbuf *p, err_t err)
{
    struct altcp_pcb *conn = (struct altcp_pcb *)arg;
    altcp_tls_state_t *state;

    LWIP_UNUSED_ARG(inner_conn);

    if (conn == NULL || conn->state == NULL) {
        if (p) pbuf_free(p);
        return ERR_VAL;
    }

    state = (altcp_tls_state_t *)conn->state;

    if (err != ERR_OK || p == NULL) {
        /* Connection closed or error */
        if (p) pbuf_free(p);
        if (conn->recv) {
            return conn->recv(conn->arg, conn, NULL, err);
        }
        return ERR_OK;
    }

    /* Append to RX buffer */
    if (state->rx_buf == NULL) {
        state->rx_buf = p;
        state->rx_buf_offset = 0;
    } else {
        pbuf_cat(state->rx_buf, p);
    }

    if (!state->handshake_done) {
        /* Continue handshake */
        int ret = mbedtls_ssl_handshake(&state->ssl);
        if (ret == 0) {
            state->handshake_done = 1;
            if (conn->connected) {
                conn->connected(conn->arg, conn, ERR_OK);
            }
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                   ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* Fatal error */
            if (conn->err) {
                conn->err(conn->arg, ERR_CLSD);
            }
            return ERR_ABRT;
        }
        return ERR_OK;
    }

    /* Handshake done — decrypt and pass plaintext to application */
    if (conn->recv) {
        unsigned char dec_buf[1024];
        int ret;

        do {
            ret = mbedtls_ssl_read(&state->ssl, dec_buf, sizeof(dec_buf));
            if (ret > 0) {
                struct pbuf *plain = pbuf_alloc(PBUF_RAW, (u16_t)ret, PBUF_RAM);
                if (plain) {
                    memcpy(plain->payload, dec_buf, (size_t)ret);
                    err_t app_err = conn->recv(conn->arg, conn, plain, ERR_OK);
                    if (app_err != ERR_OK) {
                        return app_err;
                    }
                }
            }
        } while (ret > 0);

        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != 0 &&
            ret != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            /* Fatal read error */
            if (conn->recv) {
                conn->recv(conn->arg, conn, NULL, ERR_CLSD);
            }
            return ERR_ABRT;
        }
    }

    return ERR_OK;
}

/**
 * Called when TCP data has been ACKed by the remote side.
 */
static err_t altcp_mbedtls_inner_sent(void *arg, struct altcp_pcb *inner_conn, u16_t len)
{
    struct altcp_pcb *conn = (struct altcp_pcb *)arg;

    LWIP_UNUSED_ARG(inner_conn);

    if (conn && conn->sent) {
        /* We pass the acked length through — this is the ciphertext length,
         * not exactly the plaintext length, but it signals progress. */
        return conn->sent(conn->arg, conn, len);
    }
    return ERR_OK;
}

/**
 * Called on TCP error.
 */
static void altcp_mbedtls_inner_err(void *arg, err_t err)
{
    struct altcp_pcb *conn = (struct altcp_pcb *)arg;

    if (conn) {
        if (conn->state) {
            altcp_tls_state_t *state = (altcp_tls_state_t *)conn->state;
            mbedtls_ssl_free(&state->ssl);
            if (state->rx_buf) {
                pbuf_free(state->rx_buf);
                state->rx_buf = NULL;
            }
            mem_free(state);
            conn->state = NULL;
        }
        conn->inner_conn = NULL;
        if (conn->err) {
            conn->err(conn->arg, err);
        }
        altcp_free(conn);
    }
}

/**
 * TCP poll callback.
 */
static err_t altcp_mbedtls_inner_poll(void *arg, struct altcp_pcb *inner_conn)
{
    struct altcp_pcb *conn = (struct altcp_pcb *)arg;
    LWIP_UNUSED_ARG(inner_conn);

    if (conn && conn->poll) {
        return conn->poll(conn->arg, conn);
    }
    return ERR_OK;
}

/* ---- ALTCP functions vtable implementation ----------------------------- */

static void altcp_mbedtls_set_poll(struct altcp_pcb *conn, u8_t interval)
{
    if (conn && conn->inner_conn) {
        altcp_poll(conn->inner_conn, altcp_mbedtls_inner_poll, interval);
    }
}

static void altcp_mbedtls_recved(struct altcp_pcb *conn, u16_t len)
{
    /* Application consumed plaintext — we don't directly tell TCP here
     * because TCP already got recved() when we consumed ciphertext.
     * This is a no-op for our simple implementation. */
    LWIP_UNUSED_ARG(conn);
    LWIP_UNUSED_ARG(len);
}

static err_t altcp_mbedtls_connect(struct altcp_pcb *conn, const ip_addr_t *ipaddr,
                                   u16_t port, altcp_connected_fn connected)
{
    if (conn == NULL || conn->inner_conn == NULL) {
        return ERR_VAL;
    }
    conn->connected = connected;
    /* Connect inner TCP, our callback will start TLS handshake */
    return altcp_connect(conn->inner_conn, ipaddr, port, altcp_mbedtls_inner_connected);
}

static err_t altcp_mbedtls_write(struct altcp_pcb *conn, const void *dataptr,
                                 u16_t len, u8_t apiflags)
{
    altcp_tls_state_t *state;
    LWIP_UNUSED_ARG(apiflags);

    if (conn == NULL || conn->state == NULL) {
        return ERR_VAL;
    }
    state = (altcp_tls_state_t *)conn->state;

    if (!state->handshake_done) {
        return ERR_VAL;
    }

    int ret = mbedtls_ssl_write(&state->ssl, (const unsigned char *)dataptr, len);
    if (ret > 0) {
        return ERR_OK;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return ERR_MEM;
    }
    return ERR_VAL;
}

static err_t altcp_mbedtls_output(struct altcp_pcb *conn)
{
    if (conn && conn->inner_conn) {
        return altcp_output(conn->inner_conn);
    }
    return ERR_VAL;
}

static err_t altcp_mbedtls_close(struct altcp_pcb *conn)
{
    altcp_tls_state_t *state;

    if (conn == NULL) {
        return ERR_VAL;
    }

    state = (altcp_tls_state_t *)conn->state;
    if (state) {
        /* Send TLS close_notify */
        if (state->handshake_done) {
            mbedtls_ssl_close_notify(&state->ssl);
        }
        mbedtls_ssl_free(&state->ssl);
        if (state->rx_buf) {
            pbuf_free(state->rx_buf);
            state->rx_buf = NULL;
        }
        mem_free(state);
        conn->state = NULL;
    }

    if (conn->inner_conn) {
        err_t err = altcp_close(conn->inner_conn);
        if (err != ERR_OK) {
            altcp_abort(conn->inner_conn);
        }
        conn->inner_conn = NULL;
    }

    altcp_free(conn);
    return ERR_OK;
}

static void altcp_mbedtls_abort(struct altcp_pcb *conn)
{
    altcp_tls_state_t *state;

    if (conn == NULL) return;

    state = (altcp_tls_state_t *)conn->state;
    if (state) {
        mbedtls_ssl_free(&state->ssl);
        if (state->rx_buf) {
            pbuf_free(state->rx_buf);
            state->rx_buf = NULL;
        }
        mem_free(state);
        conn->state = NULL;
    }

    if (conn->inner_conn) {
        altcp_abort(conn->inner_conn);
        conn->inner_conn = NULL;
    }
}

static void altcp_mbedtls_dealloc(struct altcp_pcb *conn)
{
    altcp_tls_state_t *state;

    if (conn == NULL) return;

    state = (altcp_tls_state_t *)conn->state;
    if (state) {
        mbedtls_ssl_free(&state->ssl);
        if (state->rx_buf) {
            pbuf_free(state->rx_buf);
        }
        mem_free(state);
        conn->state = NULL;
    }
}

/* ---- The altcp functions vtable ---------------------------------------- */

static const struct altcp_functions altcp_mbedtls_functions = {
    altcp_mbedtls_set_poll,
    altcp_mbedtls_recved,
    altcp_default_bind,
    altcp_mbedtls_connect,
    NULL, /* listen — not implemented (client only) */
    altcp_mbedtls_abort,
    altcp_mbedtls_close,
    altcp_default_shutdown,
    altcp_mbedtls_write,
    altcp_mbedtls_output,
    altcp_default_mss,
    altcp_default_sndbuf,
    altcp_default_sndqueuelen,
    altcp_default_nagle_disable,
    altcp_default_nagle_enable,
    altcp_default_nagle_disabled,
    altcp_default_setprio,
    altcp_mbedtls_dealloc,
    altcp_default_get_tcp_addrinfo,
    altcp_default_get_ip,
    altcp_default_get_port
#ifdef LWIP_DEBUG
    , altcp_default_dbg_get_tcp_state
#endif
};

/* ---- Public API (altcp_tls.h) ------------------------------------------ */

struct altcp_tls_config *
altcp_tls_create_config_client(const u8_t *cert, size_t cert_len)
{
    int ret;
    struct altcp_tls_config *conf;

    conf = (struct altcp_tls_config *)mem_calloc(1, sizeof(struct altcp_tls_config));
    if (conf == NULL) {
        return NULL;
    }

    mbedtls_ssl_config_init(&conf->ssl_conf);
    mbedtls_entropy_init(&conf->entropy);
    mbedtls_ctr_drbg_init(&conf->ctr_drbg);
    mbedtls_x509_crt_init(&conf->ca_cert);

    /* Seed the PRNG */
    ret = mbedtls_ctr_drbg_seed(&conf->ctr_drbg, mbedtls_entropy_func,
                                 &conf->entropy, NULL, 0);
    if (ret != 0) {
        goto fail;
    }

    /* Set up SSL config as TLS client */
    ret = mbedtls_ssl_config_defaults(&conf->ssl_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        goto fail;
    }

    mbedtls_ssl_conf_rng(&conf->ssl_conf, mbedtls_ctr_drbg_random, &conf->ctr_drbg);

    /* Parse and set CA certificate */
    if (cert == NULL || cert_len == 0) {
        goto fail;
    }

    ret = mbedtls_x509_crt_parse(&conf->ca_cert, cert, cert_len);
    if (ret != 0) {
        goto fail;
    }

    mbedtls_ssl_conf_ca_chain(&conf->ssl_conf, &conf->ca_cert, NULL);
    mbedtls_ssl_conf_authmode(&conf->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);

    return conf;

fail:
    mbedtls_x509_crt_free(&conf->ca_cert);
    mbedtls_ctr_drbg_free(&conf->ctr_drbg);
    mbedtls_entropy_free(&conf->entropy);
    mbedtls_ssl_config_free(&conf->ssl_conf);
    mem_free(conf);
    return NULL;
}

struct altcp_tls_config *
altcp_tls_create_config_server_privkey_cert(const u8_t *privkey, size_t privkey_len,
                                            const u8_t *privkey_pass, size_t privkey_pass_len,
                                            const u8_t *cert, size_t cert_len)
{
    /* Server mode not supported in this firmware */
    LWIP_UNUSED_ARG(privkey);
    LWIP_UNUSED_ARG(privkey_len);
    LWIP_UNUSED_ARG(privkey_pass);
    LWIP_UNUSED_ARG(privkey_pass_len);
    LWIP_UNUSED_ARG(cert);
    LWIP_UNUSED_ARG(cert_len);
    return NULL;
}

struct altcp_tls_config *
altcp_tls_create_config_client_2wayauth(const u8_t *ca, size_t ca_len,
                                        const u8_t *privkey, size_t privkey_len,
                                        const u8_t *privkey_pass, size_t privkey_pass_len,
                                        const u8_t *cert, size_t cert_len)
{
    /* Mutual TLS — not implemented yet. Stub for future use. */
    LWIP_UNUSED_ARG(ca);
    LWIP_UNUSED_ARG(ca_len);
    LWIP_UNUSED_ARG(privkey);
    LWIP_UNUSED_ARG(privkey_len);
    LWIP_UNUSED_ARG(privkey_pass);
    LWIP_UNUSED_ARG(privkey_pass_len);
    LWIP_UNUSED_ARG(cert);
    LWIP_UNUSED_ARG(cert_len);
    return NULL;
}

void altcp_tls_free_config(struct altcp_tls_config *conf)
{
    if (conf) {
        mbedtls_x509_crt_free(&conf->ca_cert);
        mbedtls_ctr_drbg_free(&conf->ctr_drbg);
        mbedtls_entropy_free(&conf->entropy);
        mbedtls_ssl_config_free(&conf->ssl_conf);
        mem_free(conf);
    }
}

struct altcp_pcb *
altcp_tls_new(struct altcp_tls_config *config, u8_t ip_type)
{
    struct altcp_pcb *inner;
    struct altcp_pcb *conn;
    altcp_tls_state_t *state;
    int ret;

    if (config == NULL) {
        return NULL;
    }

    /* Create the inner TCP connection */
    inner = altcp_tcp_new_ip_type(ip_type);
    if (inner == NULL) {
        return NULL;
    }

    /* Create the outer ALTCP pcb */
    conn = altcp_alloc();
    if (conn == NULL) {
        altcp_close(inner);
        return NULL;
    }

    /* Allocate TLS state */
    state = (altcp_tls_state_t *)mem_calloc(1, sizeof(altcp_tls_state_t));
    if (state == NULL) {
        altcp_close(inner);
        altcp_free(conn);
        return NULL;
    }

    state->conf = config;
    mbedtls_ssl_init(&state->ssl);

    /* Set up the SSL context */
    ret = mbedtls_ssl_setup(&state->ssl, &config->ssl_conf);
    if (ret != 0) {
        mbedtls_ssl_free(&state->ssl);
        mem_free(state);
        altcp_close(inner);
        altcp_free(conn);
        return NULL;
    }

    /* Set BIO callbacks */
    mbedtls_ssl_set_bio(&state->ssl, conn,
                        altcp_mbedtls_bio_send,
                        altcp_mbedtls_bio_recv,
                        NULL);

    /* Wire up the ALTCP pcb */
    conn->state = state;
    conn->fns = &altcp_mbedtls_functions;
    conn->inner_conn = inner;

    /* Set callbacks on inner connection to route data through TLS */
    altcp_arg(inner, conn);
    altcp_recv(inner, altcp_mbedtls_inner_recv);
    altcp_sent(inner, altcp_mbedtls_inner_sent);
    altcp_err(inner, altcp_mbedtls_inner_err);

    return conn;
}

struct altcp_pcb *
altcp_tls_alloc(void *arg, u8_t ip_type)
{
    return altcp_tls_new((struct altcp_tls_config *)arg, ip_type);
}

struct altcp_pcb *
altcp_tls_wrap(struct altcp_tls_config *config, struct altcp_pcb *inner_pcb)
{
    /* Not needed for client mode — stub */
    LWIP_UNUSED_ARG(config);
    LWIP_UNUSED_ARG(inner_pcb);
    return NULL;
}

void *altcp_tls_context(struct altcp_pcb *conn)
{
    if (conn && conn->state) {
        altcp_tls_state_t *state = (altcp_tls_state_t *)conn->state;
        return &state->ssl;
    }
    return NULL;
}

#endif /* LWIP_ALTCP && LWIP_ALTCP_TLS */
#endif /* DAP_SERVER_MODE_TLS */
