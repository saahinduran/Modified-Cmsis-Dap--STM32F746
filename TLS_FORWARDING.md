# Remote JTAG TLS Forwarding

This document describes how the two Windows applications and the STM32F746
gateway work together to transport CMSIS-DAP traffic over a TLS-protected
network connection.

The two Windows programs are:

- `tls_forward.cpp`: the host-side TLS proxy.
- `tls_relay_windows.cpp`: the VPS relay.

The embedded side is implemented in this project by `Core/Src/tls_client.c`.
Although the source comments refer to an ESP32 in some places, the same role
is performed here by the STM32F746 gateway.

## 1. Complete topology

```text
OpenOCD / openFPGALoader
        |
        | Plain TCP, local only
        | 127.0.0.1:6666
        v
Windows host TLS proxy (`tls_forward.cpp`)
        |
        | TLS records inside a TCP connection
        | Host-side relay port, for example 4441
        v
VPS relay (`tls_relay_windows.cpp`)
        |
        | The exact same encrypted TLS bytes
        | Gateway-side relay port, for example 4442
        v
STM32F746 gateway (`Core/Src/tls_client.c`)
        |
        | CMSIS-DAP request/response processing
        v
JTAG target
```

The VPS is not a TLS endpoint. It does not have the server certificate, does
not decrypt the connection, and does not understand CMSIS-DAP. It only joins
one host-side TCP connection to one gateway-side TCP connection and copies
bytes in both directions.

## 2. TLS and TCP roles

The TLS roles are intentionally asymmetric:

| Component | TCP role | TLS role | Data handled |
|---|---|---|---|
| Windows host proxy | Connects to relay; listens locally | TLS server | Local plaintext and decrypted TLS application data |
| VPS relay | Listens on two ports | No TLS role | Opaque TCP bytes |
| STM32F746 gateway | Connects to relay | TLS client | TLS application data and CMSIS-DAP frames |

This means the gateway initiates the outbound connection, which is useful
when the gateway is behind a home router or firewall. The gateway does not
need an inbound public port.

The Windows proxy loads `server.crt` and `server.key`, creates an OpenSSL
`TLS_server_method()` context, and requires TLS 1.2 or newer. The STM32 uses
the CA certificate embedded in `Core/Inc/tls_certs.h` to authenticate the
Windows proxy certificate during the mbedTLS handshake.

## 3. Startup and connection establishment

### 3.1 VPS relay

The relay is started with two different listening ports:

```text
tls_relay_windows.exe <host_port> <gateway_port>
```

For example:

```text
tls_relay_windows.exe 4441 4442
```

It creates two IPv4 listeners on `0.0.0.0`:

- `4441` accepts the Windows host proxy.
- `4442` accepts the STM32 gateway.

`AcceptPair()` waits until both listeners have an accepted connection. The
connections are paired only by arrival order: the next host connection is
paired with the next gateway connection. The relay does not authenticate,
identify, or otherwise match clients.

### 3.2 Windows host proxy

The host proxy creates a local plaintext listener, using these defaults:

```text
127.0.0.1:6666
```

It then repeatedly performs the following sequence:

1. Connect to the VPS host-side relay address and port.
2. Attach the connected socket to an OpenSSL `SSL` object.
3. Immediately call `SSL_accept()`.
4. Wait for the STM32 ClientHello through the opaque relay.
5. After the TLS handshake completes, wait for OpenOCD or
   `openFPGALoader` to connect locally.
6. Bridge local plaintext traffic to TLS and TLS application data back to the
   local tool.

The handshake is completed before waiting for the local tool. This ordering
is important: the STM32 connects and starts its TLS handshake independently
of OpenOCD. Waiting for a local client first could leave the gateway waiting
until its handshake timeout expires.

### 3.3 STM32F746 gateway

During `tcp_server_init()` in `Core/Src/tls_client.c`, the firmware:

1. Creates an mbedTLS/ALTCP client configuration using the embedded CA.
2. Starts a plain LAN TCP server on `DAP_TCP_SERVER_PORT` (default `5000`).
3. Starts an outbound TLS connection to `REMOTE_SERVER_IP:REMOTE_SERVER_PORT`.
4. Registers receive, error, sent, and polling callbacks.
5. Reconnects after `REMOTE_RECONNECT_MS` (default `10000` ms) when the
   remote TLS connection fails.

The remote address and port are compile-time settings in
`Core/Inc/dap_server_config.h`. They must point to the VPS gateway-side
listener, not to the host-side listener.

## 4. What happens to one CMSIS-DAP request

Assume that OpenOCD sends a CMSIS-DAP request to the local host proxy.

### Request direction

1. OpenOCD writes a plaintext CMSIS-DAP TCP frame to `127.0.0.1:6666`.
2. `BridgeTraffic()` reads the bytes from the local socket.
3. `SendAllTls()` passes the bytes to `SSL_write()`.
4. OpenSSL encrypts the application data and sends TLS records to the VPS.
5. The VPS `RelayBidirectionally()` loop reads those records from the host
   socket without inspecting them.
6. The VPS sends the records unchanged to the gateway socket.
7. lwIP/ALTCP and mbedTLS decrypt the records in the STM32.
8. `tls_remote_recv()` accumulates the decrypted bytes in `msgbuf_t`.
9. `msgbuf_parse()` validates the CMSIS-DAP header and waits for a complete
   frame if TCP fragmentation split it across callbacks.
10. `DAP_ProcessCommand()` executes the command against the JTAG target.

### Response direction

1. The STM32 constructs a response frame with the `DAP` signature and
   response packet type.
2. `tls_send_dap_response()` sends the response through ALTCP and mbedTLS.
3. The encrypted TLS records travel through the VPS unchanged.
4. The Windows proxy receives and decrypts them with `SSL_read()`.
5. `SendAllPlain()` writes the resulting plaintext response to OpenOCD.

The relay therefore never sees a CMSIS-DAP header. Its counters measure TLS
record bytes, not necessarily the exact CMSIS-DAP payload size.

## 5. Framing and buffering

TCP is a byte stream; one `send()` does not necessarily correspond to one
`recv()`. TLS adds another record layer, but it does not restore application
message boundaries.

The Windows proxy consequently treats both directions as streams:

- Local-to-TLS data is read in chunks up to 16 KiB and passed to
  `SSL_write()` until all bytes are accepted.
- TLS-to-local data is read with `SSL_read()` and written with a loop that
  handles partial `send()` results.
- `SSL_pending()` is checked so already-decrypted data buffered inside
  OpenSSL is consumed without waiting for another socket-read event.

The embedded gateway performs the protocol framing. Its `msgbuf_t` buffer
accumulates incoming data, validates the 8-byte CMSIS-DAP TCP header, waits
for the declared payload length, and processes all complete messages. This
handles both fragmented TCP delivery and multiple CMSIS-DAP frames delivered
in one callback.

## 6. Relay forwarding loop

The VPS relay uses `WSAPoll()` on the two connected sockets:

```text
host socket readable    -> recv(host)    -> send(gateway)
gateway socket readable -> recv(gateway) -> send(host)
```

`ForwardOnce()` copies the received bytes without modification. `SendAll()`
loops until the complete received chunk has been sent, so a short TCP send
does not silently discard data. The relay uses a 64 KiB forwarding buffer and
reports byte counters for both directions at the end of a session.

The accepted sockets enable:

- `SO_KEEPALIVE`, to help detect dead peers eventually.
- `TCP_NODELAY`, to reduce additional delay from Nagle coalescing.

When either endpoint closes or reports a fatal poll event, the relay ends the
session, closes both accepted sockets, and waits for a new host/gateway pair.

## 7. Disconnect and reconnect behavior

### Windows proxy

The proxy resets the current session when one of these occurs:

- The local tool disconnects.
- The STM32 closes the TLS connection.
- A TLS read/write or socket operation fails.

It closes the local socket, performs best-effort `SSL_shutdown()`, closes the
relay socket, waits one second, and reconnects to the relay. The local
listener remains available throughout the process.

### STM32 gateway

The gateway clears its remote receive buffer and schedules a reconnect after
remote errors or a clean remote close. The LAN server remains a separate
plain-TCP service. In TLS mode, its remote TLS connection is the path used by
the host proxy in this architecture.

### VPS relay

The relay does not reconnect either endpoint itself. It terminates the current
pair and returns to its accept loop. The host proxy and the STM32 are
responsible for establishing the next pair.

## 8. Port and configuration checklist

The following values must agree across the deployment:

| Setting | Location | Meaning |
|---|---|---|
| Host relay port | Windows proxy argument `--relay-port` | VPS listener for `tls_forward.cpp` |
| Gateway relay port | STM32 `REMOTE_SERVER_PORT` | VPS listener for STM32 |
| Relay host IP | Windows proxy `--relay-ip` | Public VPS address |
| Relay gateway IP | STM32 `REMOTE_SERVER_IP` | Public VPS address |
| Local tool port | Windows proxy `--local-port` | Port used by OpenOCD/openFPGALoader |
| Local LAN port | STM32 `DAP_TCP_SERVER_PORT` | Separate embedded plain-TCP service |

Example mapping:

```text
VPS host listener:     0.0.0.0:4441
VPS gateway listener:  0.0.0.0:4442
Windows proxy:         --relay-port 4441 --local-port 6666
STM32 firmware:        REMOTE_SERVER_PORT 4442
OpenOCD target:        127.0.0.1:6666
```

The relay ports must be reachable through the VPS firewall. The host proxy
must be started from a directory containing `server.crt` and `server.key`,
unless those file names are changed in the source.

## 9. Security properties and limitations

The design provides confidentiality and integrity for the traffic between the
Windows proxy and the STM32, assuming certificate verification and private-key
protection are configured correctly. The VPS cannot read the TLS application
data because it only forwards encrypted records.

The relay itself is still an unauthenticated TCP rendezvous point. It accepts
connections on both public ports and pairs them by arrival order. TLS
authentication protects the endpoint that owns the certificate, but the relay
does not independently prevent an unwanted client from connecting to either
listener or causing an incorrect pair. Firewall rules, access control, or an
additional client-authentication design would be needed for a hostile public
environment.

The Windows proxy also accepts the local plaintext connection on the
configured local address. Binding to `127.0.0.1` keeps that unencrypted
interface local to the Windows machine; binding it to another interface would
expose plaintext CMSIS-DAP traffic to that network.

## 10. Summary

The forwarding chain has two distinct responsibilities:

1. The Windows proxy terminates TLS and bridges a local plaintext CMSIS-DAP
   stream to the encrypted connection.
2. The VPS relay provides only a blind, bidirectional TCP path.
3. The STM32 initiates TLS, verifies the peer certificate, decrypts the
   application data, parses CMSIS-DAP frames, executes JTAG operations, and
   encrypts the responses.

This separation allows the embedded gateway to make an outbound connection
through NAT while keeping the VPS unaware of both the TLS contents and the
JTAG protocol.