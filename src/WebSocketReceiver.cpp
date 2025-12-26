/*
 *  WebSocketReceiver.cpp
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#include "WebSocketReceiver.h"

#include <event2/buffer.h>
#include <algorithm>
#include <cstring>

#include "Logger.h"

WebSocketReceiver::WebSocketReceiver(IWebSocketSinks& sinks) : _sinks(sinks) {}

WebSocketReceiver::~WebSocketReceiver() {
    shutdownCompression();
}

void WebSocketReceiver::shutdownCompression() {
    if (inflate_initialized) {
        inflateEnd(&inflate_stream);
        inflate_initialized = false;
    }
    if (deflate_initialized) {
        deflateEnd(&deflate_stream);
        deflate_initialized = false;
    }
    tx_compressed_buf.clear();
    tx_payload_len = 0;
    _cfg = PerMessageDeflateConfig{};
}

bool WebSocketReceiver::initializeCompression(const PerMessageDeflateConfig& cfg) {
    shutdownCompression();
    _cfg = cfg;

    if (!_cfg.enabled) {
        return true;
    }

    if (!rxInitInflate()) {
        shutdownCompression();
        return false;
    }

    if (!txInitDeflate()) {
        if (inflate_initialized) {
            inflateEnd(&inflate_stream);
            inflate_initialized = false;
        }
        shutdownCompression();
        return false;
    }

    log_debug("Compression initialized successfully");

    /*log_debug("Compression initialized successfully (rx bits=%d, tx bits=%d)",
              _cfg.server_max_window_bits, _cfg.client_max_window_bits);*/
    return true;
}

bool WebSocketReceiver::rxInitInflate() {
    std::memset(&inflate_stream, 0, sizeof(inflate_stream));
    int ret = inflateInit2(&inflate_stream, -_cfg.server_max_window_bits);
    if (ret != Z_OK) {
        log_error("Failed to initialize inflate: %d", ret);
        return false;
    }
    inflate_initialized = true;
    return true;
}

bool WebSocketReceiver::txInitDeflate() {
    std::memset(&deflate_stream, 0, sizeof(deflate_stream));
    int ret = deflateInit2(&deflate_stream,
                           _cfg.compression_level,
                           Z_DEFLATED,
                           -_cfg.client_max_window_bits,
                           8,
                           Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        log_error("Failed to initialize deflate: %d", ret);
        return false;
    }
    deflate_initialized = true;
    return true;
}

void WebSocketReceiver::rxResetInflate() {
    if (!inflate_initialized) return;
    inflateEnd(&inflate_stream);
    inflate_initialized = false;
    rxInitInflate();
}

void WebSocketReceiver::txResetDeflate() {
    if (!deflate_initialized) return;
    deflateReset(&deflate_stream);
}

bool WebSocketReceiver::txDeflate(const uint8_t* in, size_t in_len) {
    if (!deflate_initialized) return false;

    // Attempt compression with progressively larger output buffers.
    // This accounts for zlib SYNC_FLUSH overhead, which is not fully
    // captured by deflateBound().
    for (int attempt = 0; attempt < 4; ++attempt) {
        txResetDeflate();

        // Add extra slack beyond deflateBound() to safely accommodate
        // SYNC_FLUSH trailer and internal zlib bookkeeping.
        const size_t extra = 64u * (attempt + 1);
        const size_t bound = deflateBound(&deflate_stream, static_cast<uLong>(in_len)) + extra;

        tx_compressed_buf.resize(bound);

        // zlib input/output buffers
        deflate_stream.next_in   = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in));
        deflate_stream.avail_in  = static_cast<uInt>(in_len);
        deflate_stream.next_out  = reinterpret_cast<Bytef*>(tx_compressed_buf.data());
        deflate_stream.avail_out = static_cast<uInt>(bound);

        // Perform a single SYNC_FLUSH compression step
        const int ret = deflate(&deflate_stream, Z_SYNC_FLUSH);

        // We only accept a clean SYNC_FLUSH result
        // Z_BUF_ERROR indicates insufficient output space and triggers a retry.
        if (ret != Z_OK) {
            // If we ran out of output space, retry larger
            if (ret == Z_BUF_ERROR || deflate_stream.avail_out == 0) {
                continue;
            }
            log_error("Compression failed (%d), sending raw", ret);
            return false;
        }

        const size_t produced = bound - deflate_stream.avail_out;
        if (produced < 4) {
            // Cannot possibly contain the SYNC_FLUSH trailer
            continue;
        }

        // A valid zlib SYNC_FLUSH output must end with 00 00 FF FF.
        // permessage-deflate requires stripping this trailer before framing.
        const size_t n = produced;
        if (!(tx_compressed_buf[n-4] == 0x00 &&
              tx_compressed_buf[n-3] == 0x00 &&
              tx_compressed_buf[n-2] == 0xFF &&
              tx_compressed_buf[n-1] == 0xFF)) {
            // Incomplete SYNC_FLUSH output, retry with more space
            continue;
        }

        // Exclude SYNC_FLUSH trailer from transmitted payload
        tx_payload_len = produced - 4;

        // Reset compression context when client_no_context_takeover is negotiated
        if (_cfg.client_no_context_takeover) {
            if (!txReinitAfterNoContextTakeover()) return false;
        }
        return true;
    }

    // Exhausted all retries without a valid SYNC_FLUSH frame
    log_error("Compression failed: could not produce complete SYNC_FLUSH output");
    return false;
}

bool WebSocketReceiver::txReinitAfterNoContextTakeover() {
    if (!deflate_initialized) return false;

    deflateEnd(&deflate_stream);
    deflate_initialized = false;
    std::memset(&deflate_stream, 0, sizeof(deflate_stream));
    
    int init_ret = deflateInit2(&deflate_stream,
                                _cfg.compression_level,
                                Z_DEFLATED,
                                -_cfg.client_max_window_bits,
                                8,
                                Z_DEFAULT_STRATEGY);
    if (init_ret != Z_OK) {
        log_error("deflateInit2 after context takeover failed (%d)", init_ret);
        return false;
    }

    deflate_initialized = true;
    return true;
}

bool WebSocketReceiver::txPrepare(const uint8_t* original_ptr, size_t original_len, bool request_compress, 
                                    const uint8_t*& payload_ptr, size_t& payload_len, bool& do_compress) {
    payload_ptr = original_ptr;
    payload_len = original_len;
    do_compress = false;

    if (!request_compress) return true;
    if (!_cfg.enabled) return true;
    if (!deflate_initialized) return true;

    if (!txDeflate(original_ptr, original_len)) {
        // fallback to raw
        payload_ptr = original_ptr;
        payload_len = original_len;
        do_compress = false;
        return true;
    }

    payload_ptr = tx_compressed_buf.data();
    payload_len = tx_payload_len;
    do_compress = true;
    return true;
}

bool WebSocketReceiver::rxInflate(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out) {
    if (!_cfg.enabled || !inflate_initialized) {
        out.assign(in, in + in_len);
        return true;
    }

    // permessage-deflate payloads omit the zlib SYNC_FLUSH trailer.
    // To inflate with Z_SYNC_FLUSH, we append 00 00 FF FF temporarily.
    static thread_local std::vector<uint8_t> src;
    src.clear();
    src.reserve(in_len + 4);

    src.insert(src.end(), in, in + in_len);
    src.push_back(0x00);
    src.push_back(0x00);
    src.push_back(0xFF);
    src.push_back(0xFF);

    // The decompressor state must be reset for each message (if negotiated).
    if (_cfg.server_no_context_takeover) {
        inflateReset(&inflate_stream);
    }

    // Configure zlib input buffer (src includes the appended SYNC_FLUSH trailer)
    inflate_stream.next_in  = reinterpret_cast<Bytef*>(src.data());
    inflate_stream.avail_in = static_cast<uInt>(src.size());

    out.clear();
    // The decompressed output can be larger than input.
    out.reserve(in_len * 4);
    
    uint8_t tmp[4096];

    while (true) {
        // Inflate into a fixed-size chunk and append to 'out'
        inflate_stream.next_out  = tmp;
        inflate_stream.avail_out = sizeof(tmp);

        int ret = inflate(&inflate_stream, Z_SYNC_FLUSH);

        if (ret == Z_BUF_ERROR) {
            // Not fatal, either the output buffer filled up (retry),
            // or we have no more input available (done).
            if (inflate_stream.avail_out == 0) {
                continue; // produced output, but tmp was full
            }
            
            if (inflate_stream.avail_in == 0) {
                break; // no more input
            }
            // Otherwise, no progress is possible
            log_error("inflate stalled (Z_BUF_ERROR with avail_in=%u avail_out=%u)",
                        inflate_stream.avail_in, inflate_stream.avail_out);
            return false;
        } else if (ret != Z_OK && ret != Z_STREAM_END) {
            // Failure (Z_DATA_ERROR, Z_MEM_ERROR)
            log_error("inflate failed: %d", ret);
            return false;
        }

        // Append produced bytes from this iteration
        size_t produced = sizeof(tmp) - inflate_stream.avail_out;
        
        if (produced) {
            const size_t old = out.size();
            out.resize(old + produced);
            std::memcpy(out.data() + old, tmp, produced);
        }

        // Z_STREAM_END would indicate the deflate stream ended.
        // With permessage-deflate + SYNC_FLUSH, we typically stop based on input/output state.
        if (ret == Z_STREAM_END) {
            break;
        }

        // If we consumed all input and did not fill the output buffer,
        // there is no more data to produce.
        if (inflate_stream.avail_in == 0 && inflate_stream.avail_out != 0) {
            break;
        }
    }

    return true;
}

void WebSocketReceiver::rxMaybeResetAfterMessage() {
    if (_cfg.enabled && _cfg.server_no_context_takeover) {
        rxResetInflate();
    }
}

void WebSocketReceiver::onData(evbuffer* buf) {
    for (;;) {

        if (_sinks.rxIsTerminating()) return;

        const size_t data_len = evbuffer_get_length(buf);
        if (data_len < 2) break;

        unsigned char hdr[14];
        const size_t peek_len = std::min(data_len, sizeof(hdr));
        evbuffer_copyout(buf, hdr, peek_len);

        const bool fin  = !!(hdr[0] & 0x80);
        const bool rsv1 = !!(hdr[0] & 0x40);
        const bool rsv2 = !!(hdr[0] & 0x20);
        const bool rsv3 = !!(hdr[0] & 0x10);
        const int  opcode = hdr[0] & 0x0F;
        const bool mask = !!(hdr[1] & 0x80);
        uint64_t payload_len = hdr[1] & 0x7F;

        if ((!_sinks.rxCompressionEnabled() && rsv1) || rsv2 || rsv3) {
            _sinks.onRxProtocolError(1002, "Unexpected RSV bits");
            return;
        }

        if ((opcode & 0x08) != 0 && !fin) {
            _sinks.onRxProtocolError(1002, "Control frame fragmented");
            return;
        }

        if (mask) {
            _sinks.onRxProtocolError(1002, "Masked frame from server");
            return;
        }

        size_t header_len = 2;

        if (payload_len == 126) {
            if (data_len < header_len + 2) break;
            header_len += 2;
            uint16_t u16; std::memcpy(&u16, &hdr[2], sizeof(u16));
            payload_len = ntohs(u16);
        } else if (payload_len == 127) {
            if (data_len < header_len + 8) break;
            header_len += 8;
            uint64_t u64; std::memcpy(&u64, &hdr[2], sizeof(u64));
            payload_len = ntohll(u64);
        }

        if ((opcode & 0x08) != 0 && payload_len > 125) {
            _sinks.onRxProtocolError(1002, "Control frame payload too large");
            return;
        }

        const size_t need = header_len + static_cast<size_t>(payload_len);
        if (data_len < need) break; // wait for full frame

        unsigned char* frame = evbuffer_pullup(buf, need);
        if (!frame) {
            _sinks.onRxProtocolError(1002, "Failed to pullup frame buffer");
            return;
        }
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
                _sinks.onRxProtocolError(1002, "Unsupported opcode");
                return;
        }

        evbuffer_drain(buf, need);
    }
}

void WebSocketReceiver::handleContinuationFrame(const unsigned char* payload, size_t payload_len, bool fin) {
    if (!message_in_progress) {
        log_error("Received continuation frame without initial frame");
        _sinks.onRxProtocolError(1002, "continuation frame without initial frame");
        return;
    }

    fragmented_message.insert(fragmented_message.end(),
                              payload,
                              payload + payload_len);

    // Only validate UTF-8 if this is an uncompressed text message
    if (!compressed_message_in_progress && fragmented_opcode == 0x01) {
        if (!utf8Validator.validateChunk(payload, payload_len)) {
            log_error("Invalid UTF-8 in continuation frame");
            utf8Validator.reset();
            _sinks.onRxProtocolError(1007, "Invalid UTF-8 in text message");
            return;
        }
    }

    if (!fin) return;

    if (compressed_message_in_progress) {
        std::vector<uint8_t> output;
        bool ok = decompressMessage(fragmented_message.data(), fragmented_message.size(), output);
        if (!ok) {
            utf8Validator.reset();
            _sinks.onRxProtocolError(1007, "Decompression failed");
            return;
        }
        fragmented_message.swap(output);

        // takeover handling
        rxMaybeResetAfterMessage();
    }

    switch (fragmented_opcode) {
        case 0x01: {
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
                _sinks.onRxProtocolError(1007, "Invalid UTF-8 in text message");
                return;
            }

            std::string message(fragmented_message.begin(), fragmented_message.end());
            utf8Validator.reset();
            _sinks.onRxText(std::move(message));
            break;
        }

        case 0x02: {
            _sinks.onRxBinary(std::move(fragmented_message));
            break;
        }

        default:
            log_error("Unknown fragmented opcode: %d", fragmented_opcode);
            _sinks.onRxProtocolError(1002, "Unknown fragmented opcode");
            return;
    }

    // Reset fragmentation state
    message_in_progress = false;
    compressed_message_in_progress = false;
    std::vector<uint8_t>().swap(fragmented_message);
    fragmented_opcode = 0;
}

void WebSocketReceiver::handleDataFrame(const unsigned char* payload, size_t payload_len, bool fin, int opcode, bool rsv1) {
    if (message_in_progress) {
        log_error("Received new data frame (opcode %d) while expecting a continuation frame.", opcode);
        _sinks.onRxProtocolError(1002, "Received new data frame when expecting continuation frame");
        return;
    }

    const bool compressed = (rsv1 && _sinks.rxCompressionEnabled());

    if (!fin) {
        message_in_progress = true;
        fragmented_opcode = opcode;
        fragmented_message.assign(payload, payload + payload_len);

        compressed_message_in_progress = compressed;

        if (opcode == 0x01 && !compressed_message_in_progress) {
            utf8Validator.reset();
            if (!utf8Validator.validateChunk(payload, payload_len)) {
                log_error("Invalid UTF-8 in initial fragment");
                utf8Validator.reset();
                _sinks.onRxProtocolError(1007, "Invalid UTF-8 in text message");
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
            _sinks.onRxProtocolError(1007, "Decompression failed");
            return;
        }
        msg_data = decompressed.data();
        msg_len  = decompressed.size();

        rxMaybeResetAfterMessage();
    }

    if (opcode == 0x01) {
        utf8Validator.reset();
        if (!utf8Validator.validateChunk(msg_data, msg_len) ||
            !utf8Validator.validateFinal()) {
            log_error("Invalid UTF-8 in unfragmented text");
            utf8Validator.reset();
            _sinks.onRxProtocolError(1007, "Invalid UTF-8 in text message");
            return;
        }

        std::string message(reinterpret_cast<const char*>(msg_data), msg_len);
        _sinks.onRxText(std::move(message));

    } else if (opcode == 0x02) {
        std::vector<uint8_t> b(msg_data, msg_data + msg_len);
        _sinks.onRxBinary(std::move(b));

    } else {
        log_error("Unsupported data opcode: %d", opcode);
        _sinks.onRxProtocolError(1002, "Unsupported opcode");
        return;
    }
}

void WebSocketReceiver::handleCloseFrame(const unsigned char* payload, size_t payload_len) {
    uint16_t close_code = 1000;
    std::string close_reason;
    bool protocol_error = false;

    if (payload_len > 125) {
        log_error("Close frame too large (%zu bytes)", payload_len);
        close_code = 1002;
        protocol_error = true;
    }
    else if (payload_len == 1) {
        log_error("Invalid close frame: payload length is 1");
        close_code = 1002;
        protocol_error = true;
    }
    else if (payload_len >= 2) {
        uint16_t net;
        std::memcpy(&net, payload, 2);
        uint16_t received = ntohs(net);
        close_code = received;

        if (!((close_code >= 1000 && close_code <= 1011 &&
               close_code != 1004 && close_code != 1005 && close_code != 1006) ||
              (close_code >= 3000 && close_code <= 4999))) {
            log_error("Received invalid close code: %d", close_code);
            close_code = 1002;
            protocol_error = true;
        }

        if (payload_len > 2) {
            const char* reason_ptr = reinterpret_cast<const char*>(payload + 2);
            size_t reason_len = payload_len - 2;
            if (reason_len > 123) reason_len = 123;

            if (!isValidUtf8(reason_ptr, reason_len)) {
                log_error("Close reason is not valid UTF-8");
                close_code = 1002;
                protocol_error = true;
            } else {
                close_reason.assign(reason_ptr, reason_len);
            }
        }
    }

    const int reply_code = protocol_error ? 1002 : static_cast<int>(close_code);
    std::string reply_reason = protocol_error ? "" : close_reason;

    _sinks.onRxClose(static_cast<uint16_t>(reply_code), std::move(reply_reason));
}

void WebSocketReceiver::handlePingFrame(const unsigned char* payload, size_t payload_len) {
    if (payload_len > 125) {
        log_error("Protocol violation: ping payload > 125 bytes");
        _sinks.onRxProtocolError(1002, "Control frame payload too large");
        return;
    }

    std::vector<uint8_t> p(payload, payload + payload_len);
    _sinks.onRxPing(std::move(p));
}

bool WebSocketReceiver::isValidUtf8(const char* str, size_t len) {
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