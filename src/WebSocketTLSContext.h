/*
 *  WebSocketTLSContext.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#pragma once

#include <string>
#include "WebSocketTLSOptions.h"

// Forward-declare
struct ssl_ctx_st;
struct ssl_st;
using SSL_CTX = ssl_ctx_st;
using SSL     = ssl_st;

class WebSocketTLSContext {
public:
    WebSocketTLSContext() = default;
    ~WebSocketTLSContext();

    WebSocketTLSContext(const WebSocketTLSContext&) = delete;
    WebSocketTLSContext& operator=(const WebSocketTLSContext&) = delete;

    WebSocketTLSContext(WebSocketTLSContext&& other) noexcept;
    WebSocketTLSContext& operator=(WebSocketTLSContext&& other) noexcept;

    bool init(const WebSocketTLSOptions& opt, std::string& err);
    void reset() noexcept;
    SSL_CTX* get() const noexcept { return _ctx; }
    bool isInitialized() const noexcept { return _ctx != nullptr; }
    SSL* createSsl(std::string& err) const;

private:
    SSL_CTX* _ctx {nullptr};
};
