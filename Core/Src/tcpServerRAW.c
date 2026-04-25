#if 1
/*
 * stm32_lwip_tcp_server.c
 *
 * TCP server example for STM32 using lwIP raw API.
 * - Listens on port 5000.
 * - Receives data, performs user-defined processing, and sends back response.
 *
 * Author: ChatGPT (modified version)
 */

#include "lwip/tcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "DAP.h"

#define TCP_SERVER_PORT 5000

static char __attribute__((aligned(4))) input[4096];

static char __attribute__((aligned(4))) response[4096];

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

/* User-defined function to process received data */
static void process_data(const char *input, char *output, size_t out_len)
{
    /* Example: convert input to uppercase */
    size_t len = strlen(input);
    for (size_t i = 0; i < len && i < out_len - 1; i++) {
        output[i] = (char)toupper((unsigned char)input[i]);
    }
    output[len < out_len - 1 ? len : out_len - 1] = '\0';
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
    //printf("TCP server listening on port %d\n", TCP_SERVER_PORT);
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

    //printf("New client connected\n");
    return ERR_OK;
}

struct cmsis_dap_tcp_packet_hdr {
    uint32_t signature;         // "DAP"
    uint16_t length;            // Not including header length.
    uint8_t packet_type;
    uint8_t reserved;           // Reserved for future use.
}cmsis_dap_tcp_pck __attribute__((__packed__));

// DAP_PKT_SIZE must be >= to what is used by the client (OpenOCD).
#define DAP_PKT_SIZE            CONFIG_ESP_DAP_TCP_MAX_PKT_SIZE
#define DAP_PKT_HDR_SIGNATURE   0x00504144   // "DAP\0" in LE
#define DAP_PKT_TYPE_REQUEST    0x01
#define DAP_PKT_TYPE_RESPONSE   0x02

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct tcp_conn_state *state = (struct tcp_conn_state *)arg;

    if (err != ERR_OK || p == NULL) {
        if (p != NULL) pbuf_free(p);
        if (err == ERR_OK && p == NULL) {
            //printf("Client closed connection\n");
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
            //printf("Invalid signature, discarding buffer\n");
            state->recv_buffer_len = 0;
            break;
        }

        uint16_t msg_length = hdr->length;
        uint32_t total_msg_len = sizeof(struct cmsis_dap_tcp_packet_hdr) + msg_length;

        /* Safeguard against invalid length values */
        if (total_msg_len > state->recv_buffer_size) {
            //printf("Message too large, discarding\n");
            state->recv_buffer_len = 0;
            break;
        }

        /* Check if we have the complete message */
        if (offset + total_msg_len > state->recv_buffer_len) {
            // Incomplete message - wait for more data
            break;
        }

        /* We have a complete message - process it */
        uint32_t num = DAP_ProcessCommand((uint8_t *)(state->recv_buffer + offset + 8), (uint8_t *)response + 8);
        uint32_t writeLen = (num & 0xFFFF) + 8;

        struct cmsis_dap_tcp_packet_hdr response_hdr;
        response_hdr.signature = DAP_PKT_HDR_SIGNATURE;
        response_hdr.length = writeLen - 8;
        response_hdr.packet_type = DAP_PKT_TYPE_RESPONSE;
        response_hdr.reserved = 0;

        memcpy(response, &response_hdr, sizeof(response_hdr));

        /* Send response */
        static int tryCnt = 0;
        if(response[8] == 0 && tryCnt > 20) {
            //while(1);
        }
        tryCnt++;
        err_t werr = tcp_write(tpcb, response, writeLen, TCP_WRITE_FLAG_COPY);
        if (werr == ERR_OK) {
            tcp_output(tpcb);
        } else {
            //printf("tcp_write failed: %d\n", werr);
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
    //printf("Connection aborted or reset\n");
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
