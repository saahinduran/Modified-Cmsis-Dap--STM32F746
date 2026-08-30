/*
 * tcpServerRAW.c
 *
 * CMSIS-DAP TCP Server using lwIP RAW API.
 * Handles incoming DAP packets over LAN, forwards to DAP_ProcessCommand,
 * and transmits responses back to client (e.g. OpenOCD / pyOCD).
 */

#include "dap_server_config.h"
#if (DAP_SERVER_MODE == DAP_SERVER_MODE_LAN)

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "DAP.h"

// Returns current cycle count
static inline uint32_t Get_CPU_Cycles(void) {
    return DWT->CYCCNT;
}

#define TCP_SERVER_PORT DAP_TCP_SERVER_PORT

static char __attribute__((aligned(4))) response[DAP_TCP_PKT_SIZE];

/* Per-connection state for receiving data */
struct tcp_conn_state {
    char *recv_buffer;
    size_t recv_buffer_len;
    size_t recv_buffer_size;
};

/* Allocate buffer for a new connection */
static struct tcp_conn_state* tcp_conn_state_new(void) {
    struct tcp_conn_state *state = (struct tcp_conn_state *)malloc(sizeof(struct tcp_conn_state));
    if (state == NULL) return NULL;
    
    state->recv_buffer_size = 4096;
    state->recv_buffer = (char *)malloc(state->recv_buffer_size);
    if (state->recv_buffer == NULL) {
        free(state);
        return NULL;
    }
    
    state->recv_buffer_len = 0;
    return state;
}

/* Free connection state */
static void tcp_conn_state_free(struct tcp_conn_state *state) {
    if (state == NULL) return;
    if (state->recv_buffer != NULL) {
        free(state->recv_buffer);
    }
    free(state);
}

/* Forward declarations */
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void  tcp_server_err(void *arg, err_t err);
static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static void  tcp_server_close_conn(struct tcp_pcb *tpcb, void *conn);


void tcp_server_init(void)
{
    struct tcp_pcb *pcb;
    err_t ret;

    pcb = tcp_new();
    if (pcb == NULL) {
        printf("tcp_server_init: tcp_new failed\n");
        return;
    }

    ret = tcp_bind(pcb, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (ret != ERR_OK) {
        printf("tcp_server_init: tcp_bind failed: %d\n", ret);
        tcp_close(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        printf("tcp_server_init: tcp_listen failed\n");
        return;
    }

    tcp_accept(pcb, tcp_server_accept);
    printf("TCP server listening on port %d\n", TCP_SERVER_PORT);
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    struct tcp_conn_state *state = tcp_conn_state_new();
    if (state == NULL) {
        // Out of memory - reject connection
        return ERR_MEM;
    }

    tcp_arg(newpcb, state);  // Store connection state as context
    tcp_recv(newpcb, tcp_server_recv);
    tcp_err(newpcb, tcp_server_err);
    tcp_sent(newpcb, tcp_server_sent);

    printf("New client connected\n");
    return ERR_OK;
}

struct cmsis_dap_tcp_packet_hdr {
    uint32_t signature;         // "DAP"
    uint16_t length;            // Not including header length.
    uint8_t packet_type;
    uint8_t reserved;           // Reserved for future use.
}cmsis_dap_tcp_pck __attribute__((__packed__));

// DAP_PKT_SIZE must be >= to what is used by the client (OpenOCD).
#define DAP_PKT_SIZE            DAP_TCP_PKT_SIZE
#define DAP_PKT_HDR_SIGNATURE   0x00504144   // "DAP\0" in LE
#define DAP_PKT_TYPE_REQUEST    0x01
#define DAP_PKT_TYPE_RESPONSE   0x02

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct tcp_conn_state *state = (struct tcp_conn_state *)arg;

    if (err != ERR_OK || p == NULL) {
        if (p != NULL) pbuf_free(p);
        if (err == ERR_OK && p == NULL) {
            printf("Client closed connection\n");
            tcp_server_close_conn(tpcb, state);
        }
        return ERR_OK;
    }

    if (state == NULL) {
        pbuf_free(p);
        return ERR_OK;
    }

    /* Inform lwIP that data has been received */
    tcp_recved(tpcb, p->tot_len);

    /* Append new data to receive buffer */
    size_t new_data_len = p->tot_len;
    if (state->recv_buffer_len + new_data_len > state->recv_buffer_size) {
        // Truncate new data to fit in buffer
        new_data_len = state->recv_buffer_size - state->recv_buffer_len;
    }
    pbuf_copy_partial(p, state->recv_buffer + state->recv_buffer_len, new_data_len, 0);
    state->recv_buffer_len += new_data_len;

    pbuf_free(p);

    /* Process all complete messages in the buffer */
    size_t offset = 0;
    while (offset + sizeof(struct cmsis_dap_tcp_packet_hdr) <= state->recv_buffer_len) {
        struct cmsis_dap_tcp_packet_hdr *hdr = (struct cmsis_dap_tcp_packet_hdr *)(state->recv_buffer + offset);
        
        /* Check if this is a valid DAP packet */
        if (hdr->signature != DAP_PKT_HDR_SIGNATURE) {
            printf("Invalid signature, discarding buffer\n");
            state->recv_buffer_len = 0;
            break;
        }

        uint16_t msg_length = hdr->length;
        uint32_t total_msg_len = sizeof(struct cmsis_dap_tcp_packet_hdr) + msg_length;

        /* Safeguard against invalid length values */
        if (total_msg_len > state->recv_buffer_size) {
            printf("Message too large, discarding\n");
            state->recv_buffer_len = 0;
            break;
        }

        /* Check if we have the complete message */
        if (offset + total_msg_len > state->recv_buffer_len) {
            // Incomplete message - wait for more data
            break;
        }

        uint32_t firstTime = Get_CPU_Cycles();
        /* We have a complete message - process it */
        uint32_t num = DAP_ProcessCommand((uint8_t *)(state->recv_buffer + offset + 8), (uint8_t *)response + 8);
        uint32_t writeLen = (num & 0xFFFF) + 8;

        uint32_t elapsedTime = Get_CPU_Cycles() - firstTime;
        //printf("Elapsed time for write len :%d is %d tick\r\n", writeLen, elapsedTime);

        struct cmsis_dap_tcp_packet_hdr response_hdr;
        response_hdr.signature = DAP_PKT_HDR_SIGNATURE;
        response_hdr.length = writeLen - 8;
        response_hdr.packet_type = DAP_PKT_TYPE_RESPONSE;
        response_hdr.reserved = 0;

        memcpy(response, &response_hdr, sizeof(response_hdr));

        /* Send response */
        err_t werr = tcp_write(tpcb, response, writeLen, TCP_WRITE_FLAG_COPY);

        // Get available bytes in the send buffer
        u16_t space_available = tcp_sndbuf(tpcb);

        // Get the current number of enqueued segments
        u16_t queue_len = tcp_sndqueuelen(tpcb);

        //printf("TCP Debug: [Buffer Space: %u bytes] [Queue Len: %u]\n", space_available, queue_len);

        if (space_available == 0) {
            printf("Warning: Send buffer is completely full!\n");
        }

        if (queue_len >= TCP_SND_QUEUELEN) {
            printf("Warning: Maximum queue length (%d) reached!\n", TCP_SND_QUEUELEN);
        }


        if (werr == ERR_OK) {
            tcp_output(tpcb);
        } else
        {
            printf("tcp_write failed: %d\n", werr);
        }

        offset += total_msg_len;
    }

    /* Shift remaining incomplete data to the beginning of the buffer */
    if (offset > 0 && offset < state->recv_buffer_len) {
        memmove(state->recv_buffer, state->recv_buffer + offset, state->recv_buffer_len - offset);
        state->recv_buffer_len -= offset;
    } else if (offset > 0) {
        state->recv_buffer_len = 0;
    }

    return ERR_OK;
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);
    return ERR_OK;
}

static void tcp_server_err(void *arg, err_t err)
{
    LWIP_UNUSED_ARG(err);
    struct tcp_conn_state *state = (struct tcp_conn_state *)arg;
    tcp_conn_state_free(state);
    printf("Connection aborted or reset\n");
}

static void tcp_server_close_conn(struct tcp_pcb *tpcb, void *conn)
{
    struct tcp_conn_state *state = (struct tcp_conn_state *)conn;
    
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_close(tpcb);
    
    tcp_conn_state_free(state);
}

/* Example usage:
   - Call tcp_server_init() after MX_LWIP_Init() completes.
   - The server listens on port 5000.
   - When a client sends data, process_data() transforms it (example: uppercase) and returns it. */
#endif
