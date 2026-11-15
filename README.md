**libwsc: C++11 WebSocket Client and Server Library**

[![Autobahn TestSuite](https://img.shields.io/badge/Autobahn-passing-brightgreen)](https://amigniter.github.io/libwsc/autobahn/)
[![Build libwsc](https://github.com/amigniter/libwsc/actions/workflows/ci.yml/badge.svg)](https://github.com/amigniter/libwsc/actions/workflows/ci.yml)

A lightweight, high-performance WebSocket client and server library written in C++11.

Originally written for [mod_audio_stream](https://github.com/amigniter/mod_audio_stream) to provide low-latency audio streaming from FreeSWITCH to a WebSocket endpoint.

Built on libevent; only zlib (for deflate) and OpenSSL (for TLS) are optional extras.

**libwsc** is a standalone asynchronous WebSocket library. Its client implementation has been validated against the [Autobahn Testsuite](https://github.com/crossbario/autobahn-testsuite).

It passes the Autobahn WebSocket Testsuite (including permessage-deflate) with performance comparable to or exceeding common C/C++ clients. You can find the test results [here](https://amigniter.github.io/libwsc/autobahn/index.html). The _examples/autobahn.cpp_ example can be used to run the Autobahn Testsuite locally and reproduce the results.

## Features

* RFC 6455–compliant handshake, framing, masking, and control frames
* Per-message deflate compression
* Event-driven, asynchronous I/O using libevent (`bufferevent`)
* Optional TLS/SSL support via OpenSSL (handled internally)
* UTF-8 validation per WebSocket specification
* **Fully thread-safe design** (concurrent connect / disconnect / send / receive)
* Clear separation between public API and internal implementation
* CMake-based build for portability
* **Very small footprint**: ~110 KB stripped shared library on x86_64 Linux (Release)

### Client improvements in v1.1.0

* Completely redesigned internal architecture based on an event-driven model
* Thread-safe client API with internal serialization and state management
* Clear separation between public API and internal implementation
* Robust connection lifecycle handling (connect, disconnect, shutdown)
* TLS support fully encapsulated inside the library
* **Threading model**
  * All libevent / bufferevent operations are executed on a single internal event thread.
  * Public API methods are safe to call from any thread; they enqueue work to the event loop.
  * User callbacks are invoked on the event thread.
  * The context object is managed via `std::shared_ptr` and keeps itself alive while async operations are in flight.

### WebSocket server (new in v1.2.0)

* Optional server build (`LIBWSC_BUILD_SERVER=ON`); the client-only build remains the default.
* Plain WS and TLS-enabled WSS listeners (`USE_TLS=ON` for TLS support).
* Bearer token authentication during the HTTP upgrade, before a client is accepted.
* Connection, message and disconnection callbacks; text/binary `send()` and `broadcast()`.
* Automatic PING/PONG heartbeat (30-second interval / 10-second timeout).
* Explicit `stop()` for shutdown (call outside server callbacks).

## Requirements

* C++11 compiler  (e.g., g++ or clang++)
* [libevent](https://libevent.org/) (v2.0+)
* Zlib (for compression)
* OpenSSL (for TLS) - optional for plaintext builds
* CMake (>=3.10)
* Runtime Libs: libevent, zlib, and OpenSSL libraries are typically present by default on most Linux distributions.

## Further reading

- [BUILDING.md](docs/BUILDING.md) — build flags, prerequisites, install and CMAKE integration
- [OPTIONS.md](docs/OPTIONS.md) — Client configuration options
- [SERVER.md](docs/SERVER.md) — Server setup, TLS certificates, authentication and usage

## Client example

You can find working demos in the [`examples/`](examples/) folder:

- **threads.cpp** — shows multi-threaded send/receive (thread-safety demo)
- **autobahn.cpp** — running tests against autobahntestsuite

```cpp
#include <iostream>
#include <string>
#include "WebSocketClient.h"

int main() {

    WebSocketClient client;

    // Configure endpoint ws:// or wss:// if using TLS
    client.setUrl("ws://localhost:3001");

    // compression (permessage-deflate) is enabled by default if supported by server and negotiated
    // you can disable it (do not offer in negotiation)
    // client.enableCompression(false);

    client.setOpenCallback([]() {
        std::cout << "[Open] Connected\n";
    });

    client.setCloseCallback([](int code, const std::string &reason) {
        std::cout << "[Close] Code=" << code
                  << " Reason=\"" << reason << "\"\n";
    });

    client.setMessageCallback([](const std::string &msg) {
        std::cout << "[Text] " << msg << "\n";
    });

    client.setBinaryCallback([](const void *data, size_t len) {
        std::cout << "[Binary] Received " << len << " bytes\n";
        //process data
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        // ...
    });

    client.setErrorCallback([&client](int code, const std::string &msg) {
        std::cerr << "[Error] Code=" << code
                  << " Message=\"" << msg << "\"\n";
        client.disconnect();
    });

    client.connect();
    // Now the event loop is running in the background.
    // Keep your main thread alive

    // Any calls to client.sendMessage() made before the connection is fully open 
    // will be queued internally and automatically flushed once the socket is ready
    client.sendMessage("HELLO");

    // Interactive loop
    std::string line;
    std::cout << "> " << std::flush;
    while (std::getline(std::cin, line)) {
        client.sendMessage(line);
        std::cout << "> " << std::flush;
    }

    // Clean up
    client.disconnect();
    return 0;
}

```

---

## WebSocket server

Since v1.2.0, libwsc includes an optional asynchronous WebSocket server supporting WS/WSS, Bearer authentication, automatic PING/PONG heartbeat, and text/binary messaging.

Enable the server with `LIBWSC_BUILD_SERVER=ON` and TLS support with `USE_TLS=ON`. The server requires an application-provided authentication callback; libwsc extracts the Bearer token but does not decide how it is validated.

See [Server documentation](docs/SERVER.md) for build instructions, TLS certificates, authentication, API usage and manual compilation of the [server example](examples/websocket_server.cpp). The example is not built automatically.

---

## License

MIT License © 2025–2026 AMSOFTSWITCH LTD.