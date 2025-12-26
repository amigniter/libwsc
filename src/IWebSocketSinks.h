/*
 *  IWebSocketSinks.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct IWebSocketSinks {
    virtual ~IWebSocketSinks() = default;
    virtual bool rxCompressionEnabled() const = 0;
    virtual void onRxPong(std::vector<uint8_t>&& payload) = 0;
    virtual void onRxPing(std::vector<uint8_t>&& payload) = 0;
    virtual void onRxClose(uint16_t code, std::string&& reason) = 0;
    virtual void onRxProtocolError(uint16_t closeCode, std::string&& why) = 0;
    virtual void onRxText(std::string&& msg) = 0;
    virtual void onRxBinary(std::vector<uint8_t>&& msg) = 0;
    virtual bool rxIsTerminating() const = 0;
};