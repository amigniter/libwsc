/*
 *  WebSocketHeaders.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#pragma once
#include <string>
#include <map>

/**
 * \class WebSocketHeaders
 * \brief Container for managing WebSocket protocol headers
 *
 * \details Provides a simple interface for manipulating key-value pairs
 * used in WebSocket protocol communication. The implementation uses
 * std::map for case-sensitive header management.
 *
 * \note For HTTP/WebSocket compliance, header fields should be compared
 * case-insensitively according to RFC 6455, though storage is case-sensitive.
 */
struct WebSocketHeaders {
    std::map<std::string, std::string> headers; ///< Internal storage of header key-value pairs

    /**
     * \brief Set or update a header value
     * \param key The header field name
     * \param value The header field value
     * \note Overwrites any existing value for the key
     */
    void set(const std::string& key, const std::string& value) {
        headers[key] = value;
    }

    /**
     * \brief Remove a header field
     * \param key The header field name to remove
     * \return Number of elements removed (0 or 1)
     */
    void remove(const std::string& key) {
        headers.erase(key);
    }

    /**
     * \brief Remove all headers
     */
    void clear() {
        headers.clear();
    }

    /**
     * \brief Check if no headers are present
     * \return true if container is empty, false otherwise
     */
    bool empty() const {
        return headers.empty();
    }

    /**
     * \brief Get all headers as a const reference
     * \return Const reference to the underlying map container
     * \warning Do not use this to modify headers - use set() instead
     */
    const std::map<std::string, std::string>& all() const {
        return headers;
    }
};
