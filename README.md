# CMSIS-DAP TCP Server for STM32F7

An Ethernet-based **CMSIS-DAP** debug probe firmware for **STM32F746** (e.g., NUCLEO-F746ZG), providing high-speed SWD and JTAG debugging over TCP/IP using the lwIP raw API.

Compatible with **OpenOCD**, **pyOCD**, **Keil MDK**, and other debuggers supporting CMSIS-DAP over TCP.

---

## 🌿 Branches

This repository maintains two specialized branches depending on your hardware signal generation requirements:

| Branch | Signal Generation | Key Advantage | Best Use Case |
| :--- | :--- | :--- | :--- |
| **`spi`** | **Hardware SPI Accelerated** | Maximum clock speeds and data throughput | Production debugging, large firmware flashing |
| **`gpio`** | **GPIO Bit-Banging** | Universal pin compatibility, simple hardware setup | Flexible prototyping, porting to other MCUs |

To switch branches:
```bash
git checkout spi   # For SPI accelerated implementation
git checkout gpio  # For GPIO bit-banging implementation
```

---

## 🚀 Key Features

- **CMSIS-DAP v1 / v2 Protocol**: Full support for standard JTAG and Serial Wire Debug (SWD) commands.
- **High-Performance TCP Stack**: Built on lightweight lwIP raw API with zero-copy packet processing and tuned TCP MSS (1024 bytes).
- **Dual Deployment Modes** (via [`Core/Inc/dap_server_config.h`](Core/Inc/dap_server_config.h)):
  - **LAN Mode (`DAP_SERVER_MODE_LAN`)**: Listens as a TCP server on the local network.
  - **WAN Mode (`DAP_SERVER_MODE_WAN`)**: Automatically initiates an outbound connection to a remote relay/VPS server—enabling remote debugging from anywhere through NAT/firewalls.
- **UART Diagnostics & Profiling**: Real-time connection status, TCP queue diagnostics, and DWT execution cycle profiling output over USART3 (ST-LINK Virtual COM Port at 115200 baud).

---

## ⚙️ Configuration

All network and server configurations are centralized in [`Core/Inc/dap_server_config.h`](Core/Inc/dap_server_config.h).

```c
/* Mode Selection: DAP_SERVER_MODE_LAN or DAP_SERVER_MODE_WAN */
#define DAP_SERVER_MODE         DAP_SERVER_MODE_LAN

/* Server listening port (LAN mode) */
#define DAP_TCP_SERVER_PORT     5000

/* Packet buffer size */
#define DAP_TCP_PKT_SIZE        4096U

/* Remote server configuration (WAN mode only) */
#if (DAP_SERVER_MODE == DAP_SERVER_MODE_WAN)
#define REMOTE_SERVER_IP        "192.168.1.137"
#define REMOTE_SERVER_PORT      4441
#define REMOTE_RECONNECT_MS     10000U
#endif
```

### Static IP / DHCP Settings

Network IP configuration can be adjusted in the STM32CubeMX `.ioc` file or in `LWIP/App/lwip.c`:
- **Default IP (LAN)**: `192.168.1.110` (or `192.168.1.114`)
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

## 🛠️ Usage with OpenOCD

Create an `openocd_tcp.cfg` file on your host PC:

```tcl
# Interface configuration for CMSIS-DAP over TCP
adapter driver cmsis-dap
cmsis-dap backend tcp
cmsis-dap tcp_server 192.168.1.110
cmsis-dap tcp_port 5000

# Select transport (swd or jtag)
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
   git checkout spi   # or: git checkout gpio
   ```
2. **Open in STM32CubeIDE**:
   - File -> Open Projects from File System... -> Select repository folder.
3. **Build**:
   - Select **Release** or **Debug** configuration (recommended `-O3` optimization for high debug speeds).
4. **Flash**:
   - Connect your STM32F746 board via onboard ST-LINK USB cable.
   - Run / Debug the firmware.
5. **Connect Ethernet**:
   - Plug the RJ45 cable from the board to your local switch/router.
   - Monitor the serial console (115200 baud) for IP and connection status messages.

---

## 📄 License

This project is released under the [Apache-2.0 License](LICENSE) (CMSIS-DAP components) and standard BSD / MIT licenses for lwIP and HAL components.
