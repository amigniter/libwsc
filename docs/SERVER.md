# WebSocket Server

`libwsc` v1.2.0 provides an optional C++11 WebSocket server built on libevent. It supports plain WebSocket (WS), TLS-protected WebSocket (WSS), Bearer authentication during the HTTP upgrade, text/binary messaging and automatic PING/PONG heartbeat (30-second interval, 10-second timeout).

The client library remains available independently. Server support is **disabled by default**.

## Build

Install the dependencies described in [BUILDING.md](BUILDING.md), then configure:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLIBWSC_BUILD_SERVER=ON -DUSE_TLS=ON
cmake --build build -j$(nproc)
```

`LIBWSC_BUILD_SERVER=ON` adds `WebSocketServer.cpp` to the `libwsc` library. `USE_TLS=ON` enables TLS for **both client and server**, linking OpenSSL and the libevent OpenSSL component. To build a plain WS-only server, set `USE_TLS=OFF`. Calling `listenTLS()` without TLS support returns `false`.

The `examples/websocket_server.cpp` file is provided as source only and is **not built by CMake**.

## TLS certificates (WSS)

For **local development only**, create a self-signed certificate and private key:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.crt -days 30 \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
chmod 600 server.key
```

Use the resulting paths with `listenTLS(address, port, certFile, keyFile)`. A self-signed certificate is not trusted by clients automatically; use a certificate issued by a trusted CA (with the correct hostname) for production. Protect the private key and never commit certificates' private keys or production credentials to Git. The `-nodes` option creates an unencrypted key for unattended local testing; protect production keys appropriately.

## Bearer authentication

An authentication callback **must be registered before** `listen()` or `listenTLS()`. Without one, the server refuses to start. During the WebSocket HTTP upgrade, the server reads:

```http
Authorization: Bearer <token>
```

It passes the extracted token to `setAuthCallback()`. Missing or invalid tokens receive HTTP **401 Unauthorized**; accepted tokens allow the upgrade to proceed.

**Token generation, secure storage, validation, revocation and expiration are entirely application responsibilities.** `libwsc` does not implement SHA-256 verification, JWT validation or any other token policy. The callback below uses a literal token **only for illustration**; real applications should use a secure verification mechanism and avoid hardcoded secrets.

```cpp
server.setAuthCallback([](const std::string& token) {
    return token == "local-demo-token"; // demo only
});
```

Use **WSS** whenever Bearer tokens cross untrusted networks. WS sends headers without transport encryption.

## Server API

```cpp
#include "WebSocketServer.h"

WebSocketServer server;

server.setAuthCallback([](const std::string& token) {
    return token == "local-demo-token"; // demo only
});

server.onConnect([&server](WebSocketServer::ConnectionId id) {
    server.send(id, "Connected");
});

server.onMessage([&server](WebSocketServer::ConnectionId id,
                           const std::string& message, bool binary) {
    server.send(id, message, binary); // echo text or binary
});

server.onDisconnect([](WebSocketServer::ConnectionId id) {
    (void)id;
});

if (!server.listen("127.0.0.1", 8081)) {
    // handle startup failure
}

// Keep the application alive while the server runs.
// Later, from outside callbacks: server.stop();
```

Use `listenTLS("127.0.0.1", 8443, "server.crt", "server.key")` instead of `listen()` for WSS. Both return `false` on startup failure. `isRunning()` reports server state.

The library supplies a `ConnectionId` to callbacks. It identifies the **current connection**, not a persistent user; it may change after reconnecting. Pass it to `send(id, payload, binary)` to reply to that connection. `send()` returns `false` if the connection is missing, the server is stopped or the queue is full. `broadcast(payload, binary)` sends to all connected clients. The default `binary` value is `false` for both methods.

Call `stop()` **outside server callbacks**. The server handles heartbeat PING/PONG automatically.

## Standalone example

See [`examples/websocket_server.cpp`](../examples/websocket_server.cpp). The example accepts `ws` or `wss`, a port, and (for WSS) certificate and key paths. It reads its demonstration Bearer token from `LIBWSC_EXAMPLE_TOKEN`.

With a **static** default build, you can compile it manually from the repository root (assuming the required development packages and `pkg-config` are installed):

```bash
g++ -std=c++11 -I./src examples/websocket_server.cpp \
  ./build/libwsc.a -o websocket_server \
  $(pkg-config --cflags --libs libevent_openssl libevent_pthreads openssl zlib) \
  -pthread
```

The command above assumes `USE_TLS=ON` and a static library at `build/libwsc.a`; adjust the library path and linkage for other build configurations.

```bash
export LIBWSC_EXAMPLE_TOKEN='replace-with-a-test-token'
./websocket_server ws 8081
./websocket_server wss 8443 server.crt server.key
```

Clients must send the same token in the HTTP `Authorization: Bearer ...` header during the handshake. The example listens on loopback for local testing; use appropriate binding, firewall and TLS configuration when deploying a real service.
