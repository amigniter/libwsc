/*
 *  WebSocketContext.h
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#pragma once
#include <event2/event.h>
#include <event2/buffer.h>

#ifdef USE_TLS
    #include <event2/bufferevent_ssl.h>
    #include "WebSocketTLSContext.h"
#else
    #include <event2/bufferevent.h>
    #include "sha1.hpp"
#endif

#include <event2/dns.h>
#include <event2/thread.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <deque>
#include <random>
#include <zlib.h>

#include <vector>
#include <array>
#include <algorithm>
#include <stdexcept>

#include "base64.h"
#include "WebSocketClient.h"
#include "WebSocketHeaders.h"
#include "WebSocketTLSOptions.h"

#include "WebSocketReceiver.h"
#include "IWebSocketSinks.h"

#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) htonll(x)

class WebSocketContext: public std::enable_shared_from_this<WebSocketContext>,
                        public IWebSocketSinks {
public:

    using MessageType = WebSocketClient::MessageType;
    using ErrorCode = WebSocketClient::ErrorCode;
    using CloseCode = WebSocketClient::CloseCode;

    using OpenCallback = std::function<void()>;
    using ErrorCallback = std::function<void(int error_code, const std::string& error_message)>;
    using CloseCallback = std::function<void(int code, const std::string& reason)>;
    using MessageCallback = std::function<void(const std::string&)>;
    using BinaryCallback = std::function<void(const void*, size_t)>;

    struct Config {
        std::string host;
        unsigned short port;
        std::string uri;
        bool secure;
        bool is_ip_address;
        unsigned int ping_interval;
        unsigned int connection_timeout;
        WebSocketHeaders headers;
        WebSocketTLSOptions tls;
        bool compression_requested;
    };

    explicit WebSocketContext(const Config& cfg);
    ~WebSocketContext();
    
    void setOpenCallback(OpenCallback cb);
    void setErrorCallback(ErrorCallback cb);
    void setCloseCallback(CloseCallback cb);
    void setMessageCallback(MessageCallback cb);
    void setBinaryCallback(BinaryCallback cb);

    void start();
    void stop();

    // IWebSocketSinks overrides
    bool rxCompressionEnabled() const override;
    bool isConnected() const;
    bool sendData(const void* data, size_t length, MessageType type);   //public wrapper

    void onRxPong(std::vector<uint8_t>&& payload) override;
    void onRxPing(std::vector<uint8_t>&& payload) override;
    void onRxClose(uint16_t code, std::string&& reason) override;
    void onRxProtocolError(uint16_t closeCode, std::string&& why) override;
    void onRxText(std::string&& msg) override;
    void onRxBinary(std::vector<uint8_t>&& msg) override;
    bool rxIsTerminating() const override;

private:
    static void libeventThreads();
    bool containsHeader(const std::string& response, const std::string& header) const;

    // Static callbacks - these will be called by libevent
    static void readCallback(bufferevent* bev, void* ctx);
    static void eventCallback(bufferevent* bev, short events, void *ctx);
    static void timeoutCallback(evutil_socket_t fd, short event, void *arg);
    static void pingCallback(evutil_socket_t fd, short event, void *arg);
    static void wakeupCallback(evutil_socket_t fd, short event, void *arg);
    static void sendCallback(evutil_socket_t fd, short events, void *arg);
    static void closeTimerCb(evutil_socket_t fd, short events, void *arg);

    void run();
    void cleanup();

    // Member callback implementations
    void handleRead(bufferevent* bev);
    // void handleWrite(bufferevent* bev);
    void handleEvent(bufferevent* bev, short events);

    inline void requestLoopExit();
    inline void requestSendFlush();
    inline void requestWakeup();
    inline void armCloseTimer();

    bool close(int code = 1000, const std::string& reason = "Normal closure");
    bool close(CloseCode code, const std::string& reason);
    void send(evbuffer* buf, const void* data, size_t len, MessageType type = MessageType::TEXT);

    bool sendNow(const void* data, size_t length, MessageType type);    //event thread only
    void stopNow();

    void sendError(int error_code, const std::string& error_message);
    void sendError(ErrorCode code, const std::string& message);
    void sendPing();
    void sendCloseCallback(int code, const std::string& reason);
private:

    using ConnectionState = WebSocketClient::ConnectionState;

    Config _cfg;
    WebSocketReceiver receiver;
#ifdef USE_TLS
    WebSocketTLSContext _tls;
#endif
    std::string key;
    std::string accept;

    std::mutex cb_mutex;
    std::mutex base_mutex;

    OpenCallback on_open;
    ErrorCallback on_error;
    CloseCallback on_close;
    MessageCallback on_message;
    BinaryCallback on_binary;

    static const size_t MAX_QUEUE_SIZE = 1024;

    // Pending queue
    struct Pending {
        enum Type { Text, Binary, Close } type;
        std::string            text;
        std::vector<uint8_t>   bin;

        Pending(std::string&&  t) : type(Text),   text(std::move(t)) {}
        Pending(std::vector<uint8_t>&& b, Type t = Binary) : type(t), bin(std::move(b)) {}
    };

    std::deque<Pending> send_queue;
    std::mutex send_queue_mutex;
    
    void flushSendQueue();

    // Libevent objects
    event_base* base = nullptr;
    evdns_base* dns_base = nullptr;
    bufferevent* _bev = nullptr;

    // Thread/state
    std::thread event_thread;
    std::thread::id event_tid{};
    std::atomic_bool running{false};

    void sendHandshakeRequest();

    // Connection state
    std::atomic<bool> upgraded{false};

    // WebSocket key
    std::array<uint8_t,20> hexToBytes(const std::string &hex);
    std::string getWebSocketKey();
    std::string computeAccept(const std::string &key);

    // Per-message Deflate
    bool use_compression = false;
    int compression_level = 6; //Z_BEST_SPEED;
    
    bool server_no_context_takeover = false;
    bool client_no_context_takeover = false;
    int  client_max_window_bits = 15;
    int  server_max_window_bits = 15;

    // Connection states
    std::atomic<ConnectionState> connection_state{ConnectionState::DISCONNECTED};

    // Close
    struct event* close_timer = nullptr;
    std::atomic_bool stop_requested{false}; // shutdown has been requested
    bool close_sent = false;
    bool close_received = false;

    // One-shot guards
    std::atomic<bool> protocol_failed{false};
    std::atomic<bool> close_cb_fired{false};

    // Timeout / Ping / Wakeup
    struct event *timeout_event = nullptr;
    struct event *ping_event = nullptr;
    struct event *wakeup_event = nullptr;

    // Sender
    struct event *send_event = nullptr;
    std::atomic_bool send_flush_pending{false};
};