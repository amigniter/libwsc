#include "WebSocketServer.h"
#include "base64.h"
#include "sha1.hpp"

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/thread.h>
#include <event2/util.h>
#ifdef USE_TLS
#include <event2/bufferevent_ssl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <sstream>

namespace {
	const size_t MAX_HEADER = 8192;
	const size_t MAX_MESSAGE = 1024 * 1024;
	std::string trim(std::string s) {
		const size_t b = s.find_first_not_of(" \t\r\n");
		if (b == std::string::npos) {
			return "";
		}
		const size_t e = s.find_last_not_of(" \t\r\n");
		return s.substr(b, e - b + 1);
	}
	std::string lower(std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return s;
	}
	bool token(const std::string& header, const std::string& expected) {
		std::istringstream in(header);
		std::string item;
		while (std::getline(in, item, ',')) {
			if (lower(trim(item)) == expected) return true;
		}
		return false;
	}
	std::string acceptKey(const std::string& key) {
		class SHA1 sha;
		sha.update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
		const std::string hex = sha.final();
		std::string bytes;
		for (size_t i = 0; i + 1 < hex.size(); i += 2) {
			bytes.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
		}
		return base64_encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
	}
	std::string frame(unsigned char opcode, const std::string& payload) {
		std::string out;
		out.push_back(static_cast<char>(0x80 | opcode));
		const uint64_t n = payload.size();
		if (n < 126) {
			out.push_back(static_cast<char>(n));
		}
		else if (n <= 65535) {
			out.push_back(126);
			out.push_back(static_cast<char>(n >> 8));
			out.push_back(static_cast<char>(n));
		} else {
			out.push_back(127);
			for (int i = 7; i >= 0; --i) {
				out.push_back(static_cast<char>(n >> (8 * i)));
			}
		}
		out += payload;
		return out;
	}
}

struct WebSocketServer::Impl {
	struct Connection {
		Impl* owner;
		ConnectionId id;
		bufferevent* bev;
		bool upgraded = false;
		bool closing = false;
		unsigned char fragmentedOpcode = 0;
		std::string fragmentedPayload;
		std::string pending;
		bool awaitingPong = false;
		uint64_t pingSequence = 0;
		std::string pingPayload;
		std::chrono::steady_clock::time_point nextPing;
		std::chrono::steady_clock::time_point pongDeadline;
		Connection(Impl* o, ConnectionId i, bufferevent* b):owner(o), id(i), bev(b) {
		}
	};
	struct Pending {
		ConnectionId id;
		std::string data;
		bool broadcast;
	};
	mutable std::mutex mutex;
	std::mutex lifecycleMutex;
	std::deque<Pending> sendQueue;
	size_t queuedBytes = 0;
	event* sendEvent = nullptr;
	event* heartbeatEvent = nullptr;
	std::atomic<bool> stopRequested{false};
	static constexpr size_t MAX_QUEUE_BYTES = 4*1024*1024;
	static constexpr size_t MAX_OUTPUT_BYTES = 256*1024;
	static constexpr int PING_INTERVAL_SECONDS = 30;
	static constexpr int PONG_TIMEOUT_SECONDS = 10;

	static void heartbeat(evutil_socket_t, short, void* arg) {
		Impl* s = static_cast<Impl*>(arg);
		if (s->stopRequested.load(std::memory_order_acquire)) return;

		const auto now = std::chrono::steady_clock::now();
		std::vector<Connection*> expired;
		for (auto& entry : s->clients) {
			Connection* c = entry.second.get();
			if (!c->upgraded || c->closing) continue;

			if (c->awaitingPong) {
				if (now >= c->pongDeadline) expired.push_back(c);
				continue;
			}
			if (now < c->nextPing) continue;

			++c->pingSequence;
			c->pingPayload.clear();
			for (int i = 7; i >= 0; --i) {
				c->pingPayload.push_back(static_cast<char>(c->pingSequence >> (i * 8)));
			}
			const std::string ping = frame(9, c->pingPayload);
			if (evbuffer_get_length(bufferevent_get_output(c->bev)) + ping.size() > MAX_OUTPUT_BYTES ||
			    bufferevent_write(c->bev, ping.data(), ping.size()) != 0) {
				expired.push_back(c);
				continue;
			}
			c->awaitingPong = true;
			c->pongDeadline = now + std::chrono::seconds(PONG_TIMEOUT_SECONDS);
		}

		// Dropping connections mutates clients; never erase while iterating it.
		for (Connection* c : expired) drop(c);
	}

	static void flushSendQueue(evutil_socket_t, short, void* arg) {
		Impl* s = static_cast<Impl*>(arg);
		if (s->stopRequested.load(std::memory_order_acquire)) {
			// All bufferevents are released on their owning event thread.
			if (s->heartbeatEvent) event_del(s->heartbeatEvent);
			if (s->listener) evconnlistener_disable(s->listener);
			std::map<ConnectionId, std::unique_ptr<Connection>> remaining;
			DisconnectCallback cb;
			{
				std::lock_guard<std::mutex> lock(s->mutex);
				s->sendQueue.clear();
				s->queuedBytes = 0;
				remaining.swap(s->clients);
				cb = s->disconnected;
			}
			std::vector<ConnectionId> disconnectedIds;
			for (auto& entry : remaining) {
				Connection* c = entry.second.get();
				if (c->upgraded) disconnectedIds.push_back(c->id);
				bufferevent_disable(c->bev, EV_READ | EV_WRITE);
				bufferevent_setcb(c->bev, nullptr, nullptr, nullptr, nullptr);
				bufferevent_free(c->bev);
				c->bev = nullptr;
			}
			remaining.clear();
			if (cb) {
				for (ConnectionId id : disconnectedIds) cb(id);
			}
			// Exit after the current iteration, not in the middle of a callback.
			const timeval now = {0, 0};
			event_base_loopexit(s->base, &now);
			return;
		}
		std::deque<Pending> local;
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			local.swap(s->sendQueue);
			s->queuedBytes = 0;
		}
		for (const auto& p : local) {
			if (p.broadcast) {
				for (auto& entry : s->clients) {
					Connection* c = entry.second.get();
					if (!c->upgraded || c->closing) continue;
					if (evbuffer_get_length(bufferevent_get_output(c->bev)) + p.data.size() > MAX_OUTPUT_BYTES) continue;
					bufferevent_write(c->bev, p.data.data(), p.data.size());
				}
			} else {
				auto it = s->clients.find(p.id);
				if (it == s->clients.end()) continue;
				Connection* c = it->second.get();
				if (!c->upgraded || c->closing) continue;
				if (evbuffer_get_length(bufferevent_get_output(c->bev)) + p.data.size() > MAX_OUTPUT_BYTES) continue;
				bufferevent_write(c->bev, p.data.data(), p.data.size());
			}
		}
	}
	bool enqueue(ConnectionId id, std::string data, bool broadcast) {
		std::lock_guard<std::mutex> lock(mutex);
		if (!running.load() || !sendEvent || queuedBytes + data.size() > MAX_QUEUE_BYTES) return false;
		if (!broadcast && clients.find(id) == clients.end()) return false;
		queuedBytes += data.size();
		sendQueue.push_back(Pending {id, std::move(data), broadcast});
		event_active(sendEvent, EV_TIMEOUT, 1);
		return true;
	}
#ifdef USE_TLS
	SSL_CTX* sslCtx = nullptr;
	// Owned by Impl; freed after all TLS bufferevents.
#endif
	bool tlsEnabled = false;
	event_base* base = nullptr;
	evconnlistener* listener = nullptr;
	std::thread worker;
	std::atomic<bool> running{false};
	ConnectionId nextId = 1;
	std::map<ConnectionId, std::unique_ptr<Connection>> clients;
	AuthCallback authenticate;
	ConnectCallback connected;
	MessageCallback message;
	DisconnectCallback disconnected;
	static void drop(Connection* c) {
		Impl* s = c->owner;
		const ConnectionId id = c->id;
		bool upgraded = c->upgraded;
		DisconnectCallback cb;
		std::unique_ptr<Connection> removed;
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			auto it = s->clients.find(id);
			if (it == s->clients.end())return;
			removed = std::move(it->second);
			s->clients.erase(it);
			cb = s->disconnected;
		}
		// Never acquire a libevent bufferevent lock while holding s->mutex.
		// read()/handshake() can acquire these locks in the opposite order.
		bufferevent_disable(removed->bev, EV_READ | EV_WRITE);
		bufferevent_setcb(removed->bev, nullptr, nullptr, nullptr, nullptr);
		bufferevent_free(removed->bev);
		if (upgraded && cb) cb(id);
	}
	static bool handshake(Connection* c) {
		const size_t end = c->pending.find("\r\n\r\n");
		if (end == std::string::npos)return c->pending.size() <= MAX_HEADER;
		if (end+4>MAX_HEADER)return false;
		std::istringstream in(c->pending.substr(0, end));
		std::string line;
		if (!std::getline(in, line))return false;
		if (line.empty() || line.back() !='\r')return false;
		line.pop_back();
		std::istringstream request(line);
		std::string method, path, version, extra;
		if (!(request>>method>>path>>version) || request>>extra || method !="GET"|| version !="HTTP/1.1")return false;
		std::map<std::string, std::string> headers;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() =='\r')line.pop_back();
			const size_t p = line.find(':');
			if (p == std::string::npos)return false;
			const std::string name = lower(trim(line.substr(0, p)));
			if (name.empty() || headers.count(name))return false;
			headers[name] = trim(line.substr(p+1));
		}
		const std::string key = headers["sec-websocket-key"];
		if (!token(headers["connection"],"upgrade") || lower(headers["upgrade"]) !="websocket"|| headers["sec-websocket-version"] !="13"|| key.size() != 24 || base64_decode(key).size() != 16)return false;
		// Authenticate before sending HTTP 101. Never log the supplied token.
		AuthCallback auth;
		{
			std::lock_guard<std::mutex> lock(c->owner->mutex);
			auth = c->owner->authenticate;
		}
		const auto authorization = headers.find("authorization");
		std::string bearer;
		if (authorization != headers.end()) {
			const std::string& value = authorization->second;
			if (value.size()>7 && lower(value.substr(0, 7)) =="bearer ") {
				bearer = value.substr(7);
				// Bearer credentials cannot contain whitespace.
				if (bearer.find_first_of(" \t\r\n") != std::string::npos) bearer.clear();
			}
		}
		bool authorized = false;
		if (auth && !bearer.empty()) {
			try {
				authorized = auth(bearer);
			}
			catch (...) {
				authorized = false;
			}
		}
		if (!authorized) {
			static const char unauthorized[] ="HTTP/1.1 401 Unauthorized\r\n""Connection: close\r\n""WWW-Authenticate: Bearer\r\n""Content-Length: 0\r\n\r\n";
			if (bufferevent_write(c->bev, unauthorized, sizeof(unauthorized)-1) != 0) return false;
			c->pending.clear();
			c->closing = true;
			bufferevent_disable(c->bev, EV_READ);
			return true;
			// write callback closes after 401 has been flushed
		}
		const std::string reply ="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+acceptKey(key)+"\r\n\r\n";
		if (bufferevent_write(c->bev, reply.data(), reply.size()) != 0)return false;
		c->pending.erase(0, end+4);
		c->upgraded = true;
		c->nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(PING_INTERVAL_SECONDS);
		bufferevent_set_timeouts(c->bev, nullptr, nullptr);
		ConnectCallback cb;
		{
			std::lock_guard<std::mutex> lock(c->owner->mutex);
			cb = c->owner->connected;
		}
		if (cb) cb(c->id);
		return true;
	}
	static bool parse(Connection* c) {
		std::string& b = c->pending;
		while (b.size() >= 2) {
			const unsigned char a = static_cast<unsigned char>(b[0]);
			const unsigned char z = static_cast<unsigned char>(b[1]);
			const bool fin = (a&0x80) != 0;
			const unsigned char opcode = a&0x0f;
			if ((a&0x70) != 0 || (z&0x80) == 0) return false;
			if (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10) return false;
			if (opcode >= 8 && (!fin || (z&0x7f)>125)) return false;
			uint64_t len = z&0x7f;
			size_t pos = 2;
			if (len == 126) {
				if (b.size()<4)return true;
				len = (static_cast<unsigned char>(b[2])<<8)|static_cast<unsigned char>(b[3]);
				pos = 4;
				if (len<126)return false;
			}
			else if (len == 127) {
				if (b.size()<10)return true;
				if ((static_cast<unsigned char>(b[2])&0x80) != 0)return false;
				len = 0;
				for (int i = 2; i<10; ++i)len = (len<<8)|static_cast<unsigned char>(b[i]);
				pos = 10;
				if (len <= 65535)return false;
			}
			if (len>MAX_MESSAGE || (opcode >= 8 && len>125))return false;
			if (opcode == 0 && c->fragmentedOpcode == 0)return false;
			if ((opcode == 1 || opcode == 2) && c->fragmentedOpcode != 0)return false;
			if (opcode<8 && len>MAX_MESSAGE-c->fragmentedPayload.size())return false;
			if (b.size()<pos+4+static_cast<size_t>(len))return true;
			const unsigned char* mask = reinterpret_cast<const unsigned char*>(b.data()+pos);
			std::string payload = b.substr(pos+4, static_cast<size_t>(len));
			for (size_t i = 0; i<payload.size(); ++i)payload[i]^=mask[i%4];
			b.erase(0, pos+4+static_cast<size_t>(len));
			if (opcode == 8) {
				if (payload.size() == 1)return false;
				const std::string reply = frame(8, payload);
				if (bufferevent_write(c->bev, reply.data(), reply.size()) != 0)return false;
				c->closing = true;
				bufferevent_disable(c->bev, EV_READ);
				return true;
			}
			if (opcode == 9) {
				const std::string pong = frame(10, payload);
				if (bufferevent_write(c->bev, pong.data(), pong.size()) != 0)return false;
				continue;
			}
			if (opcode == 10) {
				if (c->awaitingPong && payload == c->pingPayload) {
					c->awaitingPong = false;
					c->pingPayload.clear();
					c->nextPing = std::chrono::steady_clock::now() + std::chrono::seconds(PING_INTERVAL_SECONDS);
				}
				continue;
			}
			if (opcode == 1 || opcode == 2) {
				if (!fin) {
					c->fragmentedOpcode = opcode;
					c->fragmentedPayload.swap(payload);
					continue;
				}
			} else {
				c->fragmentedPayload += payload;
				if (!fin)continue;
				payload.swap(c->fragmentedPayload);
			}
			const bool binary = (opcode == 2 || (opcode == 0 && c->fragmentedOpcode == 2));
			c->fragmentedOpcode = 0;
			c->fragmentedPayload.clear();
			MessageCallback cb;
			{
				std::lock_guard<std::mutex> lock(c->owner->mutex);
				cb = c->owner->message;
			}
			if (cb) cb(c->id, payload, binary);
		}
		return true;
	}
	static void written(bufferevent*, void* arg) {
		Connection* c = static_cast<Connection*>(arg);
		if (c->closing && evbuffer_get_length(bufferevent_get_output(c->bev)) == 0)drop(c);
	}
	static void read(bufferevent* bev, void* arg) {
		Connection* c = static_cast<Connection*>(arg);
		char buf[4096];
		int n;
		while ((n = evbuffer_remove(bufferevent_get_input(bev), buf, sizeof(buf)))>0) {
			c->pending.append(buf, static_cast<size_t>(n));
			if (!c->upgraded && !handshake(c)) {
				drop(c);
				return;
			}
			if (c->upgraded && !c->closing && !parse(c)) {
				drop(c);
				return;
			}
			if (c->closing) return;
			if (c->pending.size()>MAX_MESSAGE+32) {
				drop(c);
				return;
			}
		}
	}
	static void socketEvent(bufferevent*, short what, void* arg) {
		Connection* c = static_cast<Connection*>(arg);
		if (what & BEV_EVENT_CONNECTED) return;
		// TLS handshake completed.
		if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) drop(c);
	}
	static void accept(evconnlistener*, evutil_socket_t fd, sockaddr*, int, void* arg) {
		Impl* s = static_cast<Impl*>(arg);
		bufferevent* bev = nullptr;
#ifdef USE_TLS
		if (s->tlsEnabled) {
			SSL* ssl = SSL_new(s->sslCtx);
			if (!ssl) {
				evutil_closesocket(fd);
				return;
			}
			bev = bufferevent_openssl_socket_new(s->base, fd, ssl, BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS);
			if (!bev) {
				SSL_free(ssl);
				evutil_closesocket(fd);
				return;
			}
		}
		else
#endif
		{
			bev = bufferevent_socket_new(s->base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS);
			if (!bev) {
				evutil_closesocket(fd);
				return;
			}
		}
		Connection* c;
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			const ConnectionId id = s->nextId++;
			std::unique_ptr<Connection> p(new Connection(s, id, bev));
			c = p.get();
			s->clients[id] = std::move(p);
		}
		bufferevent_setcb(bev, &read, &written, &socketEvent, c);
		// Bound unauthenticated TLS/HTTP handshake duration.
		const timeval timeout = {15, 0};
		bufferevent_set_timeouts(bev, &timeout, &timeout);
		bufferevent_enable(bev, EV_READ|EV_WRITE);
	}
};

// C++11 requires out-of-class definitions for ODR-used constexpr members.
constexpr int WebSocketServer::Impl::PING_INTERVAL_SECONDS;
constexpr int WebSocketServer::Impl::PONG_TIMEOUT_SECONDS;

WebSocketServer::WebSocketServer():impl_(new Impl) {
}
WebSocketServer::~WebSocketServer() {
	stop();
}
bool WebSocketServer::listen(const std::string& address, uint16_t port) {
	std::lock_guard<std::mutex> life(impl_->lifecycleMutex);
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (impl_->worker.joinable() || impl_->running || !impl_->authenticate)return false;
	impl_->tlsEnabled = false;
	static const bool ready = evthread_use_pthreads() == 0;
	if (!ready)return false;
	sockaddr_storage addr;
	std::memset(&addr, 0, sizeof(addr));
	int len = sizeof(addr);
	if (evutil_parse_sockaddr_port((address+":"+std::to_string(port)).c_str(), reinterpret_cast<sockaddr*>(&addr), &len) != 0)return false;
	impl_->base = event_base_new();
	if (!impl_->base)return false;
	impl_->listener = evconnlistener_new_bind(impl_->base, &Impl::accept, impl_.get(), LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, 128, reinterpret_cast<sockaddr*>(&addr), len);
	if (!impl_->listener) {
		event_base_free(impl_->base);
		impl_->base = nullptr;
		return false;
	}
	impl_->sendEvent = event_new(impl_->base, -1, EV_PERSIST, &Impl::flushSendQueue, impl_.get());
	if (!impl_->sendEvent || event_add(impl_->sendEvent, nullptr) != 0) {
		if (impl_->sendEvent) event_free(impl_->sendEvent);
		impl_->sendEvent = nullptr;
		evconnlistener_free(impl_->listener);
		impl_->listener = nullptr;
		event_base_free(impl_->base);
		impl_->base = nullptr;
		return false;
	}
	impl_->heartbeatEvent = event_new(impl_->base, -1, EV_PERSIST, &Impl::heartbeat, impl_.get());
	const timeval heartbeatInterval = {1, 0};
	if (!impl_->heartbeatEvent || event_add(impl_->heartbeatEvent, &heartbeatInterval) != 0) {
		if (impl_->heartbeatEvent) event_free(impl_->heartbeatEvent);
		impl_->heartbeatEvent = nullptr;
		event_free(impl_->sendEvent);
		impl_->sendEvent = nullptr;
		evconnlistener_free(impl_->listener);
		impl_->listener = nullptr;
		event_base_free(impl_->base);
		impl_->base = nullptr;
#ifdef USE_TLS
		if (impl_->sslCtx) {
			SSL_CTX_free(impl_->sslCtx);
			impl_->sslCtx = nullptr;
		}
#endif
		impl_->tlsEnabled = false;
		return false;
	}
	impl_->stopRequested = false;
	impl_->running = true;
	try {
		impl_->worker = std::thread([this] {event_base_dispatch(impl_->base); impl_->running = false; });
	}
	catch (...) {
		impl_->running = false;
		event_free(impl_->heartbeatEvent);
		impl_->heartbeatEvent = nullptr;
		event_free(impl_->sendEvent);
		impl_->sendEvent = nullptr;
		evconnlistener_free(impl_->listener);
		event_base_free(impl_->base);
		impl_->listener = nullptr;
		impl_->base = nullptr;
		return false;
	}
	return true;
}
bool WebSocketServer::listenTLS(const std::string& address, uint16_t port, const std::string& certFile, const std::string& keyFile) {
	std::lock_guard<std::mutex> life(impl_->lifecycleMutex);
	std::lock_guard<std::mutex> lock(impl_->mutex);
	if (impl_->worker.joinable() || impl_->running || !impl_->authenticate)return false;
#ifndef USE_TLS
	(void)address;
	(void)port;
	(void)certFile;
	(void)keyFile;
	return false;
#else
	if (certFile.empty() || keyFile.empty()) return false;
	SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) return false;
	if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 || SSL_CTX_use_certificate_chain_file(ctx, certFile.c_str()) != 1 || SSL_CTX_use_PrivateKey_file(ctx, keyFile.c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
		SSL_CTX_free(ctx);
		return false;
	}
	impl_->sslCtx = ctx;
	static const bool ready = evthread_use_pthreads() == 0;
	if (!ready) {
		SSL_CTX_free(ctx);
		impl_->sslCtx = nullptr;
		return false;
	}
	sockaddr_storage addr;
	std::memset(&addr, 0, sizeof(addr));
	int len = sizeof(addr);
	if (evutil_parse_sockaddr_port((address+":"+std::to_string(port)).c_str(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
		SSL_CTX_free(ctx);
		impl_->sslCtx = nullptr;
		return false;
	}
	impl_->base = event_base_new();
	if (!impl_->base) {
		SSL_CTX_free(ctx);
		impl_->sslCtx = nullptr;
		return false;
	}
	impl_->listener = evconnlistener_new_bind(impl_->base, &Impl::accept, impl_.get(), LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, 128, reinterpret_cast<sockaddr*>(&addr), len);
	if (!impl_->listener) {
		event_base_free(impl_->base);
		impl_->base = nullptr;
		SSL_CTX_free(ctx);
		impl_->sslCtx = nullptr;
		return false;
	}
	impl_->sendEvent = event_new(impl_->base, -1, EV_PERSIST, &Impl::flushSendQueue, impl_.get());
	if (!impl_->sendEvent || event_add(impl_->sendEvent, nullptr) != 0) {
		if (impl_->sendEvent) event_free(impl_->sendEvent);
		impl_->sendEvent = nullptr;
		evconnlistener_free(impl_->listener);
		impl_->listener = nullptr;
		event_base_free(impl_->base);
		impl_->base = nullptr;
		SSL_CTX_free(ctx);
		impl_->sslCtx = nullptr;
		impl_->tlsEnabled = false;
		return false;
	}
	impl_->tlsEnabled = true;
	impl_->heartbeatEvent = event_new(impl_->base, -1, EV_PERSIST, &Impl::heartbeat, impl_.get());
	const timeval heartbeatInterval = {1, 0};
	if (!impl_->heartbeatEvent || event_add(impl_->heartbeatEvent, &heartbeatInterval) != 0) {
		if (impl_->heartbeatEvent) event_free(impl_->heartbeatEvent);
		impl_->heartbeatEvent = nullptr;
		event_free(impl_->sendEvent);
		impl_->sendEvent = nullptr;
		evconnlistener_free(impl_->listener);
		impl_->listener = nullptr;
		event_base_free(impl_->base);
		impl_->base = nullptr;
#ifdef USE_TLS
		if (impl_->sslCtx) {
			SSL_CTX_free(impl_->sslCtx);
			impl_->sslCtx = nullptr;
		}
#endif
		impl_->tlsEnabled = false;
		return false;
	}
	impl_->stopRequested = false;
	impl_->running = true;
	try {
		impl_->worker = std::thread([this] {event_base_dispatch(impl_->base); impl_->running = false; });
	}
	catch (...) {
		impl_->running = false;
		event_free(impl_->heartbeatEvent);
		impl_->heartbeatEvent = nullptr;
		event_free(impl_->sendEvent);
		impl_->sendEvent = nullptr;
		evconnlistener_free(impl_->listener);
		event_base_free(impl_->base);
		impl_->listener = nullptr;
		impl_->base = nullptr;
		SSL_CTX_free(ctx);
		impl_->sslCtx = nullptr;
		impl_->tlsEnabled = false;
		return false;
	}
	return true;
#endif
}
void WebSocketServer::stop() {
	// stop() must be called by the owner, never from a server callback.
	if (impl_->worker.joinable() && impl_->worker.get_id() == std::this_thread::get_id()) return;
	std::lock_guard<std::mutex> life(impl_->lifecycleMutex);
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (!impl_->worker.joinable()) return;
		impl_->running = false;
		impl_->stopRequested.store(true, std::memory_order_release);
		// The callback runs on the event thread, including connection teardown.
		event_active(impl_->sendEvent, EV_TIMEOUT, 1);
	}
	impl_->worker.join();
	// No connection objects or bufferevents should remain here.
	if (impl_->heartbeatEvent) {
		event_free(impl_->heartbeatEvent);
		impl_->heartbeatEvent = nullptr;
	}
	if (impl_->sendEvent) {
		event_free(impl_->sendEvent);
		impl_->sendEvent = nullptr;
	}
	if (impl_->listener) {
		evconnlistener_free(impl_->listener);
		impl_->listener = nullptr;
	}
	event_base_free(impl_->base);
	impl_->base = nullptr;
#ifdef USE_TLS
	if (impl_->sslCtx) {
		SSL_CTX_free(impl_->sslCtx);
		impl_->sslCtx = nullptr;
	}
#endif
	impl_->tlsEnabled = false;
	impl_->stopRequested = false;
}
bool WebSocketServer::isRunning()const {
	return impl_->running.load();
}
bool WebSocketServer::send(ConnectionId id, const std::string& payload, bool binary) {
	if (payload.size()>MAX_MESSAGE)return false;
	return impl_->enqueue(id, frame(binary?2:1, payload), false);
}
void WebSocketServer::broadcast(const std::string& payload, bool binary) {
	if (payload.size()>MAX_MESSAGE)return;
	impl_->enqueue(0, frame(binary?2:1, payload), true);
}
void WebSocketServer::onConnect(ConnectCallback cb) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->connected = std::move(cb);
}
void WebSocketServer::onMessage(MessageCallback cb) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->message = std::move(cb);
}
void WebSocketServer::onDisconnect(DisconnectCallback cb) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->disconnected = std::move(cb);
}
void WebSocketServer::setAuthCallback(AuthCallback cb) {
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->authenticate = std::move(cb);
}
