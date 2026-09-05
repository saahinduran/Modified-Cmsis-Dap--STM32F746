# CMSIS-DAP TCP Server for STM32F7

An Ethernet-based **CMSIS-DAP** debug probe firmware for **STM32F746** (e.g., NUCLEO-F746ZG), providing SWD and JTAG debugging over TCP/IP using the lwIP raw API.

---

## 🌿 Branches & Implementation Differences

This repository contains two distinct implementations on separate branches:

| Branch | Signal Generation | Protocol Compatibility | Description & Best Use Case |
| :--- | :--- | :--- | :--- |
| **`gpio`** | **GPIO Bit-Banging** | **Standard CMSIS-DAP** | Standard CMSIS-DAP v1/v2 compatible out-of-the-box with upstream **OpenOCD**, **pyOCD**, and Keil MDK. Universal pin mapping and straightforward setup. |
| **`spi`** | **Hardware SPI Accelerated** | **Customized Protocol** | Custom high-speed JTAG protocol leveraging STM32 hardware SPI peripherals (SPI3/SPI4 FIFOs) for accelerated clocking and high data throughput. Requires compatible customized client/fork. |

To switch branches:
```bash
git checkout gpio  # For Standard CMSIS-DAP (GPIO bit-banging)
git checkout spi   # For High-Speed SPI Accelerated version
```

---

## 🚀 Key Features

- **Networked Debugging**: Direct TCP server implementation using lightweight lwIP raw API with zero-copy packet processing and tuned TCP MSS (1024 bytes).
- **Deployment Modes** (configured in [`Core/Inc/dap_server_config.h`](Core/Inc/dap_server_config.h)):
  - **LAN Mode (`DAP_SERVER_MODE_LAN`)**: local TCP endpoint on the network.
  - **WAN Mode (`DAP_SERVER_MODE_WAN`)**: outbound connection to a remote relay/VPS.
  - **TLS Mode (`DAP_SERVER_MODE_TLS`)**: local LAN server plus encrypted relay connection.
- **Remote Relay Support**: works with a Windows relay or a relay hosted on a remote VPS.
- **Diagnostics & Profiling**: real-time connection status, TCP queue diagnostics, and DWT cycle execution profiling over USART3.

---

## ⚙️ Configuration

All network and server configurations are centralized in [`Core/Inc/dap_server_config.h`](Core/Inc/dap_server_config.h):

```c
/* Mode Selection: DAP_SERVER_MODE_LAN, DAP_SERVER_MODE_WAN or DAP_SERVER_MODE_TLS */
#define DAP_SERVER_MODE         DAP_SERVER_MODE_TLS

/* Server listening port (LAN mode) */
#define DAP_TCP_SERVER_PORT     5000

/* Packet buffer size */
#define DAP_TCP_PKT_SIZE        4096U

/* Remote server configuration (WAN/TLS mode only) */
#if (DAP_SERVER_MODE == DAP_SERVER_MODE_WAN) || (DAP_SERVER_MODE == DAP_SERVER_MODE_TLS)
#define REMOTE_SERVER_IP        "192.168.1.137"
#define REMOTE_SERVER_PORT      4441
#define REMOTE_RECONNECT_MS     10000U
#endif
```

## 🔐 Remote TLS / Relay Path

The remote path uses a relay between the host tool and the STM32 gateway:

```text
OpenOCD / pyOCD / openFPGALoader -> Host proxy -> Relay/VPS -> STM32 gateway -> JTAG target
```

The relay forwards TCP bytes without terminating TLS or parsing CMSIS-DAP packets. The STM32 gateway decrypts the stream, validates the DAP frame, executes the command, and sends the encrypted response back. The same design works with a Windows relay or a relay running on a remote VPS.

---

### Static IP / Network Settings

Network IP configuration is defined in `LWIP/App/lwip.c` (and STM32CubeMX `.ioc`):
- **Default IP (LAN)**: `192.168.1.114`
- **Netmask**: `255.255.255.0`
- **Gateway**: `192.168.1.1`

---

## 📌 Pinout & Hardware Connections

### Target Debug Interface

| Signal | Function | GPIO (`gpio` branch) | SPI (`spi` branch) |
| :--- | :--- | :--- | :--- |
| **TCK / SWCLK** | Clock | `GPIOC Pin 10` | `SPI4_SCK (PE2)` / `PC10` |
| **TMS / SWDIO** | Mode Select / Data I/O | `GPIOC Pin 11` | `SPI4_MOSI (PE6)` / `PC11` |
| **TDI** | JTAG Data In | `GPIOC Pin 12` | `SPI3_MOSI (PC12)` |
| **TDO** | JTAG Data Out | `GPIOC Pin 2` | `SPI3_MISO (PC11)` / `PC2` |
| **nTRST** *(Optional)* | JTAG Test Reset | `GPIOC Pin 8` | `GPIOC Pin 8` |
| **nRESET / SRST** *(Optional)* | Target System Reset | `GPIOD Pin 2` | `GPIOD Pin 2` |
| **GND** | Ground | `GND` | `GND` |

### Serial Debug Output (Virtual COM Port)

- **USART3 TX**: `PD8` (Connected to ST-LINK Virtual COM Port)
- **USART3 RX**: `PD9`
- **Baud Rate**: `115200 8-N-1`

---

## 🛠️ Usage with OpenOCD (Standard `gpio` branch)

Create an `openocd_tcp.cfg` configuration file on your host machine:

```tcl
# Interface configuration for CMSIS-DAP over TCP
adapter driver cmsis-dap
cmsis-dap backend tcp
cmsis-dap tcp_server 192.168.1.114
cmsis-dap tcp_port 5000

# Select transport protocol (swd or jtag)
transport select swd

# Target configuration (example for STM32F4)
source [find target/stm32f4x.cfg]
```

Run OpenOCD:
```bash
openocd -f openocd_tcp.cfg
```

Connect with GDB:
```bash
arm-none-eabi-gdb your_firmware.elf
(gdb) target extended-remote :3333
(gdb) monitor reset halt
(gdb) load
(gdb) continue
```

---

## 🔨 Building and Flashing

1. **Clone the repository**:
   ```bash
   git clone https://github.com/saahinduran/CMSIS-DAP-TCP.git
   cd CMSIS-DAP-TCP
   git checkout gpio  # or: git checkout spi
   ```
2. **Open in STM32CubeIDE**:
   - File -> Open Projects from File System... -> Select repository directory.
3. **Build**:
   - Select **Release** or **Debug** configuration (recommended `-O3` optimization level for highest transfer performance).
4. **Flash**:
   - Connect the STM32F746 board via the onboard ST-LINK USB port and flash the target.
5. **Network Connection**:
   - Connect an RJ45 Ethernet cable to your local network switch/router.
   - Monitor the USART3 serial console (115200 baud) for IP status and connection logs.

---

## 📄 License

This project is licensed under the [Apache-2.0 License](LICENSE) with upstream components licensed under standard BSD / MIT licenses (lwIP and STM32 HAL).
