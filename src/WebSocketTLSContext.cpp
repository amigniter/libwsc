/*
 *  WebSocketTLSContext.cpp
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#include "WebSocketTLSContext.h"
#include <openssl/ssl.h>
#include <openssl/sha.h>
#include <openssl/err.h>

WebSocketTLSContext::~WebSocketTLSContext() {
    reset();
}

WebSocketTLSContext::WebSocketTLSContext(WebSocketTLSContext&& other) noexcept
    : _ctx(other._ctx) {
    other._ctx = nullptr;
}

WebSocketTLSContext& WebSocketTLSContext::operator=(WebSocketTLSContext&& other) noexcept {
    if (this != &other) {
        reset();
        _ctx = other._ctx;
        other._ctx = nullptr;
    }
    return *this;
}

void WebSocketTLSContext::reset() noexcept {
    if (_ctx) {
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
    }
}

static std::string opensslLastErrorString() {
    unsigned long e = ERR_get_error();
    if (!e) return {};
    char buf[256]{};
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string(buf);
}

bool WebSocketTLSContext::init(const WebSocketTLSOptions& opt, std::string& err) {
    err.clear();
    reset();

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        err = "SSL_CTX_new failed: " + opensslLastErrorString();
        return false;
    }

    SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    // Ciphers: either default list or user-provided.
    const std::string& cipherStr = opt.isUsingDefaultCiphers()
        ? WebSocketTLSOptions::getDefaultCiphers()
        : opt.ciphers;

    if (!cipherStr.empty()) {
        if (SSL_CTX_set_cipher_list(ctx, cipherStr.c_str()) != 1) {
            err = "SSL_CTX_set_cipher_list failed: " + opensslLastErrorString();
            SSL_CTX_free(ctx);
            return false;
        }
    }

    // Verification / CA handling.
    if (opt.isPeerVerifyDisabled()) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

        if (opt.isUsingSystemCA()) {
            // Uses default OS trust store locations (if OpenSSL is built with them).
            if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
                err = "SSL_CTX_set_default_verify_paths failed: " + opensslLastErrorString();
                SSL_CTX_free(ctx);
                return false;
            }
        } else if (opt.isUsingCustomCA()) {
            if (SSL_CTX_load_verify_locations(ctx, opt.caFile.c_str(), nullptr) != 1) {
                err = "SSL_CTX_load_verify_locations failed for CA file '" + opt.caFile +
                      "': " + opensslLastErrorString();
                SSL_CTX_free(ctx);
                return false;
            }
        }
    }

    // Optional client certificate.
    if (opt.hasCertAndKey()) {
        if (SSL_CTX_use_certificate_file(ctx, opt.certFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            err = "SSL_CTX_use_certificate_file failed for '" + opt.certFile +
                  "': " + opensslLastErrorString();
            SSL_CTX_free(ctx);
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, opt.keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            err = "SSL_CTX_use_PrivateKey_file failed for '" + opt.keyFile +
                  "': " + opensslLastErrorString();
            SSL_CTX_free(ctx);
            return false;
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            err = "SSL_CTX_check_private_key failed: " + opensslLastErrorString();
            SSL_CTX_free(ctx);
            return false;
        }
    }

    _ctx = ctx;
    return true;
}

SSL* WebSocketTLSContext::createSsl(std::string& err) const {
    err.clear();
    if (!_ctx) {
        err = "TLS context not initialized (SSL_CTX is null)";
        return nullptr;
    }

    SSL* ssl = SSL_new(_ctx);
    if (!ssl) {
        err = "SSL_new failed: " + opensslLastErrorString();
        return nullptr;
    }
    return ssl;
}