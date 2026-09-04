/**
 * @file    dap_server_config.h
 * @brief   CMSIS-DAP TCP Server configuration.
 *
 * Change DAP_SERVER_MODE to switch between LAN-only, WAN, and TLS-backed
 * remote operation. All network-level tunables live here so that no other
 * source file needs to be edited when changing the deployment mode.
 */

#ifndef DAP_SERVER_CONFIG_H
#define DAP_SERVER_CONFIG_H

/* ---- Mode selectors ---------------------------------------------------- */
#define DAP_SERVER_MODE_LAN   0   /**< Direct LAN TCP server only.          */
#define DAP_SERVER_MODE_WAN   1   /**< LAN server + outbound remote client. */
#define DAP_SERVER_MODE_TLS   2   /**< LAN server + outbound TLS client.    */

/* ========================================================================
 *  >>> CHANGE THIS LINE TO SWITCH BETWEEN LAN, WAN AND TLS <<<
 * ======================================================================== */
#define DAP_SERVER_MODE       DAP_SERVER_MODE_LAN

#define DAP_SERVER_IS_REMOTE() \
	((DAP_SERVER_MODE == DAP_SERVER_MODE_WAN) || \
	 (DAP_SERVER_MODE == DAP_SERVER_MODE_TLS))

/* ---- Common settings --------------------------------------------------- */
#ifndef DAP_TCP_SERVER_PORT
#define DAP_TCP_SERVER_PORT   5000
#endif

#ifndef DAP_TCP_PKT_SIZE
#define DAP_TCP_PKT_SIZE      4096U
#endif

/* ---- Remote-mode settings (ignored in LAN mode) ------------------------ */
#if DAP_SERVER_IS_REMOTE()

#ifndef REMOTE_SERVER_IP
#define REMOTE_SERVER_IP          "192.168.1.55"
#endif

#ifndef REMOTE_SERVER_PORT
#define REMOTE_SERVER_PORT        9999
#endif

#ifndef REMOTE_RECONNECT_MS
#define REMOTE_RECONNECT_MS       10000U
#endif

#ifndef REMOTE_TCP_POLL_INTERVAL
#define REMOTE_TCP_POLL_INTERVAL  4U
#endif

#endif /* DAP_SERVER_IS_REMOTE() */

#endif /* DAP_SERVER_CONFIG_H */
