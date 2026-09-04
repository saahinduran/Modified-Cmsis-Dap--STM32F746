/**
 * @file    tls_client.h
 * @brief   TLS-encrypted outbound client for CMSIS-DAP remote mode.
 *
 * Active only when DAP_SERVER_MODE == DAP_SERVER_MODE_TLS.
 * Provides tcp_server_init() which starts a local LAN TCP server
 * AND an outbound TLS-encrypted connection to the remote relay.
 */

#ifndef TLS_CLIENT_H
#define TLS_CLIENT_H

#include "dap_server_config.h"

#if (DAP_SERVER_MODE == DAP_SERVER_MODE_TLS)

/**
 * Initialise the TLS client.
 *
 * - Starts a plain TCP server on DAP_TCP_SERVER_PORT for local LAN access.
 * - Creates a TLS-encrypted outbound connection to REMOTE_SERVER_IP:REMOTE_SERVER_PORT.
 * - On disconnection the TLS link reconnects automatically after REMOTE_RECONNECT_MS.
 *
 * Call once after MX_LWIP_Init().
 */
void tcp_server_init(void);

#endif /* DAP_SERVER_MODE_TLS */

#endif /* TLS_CLIENT_H */
