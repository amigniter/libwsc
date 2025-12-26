/*
 *  WebSocketContext.cpp
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */
#include "WebSocketContext.h"
#include "Logger.h"
#ifdef USE_TLS
  #include <openssl/ssl.h>
  #include <openssl/sha.h>
  #include <openssl/err.h>
#endif
//#include <sstream>

static std::once_flag g_evthread_once;

WebSocketContext::WebSocketContext(const Config& cfg) : _cfg(cfg), receiver(*this) {
    key = getWebSocketKey();
    accept = computeAccept(key);
}

WebSocketContext::~WebSocketContext() {
    stop();
}

void WebSocketContext::cleanup() {
    if (close_timer) {
        evtimer_del(close_timer);
        event_free(close_timer);
        close_timer = nullptr;
    }

    if (ping_event) {
        event_del(ping_event);
        event_free(ping_event);
        ping_event = nullptr;
    }

    if (timeout_event) {
        event_del(timeout_event);
        event_free(timeout_event);
        timeout_event = nullptr;
    }

    if (_bev) {
        if (_cfg.secure) {
#ifdef USE_TLS
            // Internally handled by libevent
            SSL* ssl = bufferevent_openssl_get_ssl(_bev);
            if (ssl) {
                SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
                SSL_shutdown(ssl);
            }
#endif
        }
        bufferevent_disable(_bev, EV_READ | EV_WRITE);
        bufferevent_setcb(_bev, nullptr, nullptr, nullptr, nullptr);
        bufferevent_free(_bev);
        _bev = nullptr;
    }

    if (dns_base) {
        evdns_base_free(dns_base, 0);
        dns_base = nullptr;
    }

    event* wev = nullptr;
    event* sev = nullptr;
    event_base* b = nullptr;

    {
        std::lock_guard<std::mutex> lk(base_mutex);
        wev = wakeup_event;
        sev = send_event;
        b = base;

        wakeup_event = nullptr;
        send_event = nullptr;
        base = nullptr;
    }

    if (wev) {
        event_del(wev);
        event_free(wev);
    }

    if (sev) {
        event_del(sev);
        event_free(sev);
    }

    if (b) {
        event_base_free(b);
    }
}

void WebSocketContext::setOpenCallback(OpenCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mutex);
    on_open = std::move(cb);
}

void WebSocketContext::setErrorCallback(ErrorCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mutex);
    on_error = std::move(cb);
}

void WebSocketContext::setCloseCallback(CloseCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mutex);
    on_close = std::move(cb);
}

void WebSocketContext::setMessageCallback(MessageCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mutex);
    on_message = std::move(cb);
}

void WebSocketContext::setBinaryCallback(BinaryCallback cb) {
    std::lock_guard<std::mutex> lk(cb_mutex);
    on_binary = std::move(cb);
}

void WebSocketContext::libeventThreads() {
    std::call_once(g_evthread_once, []() {
        evthread_use_pthreads();
    });
}

void WebSocketContext::start() {
    auto self = shared_from_this();

    libeventThreads();

    event_thread = std::thread([self]() {
        self->run();
    });
}

void WebSocketContext::run() {

    event_tid = std::this_thread::get_id();

    /*std::ostringstream oss;
    oss << event_tid;
    log_debug("event thread started, tid=%s", oss.str().c_str());*/

    if (running.load()) {
        log_debug("Already connected or connecting");
        return;
    }

    if (_cfg.host.empty() || _cfg.port <= 0) {
        log_error("setUrl() must be called before connect(): invalid host or port");
        sendError(ErrorCode::CONNECT_FAILED, "Invalid host or port");
        return;
    }

#ifdef USE_TLS
    SSL *ssl = nullptr;
#endif

    if(_cfg.secure) {
#ifdef USE_TLS
        std::string err;
        if (!_tls.init(_cfg.tls, err)) {
            log_error("TLS init failed: %s", err.c_str());
            sendError(ErrorCode::TLS_INIT_FAILED, "Failed to initialize TLS");
            return;
        }

        ssl = _tls.createSsl(err);
        if (!ssl) {
            log_error("TLS SSL_new failed: %s", err.c_str());
            sendError(ErrorCode::TLS_INIT_FAILED, "Failed SSL context creation");
            _tls.reset();
            return;
        }

        if (!_cfg.is_ip_address) {
            SSL_set_tlsext_host_name(ssl, _cfg.host.c_str());
        }

        if (!_cfg.tls.disableHostnameValidation) {
            X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
            if (param) {
                int ret = X509_VERIFY_PARAM_set1_host(param, _cfg.host.c_str(), 0);  // No port matching
                if (ret != 1) {
                    log_error("Failed to set hostname for verification");
                    sendError(ErrorCode::TLS_INIT_FAILED, "Failed hostname verification setup");
                    SSL_free(ssl);
                    _tls.reset();
                    return;
                }
            }
        }
#else 
        log_error("TLS support not compiled in (USE_TLS=OFF), proceeding in insecure mode");
        _cfg.secure = false;
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
        sendError(ErrorCode::IO, "Failed to create DNS base");
        event_base_free(base);
        base = nullptr;
        return;
    }

    if (_cfg.secure) {
#ifdef USE_TLS
        _bev = bufferevent_openssl_socket_new(base, -1, ssl, BUFFEREVENT_SSL_CONNECTING, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
        if (!_bev) {
            log_error("Failed to create secure bufferevent");
            SSL_free(ssl); // because _bev didn't take ownership.
            _tls.reset();
            cleanup();
            return;
        }
#endif
    } else {
        _bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
        if (!_bev) {
            log_error("Failed to create bufferevent");
            sendError(ErrorCode::IO, "Failed to create bufferevent");
            cleanup();
            return;
        }
    }
    
    event* wev = event_new(base, -1, 0, &WebSocketContext::wakeupCallback, this);
    event* sev = event_new(base, -1, EV_PERSIST, &WebSocketContext::sendCallback, this);

    {
        std::lock_guard<std::mutex> lk(base_mutex);
        wakeup_event = wev;
        send_event   = sev;
    }

    if (wev) event_add(wev, nullptr);
    else { log_error("Failed to create wakeup_event"); cleanup(); return; }
    
    if (sev) event_add(sev, nullptr);
    else { log_error("Failed to create send_event"); cleanup(); return; }

    
    struct timeval timeout;
    timeout.tv_sec = _cfg.connection_timeout;
    timeout.tv_usec = 0;
    

    timeout_event = event_new(base, -1, 0, timeoutCallback, this);
    event_add(timeout_event, &timeout);
    

    if (_cfg.ping_interval > 0) {
        struct timeval tv;
	    tv.tv_sec = _cfg.ping_interval;
  	    tv.tv_usec = 0;

        ping_event = event_new(base, -1, EV_PERSIST, pingCallback, this);
        evtimer_add(ping_event, &tv);
    }

    bufferevent_setcb(_bev, &WebSocketContext::readCallback, /*writeCallback*/nullptr, &WebSocketContext::eventCallback, this);

    bufferevent_enable(_bev, EV_READ | EV_WRITE);

    if (bufferevent_socket_connect_hostname(_bev, dns_base, AF_INET, _cfg.host.c_str(), _cfg.port) < 0) {
        log_error("Failed to start connection");
        sendError(ErrorCode::CONNECT_FAILED, "Failed to start connection");
        cleanup();
        return;
    }

    running.store(true, std::memory_order_release);
    event_base_dispatch(base);

    log_debug("event loop exited, proceeding to cleanup()");
    running.store(false, std::memory_order_release);

    cleanup();
}

void WebSocketContext::stop() {
    stop_requested.store(true, std::memory_order_release);

    requestWakeup();

    const bool on_event_thread = (std::this_thread::get_id() == event_tid);

    if (event_thread.joinable()) {
        if (!on_event_thread) {
            event_thread.join();
        } else {
            event_thread.detach();
        }
    }
}

void WebSocketContext::stopNow() {
    auto st = connection_state.load(std::memory_order_acquire);
    if (st == ConnectionState::DISCONNECTING || st == ConnectionState::DISCONNECTED) {
        return;
    }

    const bool can_handshake = upgraded.load(std::memory_order_acquire) && (_bev != nullptr);

    // Only try graceful WS close if we are still connected (not already failing)
    if (!can_handshake || st != ConnectionState::CONNECTED) {
        // Abort, nothing to handshake
        connection_state.store(ConnectionState::DISCONNECTED, std::memory_order_release);
        requestLoopExit();
        return;
    }

    // Graceful WS close
    close();
}

void WebSocketContext::timeoutCallback(evutil_socket_t /*fd*/, short /*event*/, void *arg) {
    auto* self = static_cast<WebSocketContext*>(arg);
    log_debug("timeoutCallback entered");
    
    // If we're already closing/closed, ignore.
    const auto st = self->connection_state.load(std::memory_order_acquire);
    if (st == ConnectionState::DISCONNECTING || st == ConnectionState::DISCONNECTED) {
        return;
    }

    // Notify user
    self->sendError(ErrorCode::TIMEOUT, "Timeout");

    // Connect/handshake timeout -> abort shutdown (no WS CLOSE possible)
    self->requestLoopExit();
}

void WebSocketContext::pingCallback(evutil_socket_t /*fd*/, short /*event*/, void *arg) {
    auto* self = static_cast<WebSocketContext*>(arg);
    self->sendPing();
}

void WebSocketContext::wakeupCallback(evutil_socket_t, short, void* arg) {
    auto* self = static_cast<WebSocketContext*>(arg);
    if (!self->base) return;

    // If shutdown was requested, initiate shutdown logic ONCE.
    if (self->stop_requested.load(std::memory_order_acquire)) {
        self->stopNow();     // does NOT necessarily exit loop immediately
        return;              // do NOT flush app data on shutdown request
    }

    self->flushSendQueue();
}

void WebSocketContext::closeTimerCb(evutil_socket_t, short, void* arg) {
    auto* self = static_cast<WebSocketContext*>(arg);
    // If peer already replied with CLOSE, nothing to do.
    if (self->close_received) {
        return;
    }

    // Close handshake didn't complete -> treat as closed anyway
    self->sendCloseCallback(1000, "Normal closure");

    self->connection_state.store(ConnectionState::DISCONNECTED, std::memory_order_release);

    // Peer did not complete close handshake in time -> force shutdown.
    self->requestLoopExit();
}

void WebSocketContext::sendCallback(evutil_socket_t /*fd*/, short /*events*/, void* arg)
{
    auto* self = static_cast<WebSocketContext*>(arg);
    if (!self || !self->base) return;

    self->flushSendQueue();
    // After flushing mark the flag
    self->send_flush_pending.store(false, std::memory_order_release);
}

void WebSocketContext::eventCallback(bufferevent* bev, short events, void* ctx) {
    auto* self = static_cast<WebSocketContext*>(ctx);
    self->handleEvent(bev, events);
}

void WebSocketContext::readCallback(bufferevent* bev, void* ctx) {
    auto* self = static_cast<WebSocketContext*>(ctx);
    self->handleRead(bev);
}

inline void WebSocketContext::requestWakeup() {
    event* ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(base_mutex);
        ev = wakeup_event;
    }
    if (ev) event_active(ev, 0, 0);
}

inline void WebSocketContext::requestLoopExit() {
    stop_requested.store(true, std::memory_order_release);

    // If we're already on the event thread, exit the loop immediately.
    if (std::this_thread::get_id() == event_tid && base) {
        timeval tv{0, 0};
        event_base_loopexit(base, &tv);
        return;
    }

    // Otherwise, wake the event thread so it can call loopexit.
    requestWakeup();
}

inline void WebSocketContext::requestSendFlush()
{
    // if already scheduled, don't schedule again
    if (send_flush_pending.exchange(true, std::memory_order_acq_rel))
        return;

    event* ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(base_mutex);
        ev = send_event;
    }
    if (ev) {
        event_active(ev, 0, 0);
    } else {
        // couldn't schedule, allow future attempts
        send_flush_pending.store(false, std::memory_order_release);
    }
}

void WebSocketContext::armCloseTimer() {
    // event-thread only
    if (!close_timer) {
        close_timer = evtimer_new(base, &WebSocketContext::closeTimerCb, this);
    }
    timeval tv{1, 0}; // 1 second
    evtimer_add(close_timer, &tv);
}

void WebSocketContext::handleEvent(bufferevent* bev, short events) {
    /*
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    log_debug("handleEvent tid=%s", oss.str().c_str());
    log_debug("handleEvent events=0x%hx", events);
    log_debug("flags: EOF=0x%hx ERROR=0x%hx TIMEOUT=0x%hx CONNECTED=0x%hx READING=0x%hx WRITING=0x%hx",
          (short)BEV_EVENT_EOF, (short)BEV_EVENT_ERROR, (short)BEV_EVENT_TIMEOUT,
          (short)BEV_EVENT_CONNECTED, (short)BEV_EVENT_READING, (short)BEV_EVENT_WRITING);
    */
    (void) bev;
    
    if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF | BEV_EVENT_TIMEOUT)) {

        const bool ws_open = upgraded.load(std::memory_order_acquire);
        const bool graceful = close_received || close_sent;
        const auto st = connection_state.load(std::memory_order_acquire);
        const bool stopping_now = stop_requested.load(std::memory_order_acquire) || st == ConnectionState::DISCONNECTING || st == ConnectionState::DISCONNECTED;

        if (stopping_now && (events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT))) {
            log_debug("handleEvent: ignoring 0x%hx during shutdown", events);
            
            requestLoopExit();
            
            return;
        }

        // --- ERROR ---
        if (events & BEV_EVENT_ERROR) {
            if (_cfg.secure) {
#ifdef USE_TLS
                unsigned long ssl_err = bufferevent_get_openssl_error(_bev);
                if (ssl_err) {
                    char err_buf[512];
                    ERR_error_string_n(ssl_err, err_buf, sizeof(err_buf));
                    log_error("TLS error: %.240s", err_buf);
                    sendError(ErrorCode::SSL_ERROR, std::string("TLS error: ") + err_buf);

                    // mark transport so stopNow() won't try to send CLOSE
                    connection_state.store(ConnectionState::DISCONNECTING, std::memory_order_release);

                    requestLoopExit();
                    return;
                }
#endif
            }

            const int err = EVUTIL_SOCKET_ERROR();
            //std::string msg = (err != 0) ? formatSocketError(err) : "Connection error";
            std::string msg;
            if (err != 0) {
                msg = std::string(evutil_socket_error_to_string(err)) + " (system error " + std::to_string(err) + ")";
            } else {
                msg = "Connection error";
            }
            log_error("%s", msg.c_str());

            // During handshake, classify as connect failure; after upgrade it's IO.
            sendError(ws_open ? ErrorCode::IO : ErrorCode::CONNECT_FAILED, msg);
            
            if (!ws_open) {
                std::lock_guard<std::mutex> lk(send_queue_mutex);
                send_queue.clear();
            }

            if (ws_open && st == ConnectionState::DISCONNECTING) {
                sendCloseCallback(1000, "Normal closure");
            }

            // mark transport so stopNow() won't try to send CLOSE
            connection_state.store(ConnectionState::DISCONNECTING, std::memory_order_release);

            requestLoopExit();
            return;
        }

        // --- TIMEOUT ---
        if (events & BEV_EVENT_TIMEOUT) {
            if (!ws_open) {
                log_error("Handshake/connect timeout");
                sendError(ErrorCode::TIMEOUT, "Connection/handshake timeout");

                {
                    std::lock_guard<std::mutex> lk(send_queue_mutex);
                    send_queue.clear();
                }

            } else {
                log_error("Connection timeout");
                sendError(ErrorCode::TIMEOUT, "Connection timeout");
            }

            connection_state.store(ConnectionState::DISCONNECTING, std::memory_order_release);

            requestLoopExit();
            return;
        }

        // --- EOF ---
        if (events & BEV_EVENT_EOF) {
            if (!ws_open) {
                // TCP closed before upgrade completed
                log_debug("EOF during handshake");
                sendError(ErrorCode::CONNECT_FAILED, "Connection closed during handshake (EOF)");

                {
                    std::lock_guard<std::mutex> lk(send_queue_mutex);
                    send_queue.clear();
                }

            } else if (!graceful) {
                // WebSocket was open but peer dropped TCP without CLOSE handshake
                log_error("Abnormal closure: EOF without WebSocket CLOSE");
                sendError(ErrorCode::IO, "Connection closed without WebSocket CLOSE (abnormal EOF)");

                connection_state.store(ConnectionState::DISCONNECTING, std::memory_order_release);
            } else {
                // CLOSE handshake likely happened; treat as expected transport shutdown
                log_debug("EOF after close handshake (normal shutdown)");
                sendCloseCallback(1000, "Normal closure");
            }

            requestLoopExit();
            return;
        }
    }

    if (events & BEV_EVENT_CONNECTED) {
        log_debug("TCP connection established");

        if (_cfg.secure) {
#ifdef USE_TLS
            SSL* ssl = bufferevent_openssl_get_ssl(bev);
            if (!ssl) {
                sendError(ErrorCode::TLS_INIT_FAILED, "SSL object not found");
                requestLoopExit();
                return;
            }
            // Certificate verification
            long verifyResult = SSL_get_verify_result(ssl);
            if (!_cfg.tls.isPeerVerifyDisabled()) {
                if (verifyResult != X509_V_OK) {
                    const char* errStr = X509_verify_cert_error_string(verifyResult);
                    sendError(ErrorCode::SSL_HANDSHAKE_FAILED, std::string("TLS certificate error: ") + errStr);
                    requestLoopExit();
                    return;
                }
                
                if (!_cfg.tls.disableHostnameValidation) {
                    // Hostname check is already set via X509_VERIFY_PARAM_set1_host
                    log_debug("Hostname verification succeeded (via OpenSSL)");
                } else {
                    log_debug("Hostname verification disabled by config");
                }
            } else {
                log_debug("Peer certificate verification disabled by config");
            }
#endif            
        }

        sendHandshakeRequest();
        return;
    }
}

void WebSocketContext::handleRead(bufferevent* bev) {
    auto input = bufferevent_get_input(bev);

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

        //log_debug("RESP: %s", resp.c_str());

        if (resp.find("HTTP/1.1 101", 0) == std::string::npos ||
            !containsHeader(resp, "Sec-WebSocket-Accept:"))
        {
            log_error("WebSocket upgrade failed");
            connection_state.store(ConnectionState::FAILED, std::memory_order_release);
            sendError(ErrorCode::CONNECT_FAILED, "WebSocket upgrade failed");
            evbuffer_drain(input, len);
            requestLoopExit();
            return;
        }

        bool negotiated = false;
        
        if (_cfg.compression_requested) {
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

                    PerMessageDeflateConfig cfg;
                    cfg.enabled = true;
                    cfg.client_no_context_takeover = client_no_context_takeover;
                    cfg.server_no_context_takeover = server_no_context_takeover;
                    cfg.client_max_window_bits = client_max_window_bits;
                    cfg.server_max_window_bits = server_max_window_bits;
                    cfg.compression_level = compression_level;

                    if (!receiver.initializeCompression(cfg)) {
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

        OpenCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex);
            cb = on_open;
        }
        if (cb) {
            cb();
        }

        log_debug("WebSocket connection upgraded successfully");

        if (timeout_event) {
            event_del(timeout_event);
            event_free(timeout_event);
            timeout_event = nullptr;
        }

        if (evbuffer_get_length(input) > 0) {
            //log_debug("Processing leftover frame data after upgrade");
            receiver.onData(input);
        }

        return;
    }

    // now the magic
    receiver.onData(input);
}

void WebSocketContext::flushSendQueue() {
    std::deque<Pending> local;
    {
        std::lock_guard<std::mutex> lk(send_queue_mutex);
        local.swap(send_queue);
    }

    const auto st = connection_state.load(std::memory_order_acquire);
    const bool can_send_app = (st == ConnectionState::CONNECTED);

    for (auto& p : local) {

        if (p.type == Pending::Text) {
            if (!can_send_app) continue;
            sendNow(p.text.data(), p.text.size(), MessageType::TEXT);
            continue;
        }
        
        if (p.type == Pending::Binary) {
            if (!can_send_app) continue;
            sendNow(p.bin.data(), p.bin.size(), MessageType::BINARY);
            continue;
        }
        
        if (p.type == Pending::Close) {

            if (close_sent) continue;

            if (sendNow(p.bin.data(), p.bin.size(), MessageType::CLOSE)) {

                close_sent = true;
                armCloseTimer();

            }

            continue;
        }
    }
}

void WebSocketContext::sendHandshakeRequest() {
    if (!_bev) return;
    log_debug("Sending WebSocket handshake request");

    auto out = bufferevent_get_output(_bev);

    evbuffer_add_printf(out, "GET %s HTTP/1.1\r\n", _cfg.uri.c_str());
    evbuffer_add_printf(out, "Host:%s:%d\r\n", _cfg.host.c_str(), _cfg.port);
    evbuffer_add_printf(out, "Upgrade:websocket\r\n");
    evbuffer_add_printf(out, "Connection:upgrade\r\n");
    evbuffer_add_printf(out, "Sec-WebSocket-Key:%s\r\n", key.c_str());
    evbuffer_add_printf(out, "Sec-WebSocket-Version:13\r\n");
    
    if (_cfg.compression_requested) {
        evbuffer_add_printf(out, "Sec-WebSocket-Extensions:permessage-deflate; client_no_context_takeover; server_no_context_takeover; client_max_window_bits=9\r\n");
    }

    evbuffer_add_printf(out, "Origin:http://%s:%d\r\n", _cfg.host.c_str(), _cfg.port);
    
    if (!_cfg.headers.headers.empty()) {
        for (const auto& header : _cfg.headers.headers) {
            evbuffer_add_printf(out, "%s:%s\r\n", header.first.c_str(), header.second.c_str());
        }
    }

    evbuffer_add_printf(out, "\r\n");

}

void WebSocketContext::sendError(int error_code, const std::string& error_message) {
    ErrorCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex);
        cb = on_error;
    }
    if (cb) {
        cb(error_code, error_message);
    } else {
        log_error("Unhandled error: %s", error_message.c_str());
    }
}

void WebSocketContext::sendError(ErrorCode code, const std::string& message) {
    sendError(static_cast<int>(code), message);
}

bool WebSocketContext::close(int code, const std::string& reason) {
    // If we're already closing/closed, do nothing.
    auto st = connection_state.load(std::memory_order_acquire);
    if (st == ConnectionState::DISCONNECTING || st == ConnectionState::DISCONNECTED) {
        return false;
    }
    
    // If we are not upgraded or have no bev, this is an ABORT (connect timeout, handshake fail).
    // No WS CLOSE frame can be sent, so we exit the loop and cleanup.
    if (!upgraded.load(std::memory_order_acquire) || !_bev) {
        log_debug("close(): abort (not upgraded or no bev)");
        connection_state.store(ConnectionState::DISCONNECTED, std::memory_order_release);
        requestLoopExit();
        return true;
    }

    connection_state.store(ConnectionState::DISCONNECTING, std::memory_order_release);

    // If we already sent CLOSE, nothing else to do.
    if (close_sent) {
        return false;
    }

    uint16_t code_be = htons(static_cast<uint16_t>(code));
    std::string r = reason;
    if (r.size() > 123) r.resize(123);

    std::vector<uint8_t> payload(sizeof(code_be) + r.size());
    memcpy(payload.data(), &code_be, sizeof(code_be));
    if (!r.empty()) {
        memcpy(payload.data() + sizeof(code_be), r.data(), r.size());
    }

    {
        std::lock_guard<std::mutex> lk(send_queue_mutex);
        if (send_queue.size() >= MAX_QUEUE_SIZE) {
            log_error("Send queue full—dropping CLOSE");
            requestLoopExit();
            return false;
        }
        send_queue.emplace_back(std::move(payload), Pending::Close);
    }
    
    requestSendFlush();
    return true;
}

bool WebSocketContext::close(CloseCode code, const std::string& reason) {
    return close(static_cast<int>(code), reason);
}

bool WebSocketContext::sendData(const void* data, size_t length, MessageType type) {
    ConnectionState state = connection_state.load(std::memory_order_acquire);

    if (type == MessageType::CLOSE) return false;
    
    // While CONNECTING: queue only
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

    // After CONNECTING:
    // Only event thread sends.
    if (std::this_thread::get_id() == event_tid) {
        return sendNow(data, length, type);
    }

    // Not event thread: queue and poke event loop (send_event)
    {
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
    }

    requestSendFlush();

    return true;
}

bool WebSocketContext::sendNow(const void* data, size_t length, MessageType type) {
    if (!_bev) {
        log_error("sendNow: No bufferevent—cannot send");
        return false;
    }

    ConnectionState state = connection_state.load(std::memory_order_acquire);

    // Allow sending:
    // Normal frames only in CONNECTED
    // Close allowed also in DISCONNECTING
    const bool state_ok = (state == ConnectionState::CONNECTED) || (type == MessageType::CLOSE && state == ConnectionState::DISCONNECTING);

    if (!state_ok) {
        log_error("sendNow: Cannot send in state %d", int(state));
        return false;
    }

    // Only require upgrade for non-close frames
    if (type != MessageType::CLOSE && !upgraded.load(std::memory_order_acquire)) {
        log_error("sendNow: WebSocket not fully upgraded yet");
        return false;
    }

    evbuffer* output = bufferevent_get_output(_bev);
    if (!output) {
        log_error("sendNow: No output buffer");
        return false;
    }

    send(output, data, length, type);

    return true;
}

void WebSocketContext::send(evbuffer* buf, const void* raw_data, size_t raw_len, MessageType type) {
    const bool is_control_frame = (type == MessageType::CLOSE || type == MessageType::PING  || type == MessageType::PONG);

    if (is_control_frame && raw_len > 125) {
        log_error("Control frame too large (%zu bytes)", raw_len);
        return;
    }

    const uint8_t* original_ptr = static_cast<const uint8_t*>(raw_data);
    const size_t   original_len = raw_len;

    const bool request_compress = !is_control_frame && use_compression && (type == MessageType::TEXT || type == MessageType::BINARY);

    const uint8_t* payload_ptr = original_ptr;
    size_t payload_len = original_len;
    bool do_compress = false;

    receiver.txPrepare(original_ptr, original_len, request_compress, payload_ptr, payload_len, do_compress);

    uint8_t b1 = 0x80; // FIN
    if (do_compress) b1 |= 0x40; // RSV1

    switch (type) {
        case MessageType::TEXT:   b1 |= 0x01; break;
        case MessageType::BINARY: b1 |= 0x02; break;
        case MessageType::CLOSE:  b1 |= 0x08; break;
        case MessageType::PING:   b1 |= 0x09; break;
        case MessageType::PONG:   b1 |= 0x0A; break;
    }

    uint8_t b2 = 0x80; // Mask bit
    if (payload_len <= 125) {
        b2 |= static_cast<uint8_t>(payload_len);
    } else if (payload_len <= 65535) {
        b2 |= 126;
    } else {
        b2 |= 127;
    }

    log_debug("send frame: b1=0x%02X b2=0x%02X len=%zu compress=%d\n",
              b1, b2, payload_len, do_compress);

    auto out = buf;

    evbuffer_add(out, &b1, 1);
    evbuffer_add(out, &b2, 1);

    if ((b2 & 0x7F) == 126) {
        uint16_t len = htons(static_cast<uint16_t>(payload_len));
        evbuffer_add(out, &len, 2);
    } else if ((b2 & 0x7F) == 127) {
        uint64_t len = htonll(static_cast<uint64_t>(payload_len));
        evbuffer_add(out, &len, 8);
    }

    // ---- Fast masking (single evbuffer_add) ----
    uint8_t mask_key[4];

    thread_local uint32_t s = 0;
    if (s == 0) {
        uint64_t t = static_cast<uint64_t>(time(nullptr));
        uintptr_t a = reinterpret_cast<uintptr_t>(&s);
        s = static_cast<uint32_t>((t ^ (t >> 32) ^ a) | 1u);
    }

    auto next_u32 = [&]() -> uint32_t {
        s += 0x9E3779B9u;
        uint32_t z = s;
        z ^= z >> 16;
        z *= 0x85EBCA6Bu;
        z ^= z >> 13;
        z *= 0xC2B2AE35u;
        z ^= z >> 16;
        return z;
    };

    uint32_t mask32 = next_u32();
    std::memcpy(mask_key, &mask32, 4);

    // Write mask key
    evbuffer_add(out, mask_key, 4);

    // Mask payload into one contiguous buffer, then add once
    static thread_local std::vector<uint8_t> masked;
    masked.resize(payload_len);

    const uint8_t* src = payload_ptr;
    for (size_t i = 0; i < payload_len; ++i) {
        masked[i] = src[i] ^ mask_key[i & 3];
    }

    // Add
    evbuffer_add(out, masked.data(), masked.size());
}

void WebSocketContext::sendPing() {
    if (!upgraded.load() || !_bev) return;
    const char ping_payload[] = "ping";
    sendNow(ping_payload, sizeof(ping_payload) - 1, MessageType::PING);
}

void WebSocketContext::sendCloseCallback(int code, const std::string& reason) {
    if (close_cb_fired.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    CloseCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex);
        cb = on_close;
    }
    if (cb) cb(code, reason);
}

bool WebSocketContext::containsHeader(const std::string& response, const std::string& header) const {
    std::string lowerResponse = response;
    std::string lowerHeader = header;
    
    // Convert both to lowercase for case insensitive search
    std::transform(lowerResponse.begin(), lowerResponse.end(), lowerResponse.begin(), ::tolower);
    std::transform(lowerHeader.begin(), lowerHeader.end(), lowerHeader.begin(), ::tolower);
    
    return lowerResponse.find(lowerHeader) != std::string::npos;
}

static inline uint8_t hexNibble(char c) {
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
    throw std::runtime_error("Invalid hex");
}

std::array<uint8_t,20> WebSocketContext::hexToBytes(const std::string& hex) {
    if (hex.size() != 40) throw std::runtime_error("SHA1 hex must be 40 chars");
    std::array<uint8_t,20> out{};
    for (size_t i = 0; i < 20; ++i) {
        out[i] = (hexNibble(hex[2*i]) << 4) | hexNibble(hex[2*i + 1]);
    }
    return out;
}

std::string WebSocketContext::getWebSocketKey() {
    std::array<uint8_t,16> nonce;
    std::random_device rd;
    for (auto &b : nonce) b = rd();
    return base64_encode(nonce.data(), nonce.size());
}


std::string WebSocketContext::computeAccept(const std::string &key) {
    std::string WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
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

bool WebSocketContext::isConnected() const {
    auto current_state = connection_state.load(std::memory_order_acquire);
    return current_state == ConnectionState::CONNECTING || current_state == ConnectionState::CONNECTED;
}

// IWebSocketSinks impl in WebSocketContext

bool WebSocketContext::rxCompressionEnabled() const {
    return use_compression;
}

void WebSocketContext::onRxPong(std::vector<uint8_t>&& payload) {
    log_debug("Received pong frame (%zu bytes)", payload.size());
    (void)payload;
}

void WebSocketContext::onRxPing(std::vector<uint8_t>&& payload) {
    // Context is the only sender.
    if (!_bev) return;

    evbuffer* output = bufferevent_get_output(_bev);
    if (!output) return;

    send(output, payload.data(), payload.size(), MessageType::PONG);
}

void WebSocketContext::onRxClose(uint16_t code, std::string&& reason)
{
    close_received = true;

    if (!close_sent) {

        close(static_cast<int>(code), reason);
        
        if (std::this_thread::get_id() == event_tid) {
            flushSendQueue();
        }
    }

    sendCloseCallback(static_cast<int>(code), reason);
}

void WebSocketContext::onRxProtocolError(uint16_t closeCode, std::string&& why) {
    // One-shot guard
    if (protocol_failed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::string msg;
    if (closeCode == 1007) {
        msg = "WebSocket invalid UTF-8 (1007): " + why;
    } else {
        msg = "WebSocket protocol error (" + std::to_string(closeCode) + "): " + why;
    }

    sendError(ErrorCode::PROTOCOL, msg);

    // If we can send a WS close, otherwise abort.
    const bool can_handshake = upgraded.load(std::memory_order_acquire) && (_bev != nullptr);

    if (can_handshake) {
        close(static_cast<int>(closeCode), why);   // queues Pending::Close
        requestSendFlush();
        // loop exit happens on rx close / close timer / transport error
    } else {
        requestLoopExit();
    }
}

void WebSocketContext::onRxText(std::string&& msg) {
    MessageCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex);
        cb = on_message;
    }
    if (cb) cb(msg);
}

void WebSocketContext::onRxBinary(std::vector<uint8_t>&& msg) {
    BinaryCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex);
        cb = on_binary;
    }
    if (cb) cb(msg.data(), msg.size());
}

bool WebSocketContext::rxIsTerminating() const {
    const auto st = connection_state.load(std::memory_order_acquire);
    return st == ConnectionState::DISCONNECTING || st == ConnectionState::DISCONNECTED || stop_requested.load(std::memory_order_acquire);
}