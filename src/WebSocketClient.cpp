/*
 *  WebSocketClient.cpp
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#include "WebSocketClient.h"
#include "WebSocketContext.h"

#include <arpa/inet.h>
#include <iomanip>

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    disconnect();
}

bool WebSocketClient::isConnected() {
    if (_ctx) {
        return _ctx->isConnected();
    }
    return false;
}

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
            return;
        }
    } else {
        host = hostport;
        port = secure ? 443 : 80;
    }

    if (host.empty()) {
        return;
    }

    uri = (path_pos == std::string::npos) ? "/" : url.substr(path_pos);

    is_ip_address = isHostIPAddress(host);
}

bool WebSocketClient::isHostIPAddress(const std::string& host) {
    struct in_addr addr4;
    if (inet_pton(AF_INET, host.c_str(), &addr4) == 1) {
        return true;
    }
    
    struct in6_addr addr6;
    std::string host_clean = host;
    
    if (host.size() >= 2 && host[0] == '[' && host[host.size()-1] == ']') {
        host_clean = host.substr(1, host.size() - 2);
    }
    
    if (inet_pton(AF_INET6, host_clean.c_str(), &addr6) == 1) {
        return true;
    }
    
    // If it's not a valid IP address, it's a domain name
    return false;
}

void WebSocketClient::setHeaders(const WebSocketHeaders& headers) {
    extra_headers = headers;
}

void WebSocketClient::setTLSOptions(const WebSocketTLSOptions& options) {
    tls_options = options;
}

void WebSocketClient::setPingInterval(int interval) {
    ping_interval = interval;
}

void WebSocketClient::setConnectionTimeout(int timeout) {
    connection_timeout = timeout;
}

void WebSocketClient::enableCompression(bool enable) {
    compression_requested = enable;
}

void WebSocketClient::setOpenCallback(OpenCallback callback) {
    open_callback = std::move(callback);
    if (_ctx) _ctx->setOpenCallback(open_callback);
}

void WebSocketClient::setCloseCallback(CloseCallback callback) {
    close_callback = std::move(callback);
    if (_ctx) _ctx->setCloseCallback(close_callback);
}

void WebSocketClient::setErrorCallback(ErrorCallback callback) {
    error_callback = std::move(callback);
    if (_ctx) _ctx->setErrorCallback(error_callback);
}

void WebSocketClient::setMessageCallback(MessageCallback callback) {
    message_callback = std::move(callback);
    if (_ctx) _ctx->setMessageCallback(message_callback);
}

void WebSocketClient::setBinaryCallback(BinaryCallback callback) {
    binary_callback = std::move(callback);
    if (_ctx) _ctx->setBinaryCallback(binary_callback);
}

bool WebSocketClient::sendMessage(const std::string& message) {
    return _ctx && _ctx->sendData(message.data(), message.size(), MessageType::TEXT);
}

bool WebSocketClient::sendMessage(const char* msg, size_t len) {
    return _ctx && _ctx->sendData(msg, len, MessageType::TEXT);
}

bool WebSocketClient::sendBinary(const void* data, size_t length) {
    return _ctx && _ctx->sendData(data, length, MessageType::BINARY);
}

void WebSocketClient::connect() {
    if (_ctx) {
        return;
    }

    WebSocketContext::Config cfg;
    cfg.host = host;
    cfg.port = port;
    cfg.uri = uri;
    cfg.secure = secure;
    cfg.is_ip_address = is_ip_address;
    cfg.ping_interval = ping_interval;
    cfg.connection_timeout = connection_timeout;
    cfg.headers = extra_headers;
    cfg.tls = tls_options;
    cfg.compression_requested = compression_requested;

    try {
        auto ctx = std::make_shared<WebSocketContext>(cfg);
        if (open_callback) ctx->setOpenCallback(open_callback);
        if (close_callback) ctx->setCloseCallback(close_callback);
        if (error_callback) ctx->setErrorCallback(error_callback);
        if (message_callback) ctx->setMessageCallback(message_callback);
        if (binary_callback) ctx->setBinaryCallback(binary_callback);

        _ctx = ctx;
        _ctx->start();

    } catch (...) {
        // Failed to create or start context.
        // Client remains disconnected; user may retry connect().
    }
}

void WebSocketClient::disconnect() {
    if (_ctx) {
        _ctx->stop();
        _ctx.reset();
    }
}
