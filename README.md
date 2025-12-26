**libwsc: C++11 WebSocket Client Library**

[![Autobahn TestSuite](https://img.shields.io/badge/Autobahn-passing-brightgreen)](https://amigniter.github.io/libwsc/autobahn/)
[![Build libwsc](https://github.com/amigniter/libwsc/actions/workflows/ci.yml/badge.svg)](https://github.com/amigniter/libwsc/actions/workflows/ci.yml)

A lightweight, high-performance and fully-compliant WebSocket client written in C++11.

Written solely for [mod_audio_stream](https://github.com/amigniter/mod_audio_stream) to provide low-latency audio streaming from FreeSWITCH to a WebSocket endpoint.

Built on libevent; only zlib (for deflate) and OpenSSL (for TLS) are optional extras.

**libwsc** is now offered as a standalone, fully RFC-6455 and [autobahn-testsuite](https://github.com/crossbario/autobahn-testsuite) compliant asynchronous client library. You can find test results [here](https://amigniter.github.io/libwsc/autobahn/index.html) (except test 2.1, we support one ping per-flight)

---

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

### What’s new in v1.1.0

* Completely redesigned internal architecture based on an event-driven model
* Thread-safe client API with internal serialization and state management
* Clear separation between public API and internal implementation
* Robust connection lifecycle handling (connect, disconnect, shutdown)
* TLS support fully encapsulated inside the library

---

## Requirements

* C++11 compiler  (e.g., g++ or clang++)
* [libevent](https://libevent.org/) (v2.0+)
* Zlib (for compression)
* OpenSSL (for TLS) - optional for plaintext builds
* CMake (>=3.10)
* Runtime Libs: libevent, zlib, and OpenSSL libraries are typically present by default on most Linux distributions.

## Further reading

- [BUILDING.md](docs/BUILDING.md) — build flags, prerequisites, install and CMAKE integration
- [OPTIONS.md](docs/OPTIONS.md) — Additional options

## Example

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

## License

MIT License © 2025 AMSOFTSWITCH LTD.