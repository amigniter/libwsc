/*
 *  WebSocketClient.cpp
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#include "WebSocketClient.h"
#include "Logger.h"

/**
 * \brief Default constructor initializes internal state and prepares for a new connection.
 *
 * - Sets flags (secure, upgraded, running) to false and pointers to nullptr.
 * - Generates the Sec-WebSocket-Key and corresponding expected accept value.
 * - Initializes libevent for use with pthreads.
 */
WebSocketClient::WebSocketClient() 
        : secure(false), upgraded(false), running(false), base(nullptr), dns_base(nullptr), m_bev(nullptr), event_thread(nullptr) {
        key = getWebSocketKey(); //dGhlIHNhbXBsZSBub25jZQ==
        accept = computeAccept(key); //s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
        log_debug("Computed accept: %s", accept.c_str());
        evthread_use_pthreads();
}

/**
 * \brief Destructor ensures the client is cleanly disconnected and all resources freed.
 */
WebSocketClient::~WebSocketClient() {
    //log_debug("destructor entered");
    disconnect();
    //log_debug("destructor exited");
}

/**
 * \brief Parse and store the WebSocket endpoint components from a URL.
 *
 * Extracts the scheme (ws or wss), host, port (defaulting to 80 or 443 if omitted),
 * and request path (URI) from the provided URL string.
 *
 * \param url  The full WebSocket URL, which must begin with "ws://" or "wss://".
 */
void WebSocketClient::setUrl(const std::string& url) {
    const std::string ws_scheme = "ws://";
    const std::string wss_scheme = "wss://";

    size_t pos = 0;
    if (url.compare(0, ws_scheme.size(), ws_scheme) == 0) {
        secure = false;
        pos = ws_scheme.size();
    } else if (url.compare(0, wss_scheme.size(), wss_scheme) == 0) {
        secure = true;
        pos = wss_scheme.size();
    } else {
        log_error("URL must start with ws:// or wss://");
        return;
    }

    size_t path_pos = url.find('/', pos);
    std::string hostport = (path_pos == std::string::npos) ? url.substr(pos) : url.substr(pos, path_pos - pos);

    size_t colon_pos = hostport.find(':');
    if (colon_pos != std::string::npos) {
        host = hostport.substr(0, colon_pos);
        try {
            port = std::stoi(hostport.substr(colon_pos + 1));
        } catch (const std::exception& e) {
            //log_error("Invalid port in URL: %s", e.what());
            return;
        }
    } else {
        host = hostport;
        port = secure ? 443 : 80;
    }

    if (host.empty()) {
        log_error("Host is empty in URL");
        return;
    }

    uri = (path_pos == std::string::npos) ? "/" : url.substr(path_pos);

    is_ip_address = isHostIPAddress(host);
}

/**
 * \brief Check if the given host string is an IP address (IPv4 or IPv6)
 * 
 * \param host The host string to check
 * \return true if host is an IP address, false if it's a domain name
 */
bool WebSocketClient::isHostIPAddress(const std::string& host) {
    // Check for IPv4 address
    struct in_addr addr4;
    if (inet_pton(AF_INET, host.c_str(), &addr4) == 1) {
        return true;
    }
    
    // Check for IPv6 address (enclosed in brackets or not)
    struct in6_addr addr6;
    std::string host_clean = host;
    
    // Remove IPv6 brackets if present
    if (host.size() >= 2 && host[0] == '[' && host[host.size()-1] == ']') {
        host_clean = host.substr(1, host.size() - 2);
    }
    
    if (inet_pton(AF_INET6, host_clean.c_str(), &addr6) == 1) {
        return true;
    }
    
    // If it's not a valid IP address, it's a domain name
    return false;
}

/**
 * \brief Initialize and begin the asynchronous WebSocket connection.
 *
 * - Resets any previous cleanup flags.
 * - Loads and configures TLS (if secure mode is enabled).
 * - Transitions the internal state to CONNECTING and notifies waiters.
 * - Creates the libevent base and DNS resolver.
 * - Allocates a bufferevent (SSL or plain) with timeout handling.
 * - Registers read/write/event callbacks and starts hostname resolution + TCP connect.
 * - Marks the client as running and spawns a dedicated thread to dispatch the event loop.
 */
void WebSocketClient::connect() {    
    cleanup_complete.store(false);

    if (running.load()) {
        log_debug("Already connected or connecting");
        return;
    }

    if (host.empty() || port <=0) {
        log_error("setUrl() must be called before connect(): invalid host or port");
        sendError(ErrorCode::CONNECT_FAILED, "Invalid host or port");
        return;
    }

#ifdef USE_TLS
    SSL *ssl = nullptr;
#endif

    if(secure) {
#ifdef USE_TLS

        if(!initTLS()) {
            sendError(ErrorCode::TLS_INIT_FAILED, "Failed to initialize TLS");
            return;
        }

        ssl = SSL_new(ctx);


        if (!ssl) {
            SSL_CTX_free(ctx);
            log_error("SSL_new() failed");
            sendError(ErrorCode::TLS_INIT_FAILED, "SSL context creation failed");
            return;
        }

        if(!is_ip_address) {
            SSL_set_tlsext_host_name(ssl, host.c_str());
        }
        
        if (!tlsOptions.disableHostnameValidation) {
            X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
            if (param) {
                int ret = X509_VERIFY_PARAM_set1_host(param, host.c_str(), 0);  // No port matching
                if (ret != 1) {
                    log_error("Failed to set hostname for verification");
                    sendError(ErrorCode::TLS_INIT_FAILED, "Hostname verification setup failed.");
                    SSL_free(ssl);
                    SSL_CTX_free(ctx);
                    ctx = nullptr;
                    return; 
                }
            }
        }
#else 
        log_error("TLS support not compiled in (USE_TLS=OFF), proceeding in insecure mode");
        secure = false;
#endif
    }

    connection_state.store(ConnectionState::CONNECTING, std::memory_order_release);

    base = event_base_new();
    if (!base) {
        log_error("Failed to create event_base");
        sendError(ErrorCode::IO, "Failed to create event_base");
        return;
    }

    dns_base = evdns_base_new(base, 1);
    if (!dns_base) {
        log_error("Failed to create DNS base");
        event_base_free(base);
        base = nullptr;
        sendError(ErrorCode::IO, "Failed to create DNS base");
        return;
    }

    if (secure) {
#ifdef USE_TLS
        m_bev = bufferevent_openssl_socket_new(base, -1, ssl, BUFFEREVENT_SSL_CONNECTING, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
        if (!m_bev) {
            log_error("Failed to create secure bufferevent");
            cleanup();
            sendError(ErrorCode::IO, "Failed to create secure bufferevent");
            return;
        }
#endif
    } else {
        m_bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
        if (!m_bev) {
            log_error("Failed to create bufferevent");
            cleanup();
            sendError(ErrorCode::IO, "Failed to create bufferevent");
            return;
        }
    }

    
    struct timeval timeout;
    timeout.tv_sec = connection_timeout;
    timeout.tv_usec = 0;

    timeout_event = event_new(base, -1, EV_TIMEOUT, timeoutCallback, this);
    event_add(timeout_event, &timeout);
    
    if (ping_interval > 0) {
        struct timeval tv;
	    tv.tv_sec = ping_interval;
  	    tv.tv_usec = 0;

        ping_event = event_new(base, -1, EV_PERSIST, pingCallback, this);
        evtimer_add(ping_event, &tv);
    }

    bufferevent_setcb(m_bev, readCallback, writeCallback, eventCallback, this);
    bufferevent_enable(m_bev, EV_READ|EV_WRITE);

    if (bufferevent_socket_connect_hostname(m_bev, dns_base, AF_INET, host.c_str(), port) < 0) {
        log_error("Failed to start connection");
        cleanup();
        sendError(ErrorCode::CONNECT_FAILED, "Failed to start connection");
        return;
    }

    running.store(true);

    event_thread = new std::thread([this]() {
        int ret = event_base_dispatch(base);
        if (ret == -1) {
            log_error("Event base dispatch failed");
        }

        log_debug("Event loop exited");
        
        running.store(false);
    });
}

/**
 * \brief Gracefully disconnects the WebSocket client and stops its event loop.
 *
 * - Transitions state to DISCONNECTING and notifies any waiters.
 * - Signals the event base to exit and disables IO on the bufferevent.
 * - Joins and cleans up the event thread.
 * - Invokes cleanup() to free all resources.
 * - Finally, sets state to DISCONNECTED and notifies waiters again.
 */
void WebSocketClient::disconnect() {
    log_debug("disconnect: entering");

    auto current_state = connection_state.load(std::memory_order_acquire);
    if (current_state == ConnectionState::DISCONNECTING || current_state == ConnectionState::DISCONNECTED) {
        log_debug("disconnect: early exit");
        return;
    }

    connection_state.store(ConnectionState::DISCONNECTING, std::memory_order_release);

    // send clean Close frame (if possible) and wait briefly for peer reply
    bool sentClose = close();
    if (sentClose) {
        std::unique_lock<std::mutex> lk(state_mutex);
        state_cv.wait_for(lk,
                        std::chrono::seconds(2),
                        [&]{ return got_remote_close.load(); });
    }

    if (base && running.load()) {
        bufferevent_lock(m_bev);
        bufferevent_disable(m_bev, EV_READ | EV_WRITE);
        // schedule a zero-timeout no-op so the loop definitely wakes
        struct timeval zero = {0,0};
        event_base_once(base,
                        /*fd*/-1,
                        EV_TIMEOUT,
                        /*cb*/ [](evutil_socket_t, short, void*) {
                        log_debug("zero-timeout callback fired");
                        },
                        /*arg*/ nullptr,
                        &zero);
        event_base_loopexit(base, nullptr);
        bufferevent_unlock(m_bev);
    }

    auto self = this;
    auto evId = event_thread ? event_thread->get_id() : std::thread::id{};
    auto meId = std::this_thread::get_id();

    // If the shutdown is happening from *inside* our event‐loop thread,
    // we cannot join that same thread—it would deadlock or crash.
    // So we defer the actual join+cleanup to a helper thread
    if (event_thread && meId == evId) {
      log_debug("disconnect: on event thread—deferring join+cleanup");
      std::thread([self](){
        // Wait for the loop to actually stop
        while (self->running.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (self->event_thread && self->event_thread->joinable()) {
          self->event_thread->join();
        }
        delete self->event_thread;
        self->event_thread = nullptr;

        self->cleanup();

        self->connection_state.store(ConnectionState::DISCONNECTED, std::memory_order_release);

        log_debug("disconnect: deferred exit");
      }).detach();

      return;
    }

    // Otherwise—called from a non‐event thread—do the normal join+cleanup
    if (event_thread && event_thread->joinable() ) {
      log_debug("Waiting for event thread to join...");
      event_thread->join();
      log_debug("Event thread joined");
    }

    delete event_thread;
    event_thread = nullptr;

    cleanup();

    connection_state.store(ConnectionState::DISCONNECTED, std::memory_order_release);

    log_debug("disconnect: exited");
}

/**
 * \brief Returns true if the client is either in CONNECTING or CONNECTED state.
 *        This allows sendData to enqueue frames during the handshake.
 */
bool WebSocketClient::isConnected() {
    auto current_state = connection_state.load(std::memory_order_acquire);
    return current_state == ConnectionState::CONNECTING
        || current_state == ConnectionState::CONNECTED;
}

/**
 * \brief Core send routine that either queues or immediately transmits a WebSocket frame.
 *
 * \param data    Pointer to the frame payload.
 * \param length  Length of the payload in bytes.
 * \param type    MessageType indicating TEXT or BINARY framing.
 *
 * - If still CONNECTING, enqueues up to MAX_QUEUE_SIZE to send later.
 * - Once CONNECTED (and upgraded), locks the bufferevent and writes the frame.
 * - Logs and drops packets if queue is full or state is invalid.
 */
bool WebSocketClient::sendData(const void* data,
                               size_t length,
                               MessageType type) {
    if (!m_bev) {
        log_error("No bufferevent—cannot send");
        return false;
    }

    ConnectionState state = connection_state.load(std::memory_order_acquire);

    // Queue
    if (state == ConnectionState::CONNECTING) {
        std::lock_guard<std::mutex> lk(send_queue_mutex);
        if (send_queue.size() >= MAX_QUEUE_SIZE) {
            log_error("Send queue full—dropping packet");
            return false;
        }
        if (type == MessageType::TEXT) {
            send_queue.emplace_back(
                std::string(reinterpret_cast<const char*>(data), length)
            );
        } else {
            send_queue.emplace_back(
                std::vector<uint8_t>(
                reinterpret_cast<const uint8_t*>(data),
                reinterpret_cast<const uint8_t*>(data) + length
                )
            );
        }
        log_debug("Queued %zu bytes during CONNECTING", length);
        return true;
    }

    if (state == ConnectionState::CONNECTED ||
         (type == MessageType::CLOSE && state == ConnectionState::DISCONNECTING)) {
        // only require full upgrade for non-close frames
        if (type != MessageType::CLOSE && !upgraded.load()) {
            log_error("WebSocket not fully upgraded yet");
            return false;
        }
        bufferevent_lock(m_bev);

        evbuffer* output = bufferevent_get_output(m_bev);
        if (!output) {
            return false;
        }

        send(output, data, length, type);

        bufferevent_unlock(m_bev);

        return true;
    }

    log_error("Cannot send in state %d", int(state));
    return false;
}

/**
 * \brief Flushes any frames that were queued during CONNECTING.
 *
 * Dequeues each pending packet and calls sendData to transmit it.
 * Once sent, the packet is removed from the internal queue.
 */
void WebSocketClient::flushSendQueue() {
    std::lock_guard<std::mutex> lk(send_queue_mutex);
    while (!send_queue.empty()) {
        auto& p = send_queue.front();
        if (p.type == Pending::Text) {
            sendData(p.text.data(),
                     p.text.size(),
                     MessageType::TEXT);
        } else {
            sendData(p.bin.data(),
                     p.bin.size(),
                     MessageType::BINARY);
        }
        send_queue.pop_front();
    }
}

// Send a text message from an std::string
bool WebSocketClient::sendMessage(const std::string& message) {
    return sendData(message.c_str(), message.length(), MessageType::TEXT);
}

// Send a text message from a raw buffer
bool WebSocketClient::sendMessage(const char* msg, size_t len) {
    return sendData(msg, len, MessageType::TEXT);
}

// Send a binary payload over the WebSocket
bool WebSocketClient::sendBinary(const void* data, size_t length) {
    return sendData(data, length, MessageType::BINARY);
}

// Set the callback to invoke on incoming text messages
void WebSocketClient::setMessageCallback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    message_callback = callback;
}

// Set the callback to invoke on incoming binary messages
void WebSocketClient::setBinaryCallback(BinaryCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    binary_callback = callback;
}

// Set the callback to invoke when the connection closes
void WebSocketClient::setCloseCallback(CloseCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    close_callback = callback;
}

// Set the callback to invoke on errors
void WebSocketClient::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    error_callback = callback;
}

// Set the callback to invoke when the connection opens
void WebSocketClient::setOpenCallback(OpenCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex);
    open_callback = callback;
}

// Invoke the error callback (or log if none set)
void WebSocketClient::sendError(int error_code, const std::string& error_message) {
    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        callback = error_callback;
    }

    if (callback) {
        callback(error_code, error_message);
    } else {
        log_error("Unhandled error: %s", error_message.c_str());
    }
}

// Overload: send an error using the CloseCode enum
void WebSocketClient::sendError(ErrorCode code, const std::string& message) {
    sendError(static_cast<int>(code), message);
}

// Send a WebSocket close frame with code and reason
bool WebSocketClient::close(int code, const std::string& reason) {
    if (!upgraded.load() || !m_bev) {
        log_error("Not connected or WebSocket not upgraded");
        return false;
    }
    if (sent_close.exchange(true)) {
        return false;
    }
    // build the payload: 2-byte code (network order) + optional UTF-8 reason
    uint16_t code_be = htons(static_cast<uint16_t>(code));
    std::vector<uint8_t> payload(sizeof(code_be) + reason.size());
    memcpy(payload.data(), &code_be, sizeof(code_be));
    memcpy(payload.data() + sizeof(code_be),
           reason.data(), reason.size());
    
    return sendData(payload.data(), payload.size(), MessageType::CLOSE);
}

// Overload: close using the CloseCode enum
bool WebSocketClient::close(CloseCode code, const std::string& reason) {
    return close(static_cast<int>(code), reason);
}

/**
 * \brief Sends the WebSocket upgrade request over the bufferevent.
 *
 * - Ensures the bufferevent is valid, then locks it for thread-safe writes.
 * 
 * - Writes the HTTP GET line and core headers (Host, Upgrade, Connection,
 *   Sec-WebSocket-Key/Version).
 * 
 * - Optionally includes the permessage-deflate extension when compression is requested.
 * 
 * - Appends any user-provided extra headers, ends with a blank line, and unlocks
 *   the bufferevent to transmit the handshake.
 */
void WebSocketClient::sendHandshakeRequest() {
    if (!m_bev) return;

    log_debug("Sending WebSocket handshake request");

    bufferevent_lock(m_bev);

    auto out = bufferevent_get_output(m_bev);    

    evbuffer_add_printf(out, "GET %s HTTP/1.1\r\n", uri.c_str());
    evbuffer_add_printf(out, "Host:%s:%d\r\n", host.c_str(), port);
    evbuffer_add_printf(out, "Upgrade:websocket\r\n");
    evbuffer_add_printf(out, "Connection:upgrade\r\n");
    evbuffer_add_printf(out, "Sec-WebSocket-Key:%s\r\n", key.c_str());
    evbuffer_add_printf(out, "Sec-WebSocket-Version:13\r\n");
    if (compression_requested) {
        evbuffer_add_printf(out, "Sec-WebSocket-Extensions:permessage-deflate; client_no_context_takeover; server_no_context_takeover; client_max_window_bits=9\r\n");
    }
    evbuffer_add_printf(out, "Origin:http://%s:%d\r\n", host.c_str(), port);
    if (!extraHeaders.headers.empty()) {
        for (const auto& header : extraHeaders.headers) {
            evbuffer_add_printf(out, "%s:%s\r\n", header.first.c_str(), header.second.c_str());
        }
    }
    evbuffer_add_printf(out, "\r\n");

    bufferevent_unlock(m_bev);
}

/**
 * \brief Release all resources and transition the client to a clean, disconnected state.
 *
 * This method is idempotent (runs only once) and performs:
 *  - Early exit if cleanup has already completed
 *  - Cleanup of ping and timeout libevent events
 *  - Proper SSL shutdown (when built with TLS) and freeing of SSL context
 *  - Freeing of the libevent bufferevent
 *  - Cleanup of DNS and event bases
 *  - Closing of zlib inflate/deflate streams
 *  - Resetting of internal flags (upgraded, running)
 *  - Transitioning connection state to DISCONNECTED and notifying waiters
 * 
 * \note Called both on connect-failure (before the event loop
 *      ever starts) and on normal teardown (after the event loop has
 *      been cleanly exited and joined).  In neither case is dispatch()
 *      running, so there is no concurrent access to bufferevent or base,
 *      and we can free them without locking.
 */
void WebSocketClient::cleanup() {
    if (cleanup_complete.load()) return;
    cleanup_complete.store(true);

    log_debug("cleanup: entered");

    // Clean up events first
    if (ping_event) {
        event_free(ping_event);
        ping_event = nullptr;
    }

    if (timeout_event) {
        event_free(timeout_event);
        timeout_event = nullptr;
    }

    // Clean up bufferevent
    if (m_bev) {
        // bufferevent_lock(m_bev);
        // For SSL, we need to do proper shutdown
        if (secure) {
#ifdef USE_TLS
            // Internally handled by libevent
            SSL* ssl = bufferevent_openssl_get_ssl(m_bev);
            if (ssl) {
                SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
                SSL_shutdown(ssl);
            }
            if (ctx) {
                SSL_CTX_free(ctx);
                ctx = nullptr;
            }
#endif
        }
        bufferevent_free(m_bev);
        m_bev = nullptr;
    }

    // Clean up DNS
    if (dns_base) {
        evdns_base_free(dns_base, 0);
        dns_base = nullptr;
    }

    // Clean up event base
    if (base) {
        event_base_free(base);
        base = nullptr;
    }

    if (inflate_initialized) {
        inflateEnd(&inflate_stream);
        inflate_initialized = false;
    }

    if (deflate_initialized) {
        deflateEnd(&deflate_stream);
        deflate_initialized = false;
    }

    upgraded.store(false);
    running.store(false);

    log_debug("cleanup: exiting");
}

/**
 * \brief Core WebSocket frame construction and transmission method.
 * 
 * This method implements the complete WebSocket frame sending pipeline:
 * 1. Validates control frame size limits (RFC 6455 Section 5.5)
 * 2. Compresses payload if enabled (RFC 7692 permessage-deflate)
 * 3. Constructs frame header with proper opcode and flags
 * 4. Handles extended payload lengths (16/64-bit)
 * 5. Applies random masking (RFC 6455 Section 5.3)
 * 6. Writes to output buffer using efficient chunked operations
 * 
 * \param buf Destination libevent buffer
 * \param raw_data Payload to send
 * \param raw_len Payload length
 * \param type Frame type (TEXT/BINARY/CLOSE/PING/PONG)
 * 
 * \note Implements: RFC 6455 Sections 5.2-5.8 (Framing), RFC 7692 (Compression)
 *                      Automatic context takeover handling
 * 
 */
void WebSocketClient::send(evbuffer* buf,
                         const void* raw_data,
                         size_t raw_len,
                         MessageType type) 
{
    const bool is_control_frame = (type == MessageType::CLOSE || 
                                 type == MessageType::PING || 
                                 type == MessageType::PONG);

    if (is_control_frame && raw_len > 125) {
        log_error("Control frame too large (%zu bytes)", raw_len);
        return;
    }

    const uint8_t* original_ptr = static_cast<const uint8_t*>(raw_data);
    size_t original_len = raw_len;
    
    const uint8_t* payload_ptr = original_ptr;
    size_t payload_len = original_len;
    std::vector<uint8_t> compressed_buf;

    bool do_compress = !is_control_frame &&
                      use_compression &&
                      (type == MessageType::TEXT || type == MessageType::BINARY) &&
                      deflate_initialized;

    if (do_compress) {
        
        deflateReset(&deflate_stream);

        size_t bound = deflateBound(&deflate_stream, original_len) + 4;
        compressed_buf.resize(bound);

        deflate_stream.next_in = const_cast<Bytef*>(original_ptr);
        deflate_stream.avail_in = original_len;
        deflate_stream.next_out = compressed_buf.data();
        deflate_stream.avail_out = bound;

        int ret = deflate(&deflate_stream, Z_SYNC_FLUSH);
        
        if (ret == Z_OK || ret == Z_BUF_ERROR) {
            
            payload_len = bound - deflate_stream.avail_out;
            
            // Sync flush check
            if (payload_len >= 4 && 
                compressed_buf[payload_len-4] == 0x00 &&
                compressed_buf[payload_len-3] == 0x00 &&
                compressed_buf[payload_len-2] == 0xFF &&
                compressed_buf[payload_len-1] == 0xFF) {
                payload_len -= 4;
            }
            
            payload_ptr = compressed_buf.data();

        } else {
            log_error("Compression failed (%d), sending raw", ret);
            deflateReset(&deflate_stream);
            payload_ptr = original_ptr;  // Explicit fallback
            payload_len = original_len;
            do_compress = false;  // Important for RSV1 bit
        }

        // Handle context takeover
        if (client_no_context_takeover) {
            deflateEnd(&deflate_stream);
            int init_ret = deflateInit2(&deflate_stream, compression_level, Z_DEFLATED,
                        -client_max_window_bits, 8, Z_DEFAULT_STRATEGY);
            if (init_ret != Z_OK) {
                log_error("deflateInit2 after context takeover failed (%d)", init_ret);
                payload_ptr = original_ptr;
                payload_len = original_len;
                do_compress = false;
            }
        }
    }

    uint8_t b1 = 0x80; // FIN
    if (do_compress) b1 |= 0x40; // RSV1

    switch (type) {
        case MessageType::TEXT:   b1 |= 0x01; break;
        case MessageType::BINARY: b1 |= 0x02; break;
        case MessageType::CLOSE:  b1 |= 0x08; break;
        case MessageType::PING:   b1 |= 0x09; break;
        case MessageType::PONG:   b1 |= 0x0A; break;
    }

    // Payload length
    uint8_t b2 = 0x80; // Mask bit
    if (payload_len <= 125) {
        b2 |= payload_len;
    } else if (payload_len <= 65535) {
        b2 |= 126;
    } else {
        b2 |= 127;
    }

    log_debug("send frame: b1=0x%02X b2=0x%02X len=%zu compress=%d\n", b1, b2, payload_len, do_compress);

    auto out = buf;

    evbuffer_add(out, &b1, 1);
    evbuffer_add(out, &b2, 1);

    // Extended payload length
    if ((b2 & 0x7F) == 126) {
        uint16_t len = htons(static_cast<uint16_t>(payload_len));
        evbuffer_add(out, &len, 2);
    } else if ((b2 & 0x7F) == 127) {
        uint64_t len = htonll(static_cast<uint64_t>(payload_len));
        evbuffer_add(out, &len, 8);
    }

    // Chunked masking implementation
    uint8_t mask_key[4];
    std::random_device rd;
    std::uniform_int_distribution<uint8_t> distrib(0, 255);
    for (int i = 0; i < 4; ++i) mask_key[i] = distrib(rd);
    evbuffer_add(out, mask_key, 4);

    uint32_t mask_32;
    memcpy(&mask_32, mask_key, 4);
    
    size_t i = 0;
    const size_t aligned_len = payload_len & ~0x03;
    const uint8_t* src = payload_ptr;
    
    // Process 32-bit chunks
    for (; i < aligned_len; i += 4) {
        uint32_t chunk;
        memcpy(&chunk, src + i, 4);
        chunk ^= mask_32;
        evbuffer_add(out, &chunk, 4);
    }
    
    // Process remaining bytes
    for (; i < payload_len; ++i) {
        uint8_t byte = src[i] ^ mask_key[i % 4];
        evbuffer_add(out, &byte, 1);
    }
}

/**
 * \brief Core WebSocket frame processor - continuously parses frames until buffer is drained.
 * 
 * This method implements a streaming frame processor that:
 * 1. Runs in a loop while data remains in the input buffer
 * 2. Completely processes each frame before draining it
 * 3. Returns control to libevent's handleRead() when either:
 *    - Buffer is fully drained (normal case)
 *    - Partial frame received (waits for more data)
 *    - Protocol error occurs (terminates connection)
 * 
 * The processing pipeline for each frame:
 * [Header parsing -> Validation -> Length processing -> Unmasking -> Dispatch]
 * 
 * \param buf libevent buffer containing raw WebSocket frames
 * 
 * \note Return to handleRead() occurs through:
 *       - Explicit 'break' when waiting for more data
 *       - Normal loop exit after final frame
 *       - Early return on protocol errors
 * 
 * \warning Maintains no internal buffer - completely drains processed frames
 *           to prevent reprocessing on next handleRead() invocation
 */
void WebSocketClient::receive(evbuffer* buf) {
    for (;;) {
        const size_t data_len = evbuffer_get_length(buf);
        if (data_len < 2)
            break;

        // Peek at the header without pullup yet
        unsigned char hdr[14];
        const size_t peek_len = std::min(data_len, sizeof(hdr));
        evbuffer_copyout(buf, hdr, peek_len);

        const bool fin  = !!(hdr[0] & 0x80);
        const bool rsv1 = !!(hdr[0] & 0x40);
        const bool rsv2 = !!(hdr[0] & 0x20);
        const bool rsv3 = !!(hdr[0] & 0x10);
        const int opcode = hdr[0] & 0x0F;
        const bool mask = !!(hdr[1] & 0x80);
        uint64_t payload_len = hdr[1] & 0x7F;

        if ((!use_compression && rsv1) || rsv2 || rsv3) {
            close(CloseCode::PROTOCOL_ERROR, "Unexpected RSV bits");
            return;
        }

        if ((opcode & 0x08) != 0 && !fin) {
            close(CloseCode::PROTOCOL_ERROR, "Control frame fragmented");
            return;
        }

        if (mask) {
            close(CloseCode::PROTOCOL_ERROR, "Masked frame from server");
            return;
        }

        size_t header_len = 2;

        if (payload_len == 126) {
            if (data_len < header_len + 2) break;
            header_len += 2;
            //payload_len = ntohs(*(uint16_t*)&hdr[2]);
            uint16_t u16; std::memcpy(&u16, &hdr[2], sizeof(u16));
            payload_len = ntohs(u16);
        } else if (payload_len == 127) {
            if (data_len < header_len + 8) break;
            header_len += 8;
            //payload_len = ntohll(*(uint64_t*)&hdr[2]);
            uint64_t u64; std::memcpy(&u64, &hdr[2], sizeof(u64));
            payload_len = ntohll(u64);
        }

        const size_t need = header_len + static_cast<size_t>(payload_len);
        if (data_len < /*header_len + payload_len*/need) {
            // Waiting for full frame
            break;
        }

        // Pull the full frame
        unsigned char* frame = evbuffer_pullup(buf, need);
        // Payload pointer inside the pulled-up frame
        const unsigned char* payload_ptr = frame + header_len;

        std::vector<uint8_t> payload;
        payload.reserve(static_cast<size_t>(payload_len));
        payload.insert(payload.end(), payload_ptr, payload_ptr + static_cast<size_t>(payload_len));

        switch (opcode) {
            case 0x00:
                handleContinuationFrame(payload.data(), payload.size(), fin);
                break;
            case 0x01:
            case 0x02:
                handleDataFrame(payload.data(), payload.size(), fin, opcode, rsv1);
                break;
            case 0x08:
                handleCloseFrame(payload.data(), payload.size());
                break;
            case 0x09:
                handlePingFrame(payload.data(), payload.size());
                break;
            case 0x0A:
                log_debug("Received pong frame");
                break;
            default:
                log_error("Unknown opcode: %d", opcode);
                close(CloseCode::PROTOCOL_ERROR, "Unsupported opcode");
                break;
        }

        evbuffer_drain(buf, header_len + payload_len);
    }
}

/**
 * \brief Processes WebSocket continuation frames for fragmented messages.
 * 
 * Handles message fragmentation by:
 * 1. Validating proper frame sequencing
 * 2. Accumulating fragments
 * 3. Performing incremental UTF-8 validation (for text messages)
 * 4. Finalizing message processing when FIN flag is set
 * 
 * \param payload Pointer to the payload bytes
 * \param payload_len Payload length (may be 0)
 * \param fin FIN flag (1=final fragment)
 * 
 * \note Implements RFC 6455 Section 5.4 (Fragmentation)
 * \warning Enforces:
 *          - Proper fragmentation sequence
 *          - UTF-8 validity for text messages
 *          - Decompression success for compressed messages
 * 
 * \see handleDataFrame() for initial fragment processing
 * \see decompressMessage() for compression handling
 */
void WebSocketClient::handleContinuationFrame(const unsigned char* payload, size_t payload_len, bool fin) {
    if (!message_in_progress) {
        log_error("Received continuation frame without initial frame");
        close(CloseCode::PROTOCOL_ERROR, "continuation frame without initial frame"); //POLICY_VIOLATION
        return;
    }
        
    // Append this fragment to our accumulated message
    fragmented_message.insert(fragmented_message.end(), 
                            payload,
                            payload + payload_len);

    // Only validate UTF-8 if this is an uncompressed text message
    if (!compressed_message_in_progress && fragmented_opcode == 0x01) {
        if (!utf8Validator.validateChunk(payload, payload_len)) {
            log_error("Invalid UTF-8 in continuation frame");
            utf8Validator.reset();
            close(CloseCode::INVALID_PAYLOAD, "Invalid UTF-8 in text message");
            return;
        }
    }

    if (!fin) return;

    // log_debug("Final fragment received, processing complete message");
    if (compressed_message_in_progress) {
        std::vector<uint8_t> output;
        bool ok = decompressMessage(fragmented_message.data(), fragmented_message.size(), output);
        if (!ok) {
            utf8Validator.reset();
            close(CloseCode::INVALID_PAYLOAD, "Decompression failed");
            return;
        }
        //fragmented_message = std::move(output);
        fragmented_message.swap(output);
    }

    switch (fragmented_opcode) {
        case 0x01: {
            // Finalize the DFA
            // For compressed text: we skipped chunk checks — validate now over the whole inflated buffer.
            // For uncompressed text: we've streamed chunks — do a final boundary check.
            bool ok = true;
            if (compressed_message_in_progress) {
                utf8Validator.reset();
                ok = utf8Validator.validateChunk(fragmented_message.data(), fragmented_message.size())
                  && utf8Validator.validateFinal();
            } else {
                ok = utf8Validator.validateFinal();
            }
            if (!ok) {
                log_error("Invalid UTF-8 at end of fragmented text");
                utf8Validator.reset();
                close(CloseCode::INVALID_PAYLOAD, "Invalid UTF-8 in text message");
                return;
            }

            std::string message(fragmented_message.begin(), fragmented_message.end());
            utf8Validator.reset();

            MessageCallback cb;
            { std::lock_guard<std::mutex> lk(callback_mutex); cb = message_callback; }
            if (cb) cb(message);
            break;
        }
        case 0x02: {
            BinaryCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex);
                callback = binary_callback;
            }
            if (callback) {
                callback(fragmented_message.data(), fragmented_message.size());
            }
            break;
        }
        default:
            log_error("Unknown fragmented opcode: %d", fragmented_opcode);
    }

    // Reset fragmentation state
    message_in_progress = false;
    compressed_message_in_progress = false;
    //fragmented_message.clear();
    std::vector<uint8_t>().swap(fragmented_message); // frees capacity
    fragmented_opcode = 0;
}

/**
 * \brief Processes incoming WebSocket data frames (text/binary) with full protocol compliance.
 * 
 * Handles both fragmented and unfragmented messages with:
 * - Frame sequence validation
 * - UTF-8 checking for text messages
 * - Payload decompression (permessage-deflate)
 * - Callback dispatching
 * 
 * \param payload Pointer to the payload bytes
 * \param payload_len Payload length (may be 0)
 * \param fin FIN flag (1=final fragment)
 * \param opcode Frame type (0x1=text, 0x2=binary, 0x0=continuation)
 * \param rsv1 RSV1 flag (indicates compressed payload)
 * 
 * \note Implements:
 *       - RFC 6455 Sections 5.2-5.7 (Framing)
 *       - RFC 7692 Section 7.2.1 (Compression)
 *       - RFC 3629 (UTF-8 validation)
 * 
 * \warning Enforces strict protocol compliance:
 *          - Validates frame sequencing
 *          - Rejects invalid UTF-8 in text messages
 *          - Verifies decompression success
 * 
 * \warning Thread-safe callback access via mutex
 * 
 * \see validateChunk() for incremental UTF-8 checking
 * \see decompressMessage() for compression handling
 */
void WebSocketClient::handleDataFrame(const unsigned char* payload, size_t payload_len, bool fin, int opcode, bool rsv1) {
    if (message_in_progress) {
        log_error("Received new data frame (opcode %d) while expecting a continuation frame. Closing connection.", opcode);
        close(CloseCode::PROTOCOL_ERROR, "Received new data frame when expecting continuation frame.");
        return;
    }

    const bool compressed = (rsv1 && use_compression && inflate_initialized);

    if (!fin) {
        // Start of a fragmented message
        message_in_progress = true;
        fragmented_opcode = opcode;
        fragmented_message.assign(payload, payload + payload_len);

        compressed_message_in_progress = compressed;

        if (opcode == 0x01 && !compressed_message_in_progress) {
            // Reset DFA for new text message
            utf8Validator.reset();
            // Validate this first chunk
            if (!utf8Validator.validateChunk(payload, payload_len)) {
                log_error("Invalid UTF-8 in initial fragment");
                utf8Validator.reset();
                close(CloseCode::INVALID_PAYLOAD,
                        "Invalid UTF-8 in text message");
                return;
            }
        }
        return;
    }

    // Single unfragmented message
    const uint8_t* msg_data = payload;
    size_t msg_len = payload_len;
    std::vector<uint8_t> decompressed;

    if (compressed) {
        bool ok = decompressMessage(msg_data, msg_len, decompressed);
        if (!ok) {
            close(CloseCode::INVALID_PAYLOAD, "Decompression failed");
            return;
        }
        msg_data = decompressed.data();
        msg_len  = decompressed.size();
    }

    if (opcode == 0x01) {
        // Complete single‐frame message
        utf8Validator.reset();
        if (!utf8Validator.validateChunk(msg_data, msg_len) ||
            !utf8Validator.validateFinal()) {
            log_error("Invalid UTF-8 in unfragmented text");
            utf8Validator.reset();
            close(CloseCode::INVALID_PAYLOAD,
                    "Invalid UTF-8 in text message");
            return;
        }

        std::string message(reinterpret_cast<const char*>(msg_data), msg_len);
        MessageCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = message_callback;
        }
        if (callback) {
            callback(message);
        }
    } else if (opcode == 0x02) {
        BinaryCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = binary_callback;
        }
        if (callback) {
            callback(msg_data, msg_len);
        }
    } else {
        log_error("Unsupported data opcode: %d", opcode);
        close(CloseCode::PROTOCOL_ERROR, "Unsupported opcode");
        return;
    }
}

/**
 * \brief Processes WebSocket CLOSE frames and initiates graceful shutdown.
 * 
 * Handles close frame reception per RFC 6455 by:
 * 
 * 1. Validating close code and reason (UTF-8)
 * 
 * 2. Triggering close callback with status
 * 
 * 3. Sending appropriate CLOSE frame response
 * 
 * \param payload Pointer to the payload bytes
 * \param payload_len Close payload length (0-125 bytes)
 * 
 * \note Implements RFC 6455 sections:
 *       - 5.5.1 (Close frame structure)
 *       - 7.4.1 (Status code ranges)
 *       - 7.1.6 (UTF-8 validation)
 * 
 * \warning Responds with PROTOCOL_ERROR (1002) for:
 *          - Invalid status codes
 *          - Malformed UTF-8 reasons
 *          - Incorrect payload sizes
 * 
 * \warning Thread-safe operations:
 *          - Uses mutex for callback access
 *          - Locks bufferevent for output
 */
void WebSocketClient::handleCloseFrame(const unsigned char* payload, size_t payload_len) {
    //log_debug("Received close frame");
    {
      std::lock_guard<std::mutex> lk(state_mutex);
      got_remote_close = true;
    }
    state_cv.notify_one();

    if (sent_close) return;

    uint16_t close_code = 1000;
    std::string close_reason;
    bool protocol_error = false;

    // RFC 6455 Section 5.5.1
    if (payload_len > 125) {
        log_error("Close frame too large (%zu bytes)", payload_len);
        close_code = 1002;
        protocol_error = true;
    }
    else if (payload_len == 1) {
        log_error("Invalid close frame: payload length is 1 (must be 0 or >=2)");
        close_code = 1002;
        protocol_error = true;
    }
    else if (payload_len >= 2) {
        // Extract close code (avoid unaligned access)
        uint16_t net;
        std::memcpy(&net, payload, 2);
        uint16_t received = ntohs(net);
        close_code = received;

        // RFC 6455 section 7.4.1
        if (!((close_code >= 1000 && close_code <= 1011 &&
            close_code != 1004 && close_code != 1005 && close_code != 1006) ||
            (close_code >= 3000 && close_code <= 4999))) {
            log_error("Received invalid close code: %d", close_code);
            close_code = 1002;
            protocol_error = true;
        }

        // Process close reason if present
        if (payload_len > 2) {
            const char* reason_ptr = reinterpret_cast<const char*>(payload + 2);
            size_t reason_len = payload_len - 2;
            if (reason_len > 123) reason_len = 123;  // RFC limit

            if (!isValidUtf8(reason_ptr, reason_len)) {
                log_error("Close reason is not valid UTF-8");
                close_code = 1002;
                protocol_error = true;
            } else {
                close_reason.assign(reason_ptr, reason_len);
            }
        }
    }

    CloseCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        callback = close_callback;
    }
    if (callback) {
        //callback(close_code, protocol_error ? "Protocol error" : close_reason);
        callback(protocol_error ? 1002 : static_cast<int>(close_code),
                 protocol_error ? "Protocol error" : close_reason);
    }

    const int reply_code = protocol_error ? 1002 : static_cast<int>(close_code);
    const std::string reply_reason = protocol_error ? "" : close_reason;
    close(reply_code, reply_reason);
}

/**
 * \brief Processes incoming WebSocket PING frames and sends PONG response.
 * 
 * Validates PING frame per RFC 6455 (max 125 byte payload) and immediately
 * responds with a PONG containing the same payload. Thread-safely accesses
 * the output buffer using libevent locking.
 * 
 * \param payload Pointer to the ping payload bytes (may be empty)
 * \param payload_len Length of ping payload (must be ≤125 bytes)
 * 
 * \warning Closes connection with PROTOCOL_ERROR if:
 *          - Payload exceeds 125 bytes (RFC violation)
 *          - Output buffer is unavailable
 * 
 * \see RFC 6455 Section 5.5.2 (PING/PONG Control Frames)
 */
void WebSocketClient::handlePingFrame(const unsigned char* payload, size_t payload_len) {
    //log_debug("Received ping frame");
    if (payload_len > 125) {
        log_error("Protocol violation: received ping frame with payload length > 125 bytes");            
        close(CloseCode::PROTOCOL_ERROR, "Control frame payload too large");            
        return;
    }

    if (!m_bev) {
        log_error("Bufferevent is null");
        return;
    }

    bufferevent_lock(m_bev);
    
    evbuffer *output = bufferevent_get_output(m_bev);
    if (output) {
        send(output,
            payload,
            payload_len,
            MessageType::PONG);
    } else {
        log_error("Cannot get output buffer");
    }

    bufferevent_unlock(m_bev);
}

/**
 * \brief Libevent read callback - delegates to handleRead()
 */
void WebSocketClient::readCallback(bufferevent* bev, void* ctx) {
    WebSocketClient* client = static_cast<WebSocketClient*>(ctx);
    client->handleRead(bev);
}

/**
 * \brief Libevent write callback - delegates to handleWrite()
 */
void WebSocketClient::writeCallback(bufferevent* bev, void* ctx) {
    WebSocketClient* client = static_cast<WebSocketClient*>(ctx);
    client->handleWrite(bev);
}

/**
 * \brief Libevent event callback - delegates to handleEvent()
 */
void WebSocketClient::eventCallback(bufferevent* bev, short events, void* ctx) {
    WebSocketClient* client = static_cast<WebSocketClient*>(ctx);
    client->handleEvent(bev, events);
}

/**
 * \brief Timer callback for sending WebSocket PING frames
 */
void WebSocketClient::pingCallback(evutil_socket_t /*fd*/, short /*event*/, void *arg) {
    WebSocketClient* client = static_cast<WebSocketClient*>(arg);
    client->sendPing();
}

/**
 * \brief Connection timeout handler - fails if not connected within timeout window
 */
void WebSocketClient::timeoutCallback(evutil_socket_t /*fd*/, short /*event*/, void *arg) {
    WebSocketClient* client = static_cast<WebSocketClient*>(arg);

    auto state = client->connection_state.load(std::memory_order_acquire);
    if (state != ConnectionState::CONNECTED && state != ConnectionState::FAILED) {
        //log_error("Connection timeout");
        client->sendError(ErrorCode::CONNECT_FAILED, "Connection timeout");
    }
}

/**
 * \brief Sends WebSocket PING frame (empty or with "ping" payload)
 */
void WebSocketClient::sendPing() {
    if (!upgraded.load() || !m_bev) return;
    const char ping_payload[] = "ping";
    sendData(ping_payload,
             sizeof(ping_payload) - 1,
             MessageType::PING);
}

/**
 * \brief Handles incoming data from the WebSocket connection, including upgrade response processing.
 * 
 * This method manages two distinct phases of operation:
 * 
 * Pre-upgrade: Processes HTTP handshake response, validates WebSocket upgrade,
 *    and negotiates compression extensions (permessage-deflate)
 * 
 * Post-upgrade: Delegates WebSocket frame processing to receive()
 *
 * Key responsibilities:
 * Validates HTTP 101 Switching Protocols response
 * Verifies Sec-WebSocket-Accept header
 * Negotiates compression parameters (window bits, context takeover)
 * Manages connection state transitions (thread-safe)
 * Processes any leftover data after upgrade
 * Flushes queued messages post-connection
 * \param bev libevent bufferevent containing received data
 */
void WebSocketClient::handleRead(bufferevent* bev) {
    //log_debug("Read callback");
    evbuffer* input = bufferevent_get_input(bev);

    if (!upgraded.load()) {
        
        const size_t len = evbuffer_get_length(input);
        if (len < 4) return;

        std::vector<char> snap(len);
        evbuffer_copyout(input, snap.data(), len);
        const char* b = snap.data();
            
        // Find end of headers: "\r\n\r\n" (length-bounded)
        size_t headerBytes = 0;
        for (size_t i = 0; i + 3 < len; ++i) {
            if (b[i] == '\r' && b[i+1] == '\n' && b[i+2] == '\r' && b[i+3] == '\n') {
                headerBytes = i + 4;
                break;
            }
        }
        if (headerBytes == 0) return;
        std::string resp(b, headerBytes);

        if (resp.find("HTTP/1.1 101", 0) == std::string::npos ||
            !containsHeader(resp, "Sec-WebSocket-Accept:"))
        {
            log_error("WebSocket upgrade failed");
            connection_state.store(ConnectionState::FAILED, std::memory_order_release);
            sendError(ErrorCode::CONNECT_FAILED, "WebSocket upgrade failed");
            evbuffer_drain(input, len);
            return;
        }

        bool negotiated = false;
        if (compression_requested) {
            std::string lowerResp = resp;
            std::transform(lowerResp.begin(), lowerResp.end(), lowerResp.begin(), ::tolower);
            const std::string key = "sec-websocket-extensions:";
            size_t extHeaderPos = lowerResp.find(key);
            if (extHeaderPos != std::string::npos) {
                size_t lineEnd = resp.find("\r\n", extHeaderPos);
                if (lineEnd == std::string::npos) lineEnd = resp.size();
                std::string extLine = resp.substr(extHeaderPos, lineEnd - extHeaderPos);

                if (containsHeader(extLine, "permessage-deflate")) {
                    negotiated = true;
                    log_debug("Compression negotiated: %s", extLine.c_str());

                    auto hasToken = [](const std::string& s, const char* tok) {
                        std::string ls = s;
                        std::transform(ls.begin(), ls.end(), ls.begin(), ::tolower);
                        return ls.find(tok) != std::string::npos;
                    };
                    auto parseBits = [&](const std::string& keyName) {
                        std::string ls = extLine;
                        std::transform(ls.begin(), ls.end(), ls.begin(), ::tolower);
                        std::string needle = keyName + "=";
                        size_t p = ls.find(needle);
                        if (p == std::string::npos) return 15;
                        size_t vstart = p + needle.size();
                        size_t vend = ls.find_first_of(" ;\r\n", vstart);
                        if (vend == std::string::npos) vend = ls.size();
                        try {
                            int v = std::stoi(ls.substr(vstart, vend - vstart));
                            return (v >= 8 && v <= 15) ? v : 15;
                        } catch (...) { return 15; }
                    };

                    client_no_context_takeover = hasToken(extLine, "client_no_context_takeover");
                    server_no_context_takeover = hasToken(extLine, "server_no_context_takeover");
                    client_max_window_bits = parseBits("client_max_window_bits");
                    server_max_window_bits = parseBits("server_max_window_bits");

                    if (!initializeCompression()) {
                        log_error("Failed to initialize compression");
                        use_compression = false;
                        sendError(ErrorCode::NOT_SUPPORTED, "Compression negotiation failed");
                    } else {
                        use_compression = true;
                    }
                }
            }
        }

        if (!negotiated) {
            log_debug("Compression not negotiated or disabled by user");
            use_compression = false;
        }

        // Drain HTTP headers only (leave any WS frames)
        evbuffer_drain(input, headerBytes);
        upgraded.store(true);
        
        connection_state.store(ConnectionState::CONNECTED, std::memory_order_release);

        // Send Pending Queue
        log_debug("Flushing %zu queued messages…", send_queue.size());
        flushSendQueue();

        OpenCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = open_callback;
        }
        if (callback) {
            callback();
        }

        log_debug("WebSocket connection upgraded successfully");

        if (evbuffer_get_length(input) > 0) {
            //log_debug("Processing leftover frame data after upgrade");
            receive(input);
        }

        return;
    } 
    
    receive(input);
}

/**
 * \brief Write callback
 */
void WebSocketClient::handleWrite(bufferevent* /*bev*/) {
    //log_debug("Write callback");
}

/**
 * \brief Case insensitive header search
 * \param response WebSocket response
 * \param header Header to search for
 * \returns bool
 */
bool WebSocketClient::containsHeader(const std::string& response, const std::string& header) const {
    std::string lowerResponse = response;
    std::string lowerHeader = header;
    
    // Convert both to lowercase for case-insensitive search
    std::transform(lowerResponse.begin(), lowerResponse.end(), lowerResponse.begin(), ::tolower);
    std::transform(lowerHeader.begin(), lowerHeader.end(), lowerHeader.begin(), ::tolower);
    
    return lowerResponse.find(lowerHeader) != std::string::npos;
}

/**
 * \brief Core event handler for WebSocket connection lifecycle events.
 * 
 * This function processes all major connection events from libevent:
 * 
 * 1. BEV_EVENT_CONNECTED:
 *    - For TLS connections: Validates server certificate and hostname
 *    - Initiates WebSocket handshake via sendHandshakeRequest()
 * 
 * 2. BEV_EVENT_ERROR:
 *    - Handles both TLS and non-TLS connection errors
 *    - Logs detailed error information
 *    - Updates connection state and notifies waiting threads
 *    - Triggers error callbacks
 * 
 * 3. BEV_EVENT_EOF:
 *    - Processes graceful connection closure
 *    - Updates state and invokes close callback
 * 
 * \param bev The libevent bufferevent triggering the callback
 * \param events Bitmask of triggered events (BEV_EVENT_* flags)
 * 
 * \note For TLS connections:
 *       - Certificate verification follows RFC 6125 hostname validation
 *       - Uses OpenSSL's built-in hostname checking when enabled
 *       - Peer verification can be disabled via tlsOptions
 * 
 * \warning State changes are thread-safe (protected by mutex)
 * \warning TLS certificate errors are fatal when verification is enabled
 * 
 */
void WebSocketClient::handleEvent(bufferevent* bev, short events) {
    if (events & BEV_EVENT_CONNECTED) {
        log_debug("Connected to server");

        if (secure) {
#ifdef USE_TLS

            SSL* ssl = bufferevent_openssl_get_ssl(bev);
            if (!ssl) {
                sendError(ErrorCode::TLS_INIT_FAILED, "SSL object not found");
                return;
            }

            // Certificate verification
            long verifyResult = SSL_get_verify_result(ssl);
            if (!tlsOptions.isPeerVerifyDisabled()) {
                if (verifyResult != X509_V_OK) {
                    const char* errStr = X509_verify_cert_error_string(verifyResult);
                    sendError(ErrorCode::SSL_HANDSHAKE_FAILED, std::string("TLS certificate error: ") + errStr);
                    return;
                }
                // Hostname validation (if enabled)
                if (!tlsOptions.disableHostnameValidation) {
                    // Hostname check is already set via X509_VERIFY_PARAM_set1_host
                    // So no need to manually check again here
                    log_debug("Hostname verification succeeded (via OpenSSL)");
                } else {
                    log_debug("Hostname verification disabled by config");
                }
            } else {
                log_debug("Peer certificate verification disabled by config");
            }
#endif
        }
        
        // TCP connected, but still need WebSocket handshake
        log_debug("TCP connection established, starting WebSocket handshake");

        sendHandshakeRequest();

    } else if (events & BEV_EVENT_ERROR) {
        
        std::string message;
        ConnectionState new_state = ConnectionState::FAILED;
        if (secure) {
#ifdef USE_TLS
            unsigned long ssl_err = bufferevent_get_openssl_error(bev);
            if (ssl_err) {
                char err_buf[512];
                ERR_error_string_n(ssl_err, err_buf, sizeof(err_buf));
                log_error("TLS error: %.240s", err_buf);
                message = err_buf;
                sendError(ErrorCode::SSL_ERROR, message);
                connection_state.store(new_state, std::memory_order_release);
                return;
            }
#endif
        }
        (void) bev;

        int error_code = EVUTIL_SOCKET_ERROR();
        message = error_code != 0
            ? formatSocketError(error_code)
            : "Connection error";
        log_error("%s", message.c_str());
        sendError(ErrorCode::IO, message);

        connection_state.store(new_state, std::memory_order_release);
        
    } else if (events & BEV_EVENT_EOF) {

        std::string message;
        int close_code = static_cast<int>(CloseCode::NORMAL);
        message = "Connection closed (EOF)";
        close_code = static_cast<int>(CloseCode::NORMAL);
        ConnectionState new_state = ConnectionState::DISCONNECTED;

        log_debug("%s", message.c_str());

        connection_state.store(new_state, std::memory_order_release);

        CloseCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex);
            callback = close_callback;
        }

        if (callback) {
            callback(close_code, message.empty() ? "Connection closed" : message);
        }

    } else {
        log_debug("Event: %d", events);
    }
}

/**
 * \brief Initializes zlib streams for WebSocket permessage-deflate compression.
 * 
 * This function sets up both inflation (decompression) and deflation (compression)
 * streams with the negotiated window sizes. Initialization follows this sequence:
 * 1. Initializes inflate stream with server's max window bits (negative for raw mode)
 * 2. Initializes deflate stream with client's max window bits and compression level
 * 3. Cleans up partially initialized state if either operation fails
 * 
 * \return true if both streams initialized successfully, false on any failure
 * 
 * \note Uses negative window bits (-server_max_window_bits/-client_max_window_bits)
 *       to enable raw deflate/inflate mode without zlib headers
 * \warning Always cleans up inflate stream if deflate initialization fails
 * \warning Compression level must be between 0 (no compression) and 9 (max)
 * 
 * \see RFC 7692 Section 7.1 (https://tools.ietf.org/html/rfc7692#section-7.1)
 * \see inflateInit2()
 * \see deflateInit2()
 */
bool WebSocketClient::initializeCompression() {

    memset(&inflate_stream, 0, sizeof(inflate_stream));
    int ret = inflateInit2(&inflate_stream, -server_max_window_bits);
    if (ret != Z_OK) {
        log_error("Failed to initialize inflate: %d", ret);
        return false;
    }

    inflate_initialized = true;
    
    memset(&deflate_stream, 0, sizeof(deflate_stream));
    ret = deflateInit2(&deflate_stream, compression_level, Z_DEFLATED, -client_max_window_bits, 8, Z_DEFAULT_STRATEGY);

    if (ret != Z_OK) {
        log_error("Failed to initialize deflate: %d", ret);
        if (inflate_initialized) {
            inflateEnd(&inflate_stream);
            inflate_initialized = false;
        }
        return false;
    }

    deflate_initialized = true;
    
    log_debug("Compression initialized successfully");
    return true;
}

/**
 * \brief Enables or disables WebSocket compression (permessage-deflate).
 * 
 * Compression is enabled by default if supported by the WebSocket server.
 * This setting only indicates client preference and may be ignored by the server.
 * 
 * \param enable Set to `true` to request compression, `false` to disable
 */
void WebSocketClient::enableCompression(bool enable) {
    compression_requested = enable;
}

/// \brief Sets additional TLS options
void WebSocketClient::setTLSOptions(const WebSocketTLSOptions& options) {
    tlsOptions = options;
}

/// \brief Sets additional HTTP headers for WebSocket handshake
void WebSocketClient::setHeaders(const WebSocketHeaders& headers) {
    extraHeaders = headers;
}

/// \brief Sets ping interval
void WebSocketClient::setPingInterval(int interval) {
    ping_interval = interval;
}

/// \brief Sets connection timeout
void WebSocketClient::setConnectionTimeout(int timeout) {
    connection_timeout = timeout;
}

#ifdef USE_TLS

/**
 * \brief Retrieves the most recent OpenSSL error message as a string.
 * 
 * This function captures the OpenSSL error stack and formats it into a human-readable string.
 * It handles all memory management internally and guarantees return of a valid error message,
 * even if OpenSSL error retrieval fails.
 * 
 * The implementation:
 * 1. Creates a memory BIO (Basic I/O) to capture error output
 * 2. Uses ERR_print_errors() to format the error stack
 * 3. Extracts the error message from the BIO
 * 4. Provides a fallback message if error retrieval fails
 * 5. Properly cleans up OpenSSL resources
 * 
 * \return Formatted error string containing:
 *         - The OpenSSL error stack if available
 *         - "BIO allocation failed" if BIO creation fails
 *         - "No OpenSSL error message available" if error retrieval fails
 * 
 * \note The returned string always contains a non-empty error description
 * 
 */
std::string WebSocketClient::getOpenSSLError() {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return "BIO allocation failed";
    ERR_print_errors(bio);
    char* buf = nullptr;
    long len = BIO_get_mem_data(bio, &buf);
    std::string err;
    if (len > 0 && buf) {
        err.assign(buf, len);
    } else {
        err = "No OpenSSL error message available";
    }
    BIO_free(bio);
    return err;
}

/**
 * \brief Initializes TLS/SSL context for secure WebSocket connections.
 * 
 * This function performs all necessary OpenSSL initialization and configuration:
 * - Creates a new SSL context with TLS client method
 * - Sets minimum protocol version to TLS 1.2
 * - Disables insecure protocol versions (SSLv2, SSLv3, TLS 1.0, TLS 1.1)
 * - Configures cipher suites based on security preferences
 * - Sets up certificate verification and client certificates
 *
 * For OpenSSL versions < 1.1.0, initializes the legacy SSL library.
 *
 * \return true if TLS initialization succeeded, false on any error
 *
 * \throw No explicit throws, but reports errors via:
 *        - log_error for OpenSSL failures
 *        - sendError() with ErrorCode::TLS_INIT_FAILED
 *
 * \note On failure, cleans up any allocated SSL context
 * 
 */
bool WebSocketClient::initTLS() {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
#endif
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        log_error("SSL_CTX_new() failed");
        sendError(ErrorCode::TLS_INIT_FAILED, "SSL context creation failed: " + getOpenSSLError());
        return false;
    }

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, 
        SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | 
        SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

    if (!configureCiphers()) {
        SSL_CTX_free(ctx);
        ctx = nullptr;
        return false;
    }
    if (!configureCertificates()) {
        SSL_CTX_free(ctx);
        ctx = nullptr;
        return false;
    }

    return true;
}

/**
 * \brief Configures TLS cipher suites for the WebSocket connection.
 * 
 * This function sets up the allowed cipher suites based on the TLS options:
 * - Uses default cipher list if configured in options
 * - Falls back to custom cipher list if specified
 * - Validates the cipher list with OpenSSL before applying
 * 
 * \return true if cipher configuration succeeded, false on failure
 * 
 * \throw No explicit throws, but sends errors via sendError() with:
 *        - ErrorCode::TLS_INIT_FAILED if cipher setup fails
 * 
 * \note The default cipher list should prioritize strong, modern ciphers
 * \warning Weak cipher lists can compromise connection security
 * 
 */
bool WebSocketClient::configureCiphers() {
    const char* cipherList = nullptr;

    if (tlsOptions.isUsingDefaultCiphers()) {
        cipherList = WebSocketTLSOptions::getDefaultCiphers().c_str();
    } else {
        cipherList = tlsOptions.ciphers.c_str();
    }

    if (SSL_CTX_set_cipher_list(ctx, cipherList) != 1) {
        sendError(ErrorCode::TLS_INIT_FAILED, "Cipher setup failed");
        return false;
    }
    return true;
}

/**
 * \brief Configures TLS/SSL certificates and verification settings.
 * 
 * This function handles all certificate-related configuration for the WebSocket connection:
 * - Loads system or custom CA certificates for peer verification
 * - Configures peer verification mode (enabled/disabled)
 * - Loads client certificates and private keys when provided
 * 
 * Behavior depends on TLS options configuration:
 * 1. System CA Mode:
 *    - Uses default system certificate store (SSL_CTX_set_default_verify_paths)
 * 2. Custom CA Mode:
 *    - Loads specified CA certificate file
 *    - Falls back to system CAs if custom load fails (when configured)
 * 3. Client Certificates:
 *    - Loads PEM-format certificate and private key
 *    - Verifies they match each other
 * 
 * \return true if all configurations succeeded, false on any error
 * 
 * \throw No explicit throws, but sends errors via sendError() for:
 *        - Failed CA certificate loading
 *        - Failed client certificate loading
 *        - Private key verification failures
 * 
 * \note Order of operations matters - CA settings are configured before client certs
 * \warning Disabling peer verification (SSL_VERIFY_NONE) reduces security
 * \warning Private key files should be properly protected on the filesystem
 * 
 * \see SSL_CTX_set_verify()
 * \see SSL_CTX_load_verify_locations()
 * \see SSL_CTX_use_certificate_file()
 */
bool WebSocketClient::configureCertificates() {
    if (tlsOptions.isUsingSystemCA()) {
        if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            sendError(ErrorCode::TLS_INIT_FAILED,
                       "System CA load failed: " + getOpenSSLError());
            return false;
        }
        return true;
    }

    // Custom certificate handling
    if (tlsOptions.isPeerVerifyDisabled()) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        
        if (tlsOptions.isUsingCustomCA()) {
            if (!SSL_CTX_load_verify_locations(ctx, 
                tlsOptions.caFile.c_str(), nullptr)) {
                sendError(ErrorCode::TLS_INIT_FAILED,
                           "Custom CA load failed: " + getOpenSSLError());
                return false;
            }
        } else {
            if (!SSL_CTX_set_default_verify_paths(ctx)) {
                sendError(ErrorCode::TLS_INIT_FAILED,
                           "System CA fallback failed: " + getOpenSSLError());
                return false;
            }
        }
    }

    if (tlsOptions.hasCertAndKey()) {
        if (!SSL_CTX_use_certificate_file(ctx, tlsOptions.certFile.c_str(), SSL_FILETYPE_PEM)) {
            sendError(ErrorCode::TLS_INIT_FAILED,
                "Failed to load client certificate: " + getOpenSSLError());
            return false;
        }
        if (!SSL_CTX_use_PrivateKey_file(ctx, tlsOptions.keyFile.c_str(), SSL_FILETYPE_PEM)) {
            sendError(ErrorCode::TLS_INIT_FAILED,
                "Failed to load private key: " + getOpenSSLError());
            return false;
        }
        if (!SSL_CTX_check_private_key(ctx)) {
            sendError(ErrorCode::TLS_INIT_FAILED,
                "Client cert and key mismatch: " + getOpenSSLError());
            return false;
        }
    }

    return true;
}
#endif

/**
 * \brief Format socket error
 * 
 * \param error_code int
 * \return formatted socket error
 */
std::string WebSocketClient::formatSocketError(int error_code) {
    return std::string(evutil_socket_error_to_string(error_code)) + 
           " (system error " + std::to_string(error_code) + ")";
}

/**
 * \brief Validates that a buffer contains well-formed UTF-8.
 *
 * The validation enforces the following checks:
 * 1. ASCII bytes (0x00–0x7F) are accepted immediately.
 * 2. Disallow invalid lead bytes (0xC0–0xC1 and 0xF5–0xFF), which cannot start any valid UTF-8 sequence.
 * 3. Two-byte sequences:
 *    - Lead byte in 0xC2–0xDF.
 *    - Followed by one continuation byte in 0x80–0xBF.
 * 4. Three-byte sequences:
 *    - Lead byte in 0xE0–0xEF, two continuation bytes in 0x80–0xBF.
 *    - Prevent overlong encoding: if lead is 0xE0, next byte must be >= 0xA0.
 *    - Prevent UTF-16 surrogates: if lead is 0xED, next byte must be < 0xA0.
 * 5. Four-byte sequences:
 *    - Lead byte in 0xF0–0xF4, three continuation bytes in 0x80–0xBF.
 *    - Prevent overlong encoding: if lead is 0xF0, next byte must be >= 0x90.
 *    - Prevent code points above U+10FFFF: if lead is 0xF4, next byte must be <= 0x8F.
 *
 * \param str Pointer to the byte sequence to validate.
 * \param len Length of the byte sequence in bytes.
 * \return true if valid UTF-8; false otherwise.
 */
bool WebSocketClient::isValidUtf8(const char* str, size_t len) {
    const unsigned char* bytes = (const unsigned char*)str;
    size_t i = 0;
    
    while (i < len) {
        if (bytes[i] <= 0x7F) {
            i++;
            continue;
        }
        
        if (bytes[i] >= 0xF5 || (bytes[i] >= 0xC0 && bytes[i] <= 0xC1)) {
            return false;
        }
        
        if ((bytes[i] & 0xE0) == 0xC0) {
            if (i + 1 >= len || (bytes[i+1] & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
        }
        else if ((bytes[i] & 0xF0) == 0xE0) {
            if (i + 2 >= len || 
                (bytes[i+1] & 0xC0) != 0x80 || 
                (bytes[i+2] & 0xC0) != 0x80) {
                return false;
            }
            
            if (bytes[i] == 0xE0 && (bytes[i+1] < 0xA0)) {
                return false;
            }
            
            if (bytes[i] == 0xED && (bytes[i+1] >= 0xA0)) {
                return false;
            }
            
            i += 3;
        }
        else if ((bytes[i] & 0xF8) == 0xF0) {
            if (i + 3 >= len || 
                (bytes[i+1] & 0xC0) != 0x80 || 
                (bytes[i+2] & 0xC0) != 0x80 || 
                (bytes[i+3] & 0xC0) != 0x80) {
                return false;
            }
            
            if (bytes[i] == 0xF0 && (bytes[i+1] < 0x90)) {
                return false;
            }
            
            if (bytes[i] == 0xF4 && (bytes[i+1] > 0x8F)) {
                return false;
            }
            
            i += 4;
        }
        else {
            return false;
        }
    }
    
    return true;
}

/**
 * \brief Decompresses a WebSocket message compressed with permessage-deflate.
 *
 * This function ensures the inflate stream is initialized and resets it when
 * no context takeover is configured, preventing use of previous compression state.
 * It removes the standard 0x00 0x00 0xFF 0xFF sync trailer appended by the WebSocket
 * extension before feeding the remaining bytes into the zlib stream. Decompression
 * is performed in a loop using a fixed-size temporary buffer, flushing the inflater
 * with Z_SYNC_FLUSH on each iteration. Decompressed data is collected in the output
 * vector, and the process continues until all input is consumed or the end of the
 * compressed block is reached. Any zlib errors other than Z_OK, Z_BUF_ERROR, or
 * Z_STREAM_END are treated as failures.
 *
 * \param input Pointer to the compressed input bytes.
 * \param input_len Number of compressed bytes available at input.
 * \param output Vector where decompressed bytes will be appended.
 * \return true if decompression succeeds, false on any error.
 */
bool WebSocketClient::decompressMessage(
    const uint8_t* input,
    size_t input_len,
    std::vector<uint8_t>& output
) {
    if (!inflate_initialized) {
        log_error("Decompressor not initialized");
        return false;
    }

    if (server_no_context_takeover) {
        int ret = inflateReset(&inflate_stream);
        if (ret != Z_OK) {
            log_error("inflateReset failed: %d (%s)", ret, zError(ret));
            return false;
        }
    }

    if (input_len >= 4 && 
        input[input_len-4] == 0x00 && 
        input[input_len-3] == 0x00 &&
        input[input_len-2] == 0xFF &&
        input[input_len-1] == 0xFF) {
        input_len -= 4;
    }

    constexpr size_t CHUNK = 65536;
    std::array<uint8_t, CHUNK> temp;

    inflate_stream.next_in = const_cast<Bytef*>(input);
    inflate_stream.avail_in = static_cast<uInt>(input_len);

    output.reserve(output.size() + (input_len * 2));

    int ret;
    do {
        inflate_stream.next_out = temp.data();
        inflate_stream.avail_out = static_cast<uInt>(temp.size());

        ret = inflate(&inflate_stream, Z_SYNC_FLUSH);

        if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END) {
            log_error("inflate failed: %d (%s)", ret, zError(ret));
            return false;
        }

        size_t got = temp.size() - inflate_stream.avail_out;
        if (got > 0) {
            output.insert(output.end(), temp.begin(), temp.begin() + got);
        }

        if (ret == Z_STREAM_END) {
            break;
        }
    } while (inflate_stream.avail_out == 0);

    return true;
}

/**
 * \brief Converts 40-char hex string to 20-byte array (for SHA-1 digests)
 * \param hex 40-character hex string (case-insensitive)
 * \throw std::runtime_error if hex.length() != 40
 */
std::array<uint8_t,20> WebSocketClient::hexToBytes(const std::string &hex) {
    if (hex.size() != 40) throw std::runtime_error("SHA1 hex must be 40 chars");
    std::array<uint8_t,20> out{};
    for (size_t i = 0; i < 20; ++i) {
        uint8_t hi = std::stoi(hex.substr(2*i,1), nullptr, 16);
        uint8_t lo = std::stoi(hex.substr(2*i+1,1), nullptr, 16);
        out[i] = (hi << 4) | lo;
    }
    return out;
}

/**
 * \brief Generates a random WebSocket handshake key for client connections.
 * \return Base64-encoded 16-byte random key suitable for WebSocket handshake
 */
std::string WebSocketClient::getWebSocketKey() {
    std::array<uint8_t,16> nonce;
    std::random_device rd;
    for (auto &b : nonce) b = rd();
    return base64_encode(nonce.data(), nonce.size());
}

/**
 * \brief Compute handshake accepting key
 * 
 * \param key The client-provided WebSocket key (must be non-empty)
 * \return Base64-encoded acceptance key, or empty string on failure
 */
std::string WebSocketClient::computeAccept(const std::string &key) {
    std::string buf = key + WS_MAGIC;
#ifdef USE_TLS
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(buf.data()),
         buf.size(),
         digest);
    return base64_encode(digest, sizeof(digest));
#else
    SHA1 sha;
    sha.update(buf);
    std::string hexDigest = sha.final();
    auto rawDigest = hexToBytes(hexDigest);
    return base64_encode(rawDigest.data(), rawDigest.size());
#endif
}
