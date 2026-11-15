// Standalone libwsc WS/WSS server example (not built by libwsc CMake).
// Set LIBWSC_EXAMPLE_TOKEN to a nonempty test token before running.
// Usage: ./websocket_server ws 8081
//        ./websocket_server wss 8443 server.crt server.key
#include "WebSocketServer.h"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {
volatile std::sig_atomic_t shutdownRequested = 0;
extern "C" void handleSignal(int) { shutdownRequested = 1; }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " ws <port> | wss <port> <cert.pem> <key.pem>\n";
        return 2;
    }
    const std::string mode(argv[1]);
    if ((mode == "ws" && argc != 3) || (mode == "wss" && argc != 5) ||
        (mode != "ws" && mode != "wss")) return 2;

    char* end = nullptr;
    const long port = std::strtol(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0' || port < 1 || port > 65535) {
        std::cerr << "Invalid port\n";
        return 2;
    }
    const char* tokenEnv = std::getenv("LIBWSC_EXAMPLE_TOKEN");
    if (!tokenEnv || !*tokenEnv) {
        std::cerr << "Set LIBWSC_EXAMPLE_TOKEN first\n";
        return 2;
    }
    const std::string expectedToken(tokenEnv);

    WebSocketServer server;
    server.setAuthCallback([expectedToken](const std::string& token) {
        return token == expectedToken; // Example only; use a secure validator in production.
    });
    server.onConnect([&server](WebSocketServer::ConnectionId id) {
        std::cout << "Connected: " << id << '\n';
        server.send(id, "Welcome to libwsc");
    });
    server.onMessage([&server](WebSocketServer::ConnectionId id,
                               const std::string& payload, bool binary) {
        server.send(id, payload, binary); // Text/binary echo to this connection.
    });
    server.onDisconnect([](WebSocketServer::ConnectionId id) {
        std::cout << "Disconnected: " << id << '\n';
    });

    const bool ok = mode == "wss"
        ? server.listenTLS("127.0.0.1", static_cast<uint16_t>(port), argv[3], argv[4])
        : server.listen("127.0.0.1", static_cast<uint16_t>(port));
    if (!ok) {
        std::cerr << "Failed to start server (WSS requires USE_TLS=ON)\n";
        return 1;
    }
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::cout << "Listening on " << mode << "://127.0.0.1:" << port << '\n';
    while (!shutdownRequested)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop(); // Call outside server callbacks.
    return 0;
}
