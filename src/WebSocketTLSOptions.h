/*
 *  WebSocketTLSOptions.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#pragma once
#include <string>

/**
 * \struct WebSocketTLSOptions
 * \brief Configuration container for WebSocket TLS/SSL settings
 *
 * \details Provides TLS configuration options including certificate files,
 * cipher suites, and validation settings for secure WebSocket connections (wss://).
 */
struct WebSocketTLSOptions {
    std::string certFile; ///< Path to client certificate file (PEM format)
    std::string keyFile; ///< Path to private key file (PEM format)
    std::string caFile = "SYSTEM"; ///< CA bundle path ("SYSTEM" for OS trust store, "NONE" to disable verification)
    std::string ciphers = "DEFAULT"; ///< Custom cipher suite string or "DEFAULT" for secure defaults
    bool disableHostnameValidation = false; ///< If true, skips hostname verification

    /**
     * \brief Get the default secure cipher suite list
     * \return Const reference to default cipher string
     *
     * \details Returns a carefully curated list of modern, secure cipher suites:
     * - Prioritizes ECDHE key exchange for forward secrecy
     * - Prefers AES-GCM authenticated encryption
     * - Includes fallbacks for compatibility
     * - Excludes known weak ciphers (RC4, 3DES, etc.)
     */
    static const std::string& getDefaultCiphers() {
        static const std::string DEFAULT_CIPHERS =
            "ECDHE-ECDSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-AES128-SHA:"
            "ECDHE-ECDSA-AES256-SHA:"
            "ECDHE-ECDSA-AES128-SHA256:"
            "ECDHE-ECDSA-AES256-SHA384:"
            "ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-RSA-AES128-SHA:"
            "ECDHE-RSA-AES256-SHA:"
            "ECDHE-RSA-AES128-SHA256:"
            "ECDHE-RSA-AES256-SHA384:"
            "DHE-RSA-AES128-GCM-SHA256:"
            "DHE-RSA-AES256-GCM-SHA384:"
            "DHE-RSA-AES128-SHA:"
            "DHE-RSA-AES256-SHA:"
            "DHE-RSA-AES128-SHA256:"
            "DHE-RSA-AES256-SHA256:"
            "AES128-SHA";
        return DEFAULT_CIPHERS;
    }

    /**
     * \brief Check if both certificate and key are configured
     * \return true if both certFile and keyFile are non-empty
     */
    bool hasCertAndKey() const {
        return !certFile.empty() && !keyFile.empty();
    }

    /**
     * \brief Check if using system CA trust store
     * \return true if caFile is "SYSTEM" or empty
     */
    bool isUsingSystemCA() const {
        return caFile == "SYSTEM" || caFile.empty();
    }

    /**
     * \brief Check if peer verification is disabled
     * \return true if caFile is "NONE"
     * \warning Disabling peer verification compromises security
     */
    bool isPeerVerifyDisabled() const {
        return caFile == "NONE";
    }

    /**
     * \brief Check if using custom CA bundle
     * \return true if using non-system, non-disabled CA file
     */
    bool isUsingCustomCA() const {
        return !isUsingSystemCA() && !isPeerVerifyDisabled();
    }

    /**
     * \brief Check if using default cipher suite
     * \return true if ciphers is empty or "DEFAULT"
     */
    bool isUsingDefaultCiphers() const {
        return ciphers.empty() || ciphers == "DEFAULT";
    }
};