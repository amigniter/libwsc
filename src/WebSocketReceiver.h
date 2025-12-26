/*
 *  WebSocketReceiver.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#pragma once
#include "IWebSocketSinks.h"
#include "Utf8Validator.h"

#include <event2/buffer.h>
#include <cstdint>
#include <string>
#include <vector>

#include <zlib.h>

#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) htonll(x)

struct PerMessageDeflateConfig {
    bool enabled = false;
    bool client_no_context_takeover = false;
    bool server_no_context_takeover = false;
    int client_max_window_bits = 15;
    int server_max_window_bits = 15;
    int compression_level = Z_DEFAULT_COMPRESSION;
};

class WebSocketReceiver {
public:
    explicit WebSocketReceiver(IWebSocketSinks& sinks);
    ~WebSocketReceiver();

    bool initializeCompression(const PerMessageDeflateConfig& cfg);
    void shutdownCompression();

    bool txPrepare(const uint8_t* original_ptr, size_t original_len, bool request_compress,
                   const uint8_t*& payload_ptr, size_t& payload_len, bool& do_compress);
    
    bool rxInflate(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out);
    void rxMaybeResetAfterMessage();

    void onData(evbuffer* buf);
    
    bool decompressMessage(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out) {
        return rxInflate(in, in_len, out);
    }

    void handleContinuationFrame(const unsigned char* payload, size_t payload_len, bool fin);
    void handleDataFrame(const unsigned char* payload, size_t payload_len, bool fin, int opcode, bool rsv1);
    void handleCloseFrame(const unsigned char* payload, size_t payload_len);
    void handlePingFrame(const unsigned char* payload, size_t payload_len);
    
private:
    bool txDeflate(const uint8_t* in, size_t in_len);
    bool txReinitAfterNoContextTakeover();
    bool rxInitInflate();
    bool txInitDeflate();
    void rxResetInflate();
    void txResetDeflate();

private:
    IWebSocketSinks& _sinks;
    PerMessageDeflateConfig _cfg;

    z_stream inflate_stream{};
    z_stream deflate_stream{};
    bool inflate_initialized = false;
    bool deflate_initialized = false;
    
    std::vector<uint8_t> tx_compressed_buf;
    size_t tx_payload_len = 0;

    bool message_in_progress = false;
    bool compressed_message_in_progress = false;
    int  fragmented_opcode = 0;
    std::vector<uint8_t> fragmented_message;

    Utf8Validator utf8Validator;
    bool isValidUtf8(const char *str, size_t len);
};
