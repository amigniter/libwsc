/*
 *  WebSocketClient.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <cstring>
#include "WebSocketHeaders.h"
#include "WebSocketTLSOptions.h"

class WebSocketContext;

/**
 * \brief Asynchronous WebSocket client.
 *
 * Provides a non-blocking WebSocket client with optional compression
 * and user-defined callbacks for connection lifecycle and messages.
 */
class WebSocketClient {
public:

    enum class MessageType {
        TEXT,
        BINARY,
        PING,
        PONG,
        CLOSE
    };

    enum class ConnectionState {
        DISCONNECTED,
        DISCONNECTING,
        CONNECTING,
        CONNECTED,
        FAILED
    };

    enum class ErrorCode {
        IO = 1,
        INVALID_HEADER,
        SERVER_MASKED,
        NOT_SUPPORTED,
        PING_TIMEOUT,
        CONNECT_FAILED,
        TLS_INIT_FAILED,
        SSL_HANDSHAKE_FAILED,
        SSL_ERROR,
        TIMEOUT,
        PROTOCOL
    };

    enum class CloseCode {
        NORMAL = 1000,
        GOING_AWAY = 1001,
        PROTOCOL_ERROR = 1002,
        UNSUPPORTED = 1003,
        NO_STATUS = 1005,
        ABNORMAL = 1006,
        INVALID_PAYLOAD = 1007,
        POLICY_VIOLATION = 1008,
        MESSAGE_TOO_BIG = 1009,
        MANDATORY_EXTENSION = 1010,
        INTERNAL_ERROR = 1011,
        SERVICE_RESTART = 1012,
        TRY_AGAIN_LATER = 1013,
        TLS_HANDSHAKE = 1015,
    
        UNKNOWN = 0  // fallback/default
    };

    // Define a message callback type
    using OpenCallback = std::function<void()>;
    using CloseCallback = std::function<void(int code, const std::string& reason)>;
    using ErrorCallback = std::function<void(int error_code, const std::string& error_message)>;
    using MessageCallback = std::function<void(const std::string&)>;
    using BinaryCallback = std::function<void(const void*, size_t)>;

    /**
     * \brief Construct a new WebSocket client instance.
     */
    WebSocketClient();

    /**
     * \brief Destroy the WebSocket client and release all resources.
     */
    virtual ~WebSocketClient();

    // non-copyable
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;
    WebSocketClient(WebSocketClient&&) noexcept = default;
    WebSocketClient& operator=(WebSocketClient&&) noexcept = default;

    /**
     * \brief Initiate a WebSocket connection.
     *
     * This method is non-blocking and returns immediately.
     * 
     * Connection success or failure is reported asynchronously
     * via the configured callbacks.
     */
    void connect();

    /**
     * \brief Gracefully close the WebSocket connection.
     */
    void disconnect();

    /**
     * \brief Check whether the client is currently connected.
     *
     * \return true if the client is connected, false otherwise.
     */
    bool isConnected();

    /**
     * \brief Send a text message.
     *
     * The message is queued if the connection is not yet established.
     *
     * \param message Text message to send.
     * \return true if the message was accepted for sending,
     *         false if the client is not usable.
     */
    bool sendMessage(const std::string& message);

    /**
     * \brief Send a text message from a raw character buffer.
     *
     * The message is queued if the connection is not yet established.
     *
     * \param msg Pointer to the message buffer.
     * \param len Length of the message in bytes.
     * \return true if the message was accepted for sending,
     *         false if the client is not usable.
     */
    bool sendMessage(const char* msg, size_t len);

    /**
     * \brief Send a binary message.
     *
     * \param data Pointer to binary data buffer.
     * \param length Size of the binary data in bytes.
     * \return true if the message was accepted for sending,
     *         false if the client is not usable.
     */
    bool sendBinary(const void* data, size_t length);

    /**
     * \brief Set the WebSocket server URL.
     *
     * \param url WebSocket endpoint URL (ws:// or wss://).
     */
    void setUrl(const std::string& url);

    /**
     * \brief Set callback invoked when the WebSocket connection is opened.
     *
     * \param callback User callback function.
     */
    void setOpenCallback(OpenCallback callback);

    /**
     * \brief Set callback invoked when the WebSocket connection is closed.
     *
     * \param callback User callback function.
     */
    void setCloseCallback(CloseCallback callback);

    /**
     * \brief Set callback invoked on protocol or transport errors.
     *
     * \param callback User callback function receiving an error description.
     */
    void setErrorCallback(ErrorCallback callback);

    /**
     * \brief Set callback invoked when a text message is received.
     *
     * \param callback User callback function receiving the message payload.
     */
    void setMessageCallback(MessageCallback callback);

    /**
     * \brief Set callback invoked when a binary message is received.
     *
     * \param callback User callback function receiving the binary payload.
     */
    void setBinaryCallback(BinaryCallback callback);

    /**
     * \brief Set custom WebSocket handshake headers.
     *
     * The provided headers are sent during the initial WebSocket handshake.
     * This method must be called before connect().
     *
     * \param headers Collection of header key-value pairs.
     */
    void setHeaders(const WebSocketHeaders& headers);

    /**
     * \brief Set TLS configuration for secure WebSocket connections.
     *
     * The provided options are applied when connecting to wss:// endpoints.
     * This method must be called before connect().
     *
     * \param options TLS configuration options.
     */
    void setTLSOptions(const WebSocketTLSOptions& options);

    /**
     * \brief Set the ping interval.
     *
     * \param seconds Ping interval in seconds.
     *                A value of 0 disables automatic pings.
     */
    void setPingInterval(int interval);

    /**
     * \brief Set the connection timout.
     *
     * \param timeout Timeout in seconds.
     *                A value of 0 disables connection timeout.
     */
    void setConnectionTimeout(int timeout);

    /**
     * \brief Enable or disable permessage-deflate compression.
     *
     * \param enable Set to true to enable compression, false to disable.
     */
    void enableCompression(bool enable = true);

private:
    // Connection properties
    std::string host;
    unsigned short port;
    std::string uri;
    bool secure;
    bool is_ip_address;
    unsigned int ping_interval = 0;
    unsigned int connection_timeout = 1;
    bool compression_requested = true;

    static bool isHostIPAddress(const std::string& host);

    OpenCallback open_callback;
    CloseCallback close_callback;
    ErrorCallback error_callback;
    MessageCallback message_callback;
    BinaryCallback binary_callback;

    std::shared_ptr<WebSocketContext> _ctx;

    WebSocketHeaders extra_headers;
    WebSocketTLSOptions tls_options;
};
