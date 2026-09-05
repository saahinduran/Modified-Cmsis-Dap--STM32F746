# CMSIS-DAP TCP Server for STM32F7

Ethernet-based CMSIS-DAP debug firmware for STM32F746. It exposes a TCP debug endpoint for OpenOCD/pyOCD and can run in local LAN mode or over a remote relay/TLS path.

## Branches

This repository has two main branches:

- `gpio`: standard CMSIS-DAP using GPIO bit-banging
- `spi`: faster SPI-based implementation for custom high-throughput JTAG transport

```bash
git checkout gpio
git checkout spi
```

## Deployment modes

The firmware is configured in [`Core/Inc/dap_server_config.h`](Core/Inc/dap_server_config.h):

```c
#define DAP_SERVER_MODE       DAP_SERVER_MODE_TLS
#define DAP_TCP_SERVER_PORT    5000
#define DAP_TCP_PKT_SIZE       4096U

#define REMOTE_SERVER_IP       "192.168.1.55"
#define REMOTE_SERVER_PORT     4442
#define REMOTE_RECONNECT_MS    10000U
```

Available modes:

- `DAP_SERVER_MODE_LAN`: local TCP server only
- `DAP_SERVER_MODE_WAN`: outbound connection to a remote relay/VPS
- `DAP_SERVER_MODE_TLS`: LAN server + secure outbound TLS link

## Forwarding architecture

The remote path is a relay-based tunnel, not a JTAG parser in the relay:

```text
OpenOCD / openFPGALoader
        |
        | plain TCP
        v
Windows host proxy (or any relay host)
        |
        | TLS-encrypted TCP stream
        v
Relay server (can be local or on a remote VPS)
        |
        | same encrypted stream, forwarded byte-for-byte
        v
STM32F746 gateway
        |
        | decrypt + parse CMSIS-DAP payload
        v
JTAG target
```

The relay only forwards TCP bytes. It does not terminate TLS and does not inspect CMSIS-DAP packets. The STM32 gateway decrypts the stream, validates the DAP packet framing, executes the command, and sends the encrypted response back.

This works whether the relay is running on a Windows machine or on a remote VPS.

## Configuration example

For a remote TLS setup:

- local tool connects to the host proxy on `127.0.0.1:6666` (or another configured port)
- the proxy connects to the relay host on a public/private relay port
- the STM32 connects outbound to the relay on `REMOTE_SERVER_PORT`
- both sides use the same relay socket pair, with the relay forwarding bytes transparently

## Usage

1. Open the project in STM32CubeIDE.
2. Select the desired branch (`gpio` or `spi`).
3. Set the mode and remote address in [`Core/Inc/dap_server_config.h`](Core/Inc/dap_server_config.h).
4. Build and flash the STM32 board.
5. Start the host proxy/relay and connect OpenOCD or pyOCD to the configured TCP port.

## Notes

- `gpio` branch: standard CMSIS-DAP compatible with upstream OpenOCD and pyOCD
- `spi` branch: optimized custom transport for higher JTAG throughput
- Network IP and Ethernet settings are defined in the lwIP configuration and STM32CubeMX project files

## License

This project is licensed under the [Apache-2.0 License](LICENSE).
