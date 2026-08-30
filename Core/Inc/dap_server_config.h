/*
 * dap_server_config.h
 *
 * Unified configuration for CMSIS-DAP TCP Server / Client.
 */

#ifndef INC_DAP_SERVER_CONFIG_H_
#define INC_DAP_SERVER_CONFIG_H_

/* -------------------------------------------------------------------------
 * DAP Server / Client Mode Selection
 * -------------------------------------------------------------------------
 * DAP_SERVER_MODE_LAN : Listens as a TCP server on the local network (port 4441)
 * DAP_SERVER_MODE_WAN : Connects as a TCP client to a remote server (e.g. ngrok / VPS)
 */
#define DAP_SERVER_MODE_LAN     0
#define DAP_SERVER_MODE_WAN     1

#define DAP_SERVER_MODE         DAP_SERVER_MODE_LAN

/* Server listening port (LAN mode) */
#define DAP_TCP_SERVER_PORT     4441

/* Remote server configuration (WAN mode) */
#define DAP_REMOTE_SERVER_IP    "192.168.1.137"
#define DAP_REMOTE_SERVER_PORT  4441

/* DAP TCP packet buffer size */
#define DAP_TCP_PKT_SIZE        4096

/* Legacy aliases for backward compatibility */
#define REMOTE_SERVER_IP        DAP_REMOTE_SERVER_IP
#define REMOTE_SERVER_PORT      DAP_REMOTE_SERVER_PORT

#endif /* INC_DAP_SERVER_CONFIG_H_ */
