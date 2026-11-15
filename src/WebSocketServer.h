#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/** @brief Standalone WebSocket server with optional TLS, bearer authentication, and automatic PING/PONG heartbeat (30s/10s). */
class WebSocketServer {
public:
	using ConnectionId = uint64_t;
	using ConnectCallback = std::function<void(ConnectionId)>;
	using MessageCallback = std::function<void(ConnectionId, const std::string&, bool binary)>;
	using AuthCallback = std::function<bool(const std::string& bearerToken)>;
	using DisconnectCallback = std::function<void(ConnectionId)>;

	WebSocketServer();
	~WebSocketServer();

	WebSocketServer(const WebSocketServer&) = delete;
	WebSocketServer& operator=(const WebSocketServer&) = delete;

	/** @brief Start listening for plain WebSocket connections. Returns false on failure. */
	bool listen(const std::string& address = "127.0.0.1", uint16_t port = 8081);

	/** @brief Start a TLS WebSocket listener using the supplied certificate and key.
	 *  @return False if startup fails or TLS support was disabled at build time.
	 */
	bool listenTLS(const std::string& address, uint16_t port,
	               const std::string& certFile, const std::string& keyFile);

	/** @brief Stop the server and release connections. Call outside server callbacks. */
	void stop();

	/** @brief Check whether the server is running. */
	bool isRunning() const;

	/** @brief Queue a text or binary message for one connection.
	 *  @return False if the connection is missing, the server is stopped, or the queue is full.
	 */
	bool send(ConnectionId id, const std::string& payload, bool binary = false);

	/** @brief Queue a text or binary message for all connected clients. */
	void broadcast(const std::string& payload, bool binary = false);

	/**
	 * @brief Register a Bearer token authentication callback.
	 *
	 * The WebSocket server extracts the token from the HTTP
	 * Authorization header:
	 *
	 *     Authorization: Bearer <token>
	 *
	 * Authentication is performed before the WebSocket upgrade.
	 *
	 * Token generation, storage, validation, and expiration policies
	 * are entirely the responsibility of the application.
	 *
	 * The callback receives the Bearer token and must return:
	 *   - true  if the token is valid
	 *   - false if the token is invalid
	 *
	 * Connections with missing or invalid tokens receive HTTP 401.
	 *
	 * An authentication callback must be registered before starting
	 * the server. The server will not start without one.
	 *
	 * @note The library does not implement any specific token
	 *       validation mechanism (SHA-256, JWT, Ed25519, etc.).
	 *       Applications must implement their own validation.
	 *
	 * @warning Bearer tokens must be transmitted over WSS when
	 *          connecting across untrusted networks.
	 */
	void setAuthCallback(AuthCallback callback);

	/** @brief Register a callback invoked after a successful WebSocket upgrade. */
	void onConnect(ConnectCallback callback);

	/** @brief Register a callback for incoming messages. */
	void onMessage(MessageCallback callback);

	/** @brief Register a callback for disconnected upgraded connections. */
	void onDisconnect(DisconnectCallback callback);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
