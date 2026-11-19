#include "udp.hpp"
#include "cursor_packet.hpp"
#include "secure.hpp"
#include "wire.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace udp {
namespace {

using Host = std::unique_ptr<ENetHost, decltype(&enet_host_destroy)>;
using Packet = std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr enet_uint32 protocol_id = 0x4d584455; // MXDU, ENet connect handshake
constexpr enet_uint32 encrypted_protocol_id = 0x4d584445; // MXDE
constexpr enet_uint32 password_mode_mismatch = 0x4d585057; // MXPW, disconnect reason
constexpr std::size_t backlog_bytes = 2 * wire::max_frame_bytes;
constexpr std::size_t backlog_messages = 3;
enum class Delivery { control, keyframe, delta };

struct Runtime {
    Runtime() {
        if (enet_initialize() != 0) throw std::runtime_error("Could not initialize UDP networking");
    }
    ~Runtime() { enet_deinitialize(); }
};

void initialize() { static Runtime runtime; }

ENetAddress address_for(const std::string& host, std::uint16_t port) {
    ENetAddress result{ENET_HOST_ANY, port};
    if (host.empty() || enet_address_set_host(&result, host.c_str()) != 0)
        throw std::runtime_error("Cannot resolve UDP IPv4 address: " + host);
    return result;
}

Host make_host(const ENetAddress* bind, bool encrypted) {
    Host host(enet_host_create(bind, 1, 1, 0, 0), enet_host_destroy);
    if (!host) throw std::runtime_error("Cannot open UDP socket (is the address already in use?)");
    // Leave room for a burst of video fragments while the receiver is decoding.
    // ENet has already installed its defaults if the OS declines the larger buffer.
    enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, 1024 * 1024);
    host->maximumPacketSize = wire::max_frame_bytes + (encrypted ? secure::record_overhead : 0);
    host->maximumWaitingData = backlog_bytes;
    return host;
}

void configure_peer(ENetPeer* peer) {
    // StreamControl responds to delivery feedback. Keep ENet from silently
    // discarding dependent pictures on RTT spikes as well. Configure only
    // after CONNECT so both peers accept the throttle configuration command.
    enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                 ENET_PEER_PACKET_THROTTLE_SCALE, 0);
    enet_peer_ping_interval(peer, 500);
    enet_peer_timeout(peer, ENET_PEER_TIMEOUT_LIMIT, 2000, 6000);
}

// Undeliverable/unsolicited messages are still owned by the caller of service().
ENetEvent poll(ENetHost* host, unsigned timeout_ms) {
    ENetEvent event{};
    if (enet_host_service(host, &event, timeout_ms) < 0)
        throw std::runtime_error("UDP socket service failed");
    return event;
}

unsigned remaining(Clock::time_point deadline) {
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<unsigned>(left) : 0;
}

void send_packet(ENetPeer* peer, const secure::Bytes& bytes, enet_uint32 flags) {
    Packet packet(enet_packet_create(bytes.data(), bytes.size(), flags), enet_packet_destroy);
    if (!packet || enet_peer_send(peer, 0, packet.get()) != 0)
        throw std::runtime_error("Could not send UDP protocol message");
    packet.release();
    enet_host_flush(peer->host);
}

secure::Bytes receive_handshake(ENetHost* host, std::size_t size, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        const auto event = poll(host, std::min(100u, remaining(deadline)));
        Packet packet(event.packet, enet_packet_destroy);
        if (event.type == ENET_EVENT_TYPE_DISCONNECT)
            throw std::runtime_error("UDP authentication disconnected. Check --password on both devices.");
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            if (!packet || packet->dataLength != size)
                throw std::runtime_error("Invalid encrypted UDP handshake");
            return {packet->data, packet->data + size};
        }
    }
    throw std::runtime_error("UDP authentication timed out or was stopped");
}

} // namespace

struct Sender::Impl {
    Host host{nullptr, enet_host_destroy};
    ENetPeer* peer = nullptr;
    std::unique_ptr<secure::Session> session; // Video records are sealed/opened only by the worker.
    net::Endpoint remote;
    std::mutex mutex;
    struct Message {
        std::vector<std::uint8_t> bytes;
        Delivery delivery;
        secure::Kind kind;
    };
    std::deque<Message> queue;
    std::optional<std::vector<std::uint8_t>> feedback;
    std::condition_variable reply_ready;
    std::optional<secure::Kind> awaiting;
    std::optional<secure::Bytes> reply;
    std::size_t outstanding_bytes = 0;
    std::size_t outstanding_messages = 0;
    bool key_in_flight = false;
    std::exception_ptr error;
    std::atomic<bool> stopping{false};
    std::atomic<bool> closed{false};
    std::thread worker;

    Impl(const std::string& name, std::uint16_t port, const std::string& password) {
        initialize();
        const auto address = address_for(name, port);
        host = make_host(nullptr, !password.empty());
        peer = enet_host_connect(host.get(), &address, 1, password.empty() ? protocol_id : encrypted_protocol_id);
        if (!peer) throw std::runtime_error("Cannot start UDP connection");
        const auto event = poll(host.get(), 5000);
        Packet unexpected(event.packet, enet_packet_destroy);
        if (event.type != ENET_EVENT_TYPE_CONNECT) {
            enet_peer_reset(peer);
            throw std::runtime_error("UDP connection failed. Check receiver transport, address and firewall.");
        }
        configure_peer(peer);
        remote = net::Endpoint::ipv4(peer->address.host, peer->address.port);
        if (!password.empty()) {
            try {
                const auto deadline = Clock::now() + wire::header_timeout;
                const auto sender_hello = secure::make_hello(secure::Role::sender);
                send_packet(peer, sender_hello, ENET_PACKET_FLAG_RELIABLE);
                const auto receiver_hello = receive_handshake(host.get(), secure::hello_size, deadline);
                secure::validate_hello(receiver_hello, secure::Role::receiver);
                const auto proof = receive_handshake(host.get(), secure::record_overhead, deadline);
                session = std::make_unique<secure::Session>(password, sender_hello, receiver_hello, secure::Role::sender);
                session->open(secure::Kind::confirm, proof);
                send_packet(peer, session->seal(secure::Kind::confirm, {}), ENET_PACKET_FLAG_RELIABLE);
            } catch (...) {
                enet_peer_disconnect_now(peer, 0);
                throw;
            }
        }
        // Connect on the calling thread, then transfer exclusive ENet ownership
        // to this worker. Keep servicing acknowledgments even on a static desktop.
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        stopping = true;
        if (worker.joinable()) worker.join();
    }

    bool enqueue(const std::vector<std::uint8_t>& bytes, Delivery delivery, secure::Kind kind) {
        if (bytes.empty() || bytes.size() > wire::max_frame_bytes)
            throw std::runtime_error("UDP message is outside protocol size limits");
        std::lock_guard lock(mutex);
        if (error) std::rethrow_exception(error);
        if (stopping || closed) throw std::runtime_error("UDP connection closed");
        if (delivery == Delivery::keyframe && key_in_flight) return false;
        const auto wire_size = bytes.size() + (session ? secure::record_overhead : 0);
        if (outstanding_messages >= backlog_messages || wire_size > backlog_bytes - outstanding_bytes) {
            if (delivery == Delivery::control) throw std::runtime_error("UDP control message queue is full");
            return false;
        }
        queue.push_back({bytes, delivery, kind});
        if (delivery == Delivery::keyframe) key_in_flight = true;
        ++outstanding_messages;
        outstanding_bytes += wire_size;
        return true;
    }

    secure::Bytes exchange_query(secure::Kind query_kind, const secure::Bytes& query,
                                 secure::Kind reply_kind, unsigned timeout_ms) {
        {
            std::lock_guard lock(mutex);
            if (awaiting) throw std::logic_error("Another UDP query is in progress");
            awaiting = reply_kind;
        }
        enqueue(query, Delivery::control, query_kind);
        std::unique_lock lock(mutex);
        if (!reply_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [&] { return reply || closed || stopping; }))
            throw std::runtime_error("Waiting for UDP reply timed out");
        if (error) std::rethrow_exception(error);
        if (!reply) throw std::runtime_error("Receiver disconnected");
        awaiting.reset();
        auto bytes = std::move(*reply);
        reply.reset();
        return bytes;
    }

    static void release(ENetPacket* packet, bool keyframe) {
        // Count queued and in-flight packets until ENet releases them.
        auto* self = static_cast<Impl*>(packet->userData);
        std::lock_guard lock(self->mutex);
        --self->outstanding_messages;
        self->outstanding_bytes -= packet->dataLength;
        if (keyframe) self->key_in_flight = false;
    }
    static void ENET_CALLBACK released(ENetPacket* packet) { release(packet, false); }
    static void ENET_CALLBACK key_released(ENetPacket* packet) { release(packet, true); }

    void submit_pending() {
        std::deque<Message> pending;
        {
            std::lock_guard lock(mutex);
            pending.swap(queue);
        }
        for (auto& message : pending) {
            if (session) message.bytes = session->seal(message.kind, message.bytes);
            const auto flags = message.delivery == Delivery::delta ? ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT
                                                                   : ENET_PACKET_FLAG_RELIABLE;
            Packet packet(enet_packet_create(message.bytes.data(), message.bytes.size(), flags),
                          enet_packet_destroy);
            if (!packet) throw std::runtime_error("Cannot allocate UDP video packet");
            packet->userData = this;
            packet->freeCallback = message.delivery == Delivery::keyframe ? key_released : released;
            // Keep each GOP's dependent pictures ordered after its sync picture.
            if (enet_peer_send(peer, 0, packet.get()) != 0)
                throw std::runtime_error("Could not send UDP video packet");
            packet.release();
        }
    }

    void run() noexcept {
        try {
            auto close_deadline = Clock::time_point::max();
            while (true) {
                // Observe shutdown before draining so it cannot bypass an enqueue
                // already waiting for the mutex.
                const bool closing = stopping.load();
                submit_pending();
                if (closing && close_deadline == Clock::time_point::max()) {
                    enet_peer_disconnect_later(peer, 0);
                    close_deadline = Clock::now() + 2s;
                }
                const auto event = poll(host.get(), 2);
                Packet packet(event.packet, enet_packet_destroy);
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    if (event.data == password_mode_mismatch)
                        throw std::runtime_error("UDP password mode mismatch. Check --password on both devices.");
                    break;
                }
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    std::lock_guard lock(mutex);
                    const auto kind = awaiting.value_or(secure::Kind::feedback);
                    auto expected_size = wire::feedback_size;
                    if (kind == secure::Kind::cursor_reply) expected_size = wire::cursor_reply_size;
                    if (kind == secure::Kind::display_reply) expected_size = wire::display_reply_size;
                    if (!packet || packet->dataLength != expected_size + (session ? secure::record_overhead : 0))
                        throw std::runtime_error("Invalid UDP receiver reply length");
                    secure::Bytes bytes(packet->data, packet->data + packet->dataLength);
                    if (session) bytes = session->open(kind, bytes);
                    if (awaiting) {
                        if (reply) throw std::runtime_error("Duplicate UDP query reply");
                        reply = std::move(bytes);
                        reply_ready.notify_one();
                    } else {
                        wire::unpack_feedback(bytes);
                        feedback = std::move(bytes);
                    }
                }
                if (Clock::now() >= close_deadline) break;
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            error = std::current_exception();
        }
        {
            std::lock_guard lock(mutex);
            closed = true;
        }
        reply_ready.notify_one();
        enet_peer_disconnect_now(peer, 0);
        host.reset(); // release packet callbacks while their counters and mutex exist
    }
};

Sender::Sender(const std::string& host, std::uint16_t port, const std::string& password)
    : impl_(std::make_unique<Impl>(host, port, password)) {}
Sender::~Sender() = default;
net::Endpoint Sender::peer_endpoint() const { return impl_->remote; }
std::unique_ptr<secure::Datagrams> Sender::datagrams() { return impl_->session ? impl_->session->datagrams() : nullptr; }

wire::CursorOffer Sender::request_cursor() {
    return wire::decode_cursor_reply(impl_->exchange_query(secure::Kind::cursor_query, wire::cursor_query,
                                                           secure::Kind::cursor_reply, wire::display_query_timeout_ms));
}

wire::DisplaySize Sender::request_display_size(unsigned timeout_ms) {
    try {
        return wire::decode_display_reply(impl_->exchange_query(secure::Kind::display_query, wire::display_query,
                                                                secure::Kind::display_reply, timeout_ms));
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(std::string(wire::display_query_error) + e.what());
    }
}
void Sender::send_header(const wire::Header& header) {
    impl_->enqueue(wire::encode_header(header), Delivery::control, secure::Kind::header);
}
bool Sender::send_frame(const std::vector<std::uint8_t>& message, bool keyframe) {
    return impl_->enqueue(message, keyframe ? Delivery::keyframe : Delivery::delta, secure::Kind::frame);
}
std::optional<std::vector<std::uint8_t>> Sender::take_feedback() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->error) std::rethrow_exception(impl_->error);
    return std::exchange(impl_->feedback, std::nullopt);
}
bool Sender::peer_closed() const {
    if (!impl_->closed) return impl_->stopping;
    std::lock_guard lock(impl_->mutex);
    if (impl_->error) std::rethrow_exception(impl_->error);
    return true;
}
void Sender::shutdown() noexcept { impl_->stopping = true; }

struct Listener::Impl {
    enum class Video { idle, pending, accepted };
    Host host{nullptr, enet_host_destroy};
    ENetPeer* peer = nullptr;
    Video video = Video::idle;
    bool header_read = false;
    net::Endpoint video_peer;
    std::deque<Packet> inbox;
    std::size_t queued_bytes = 0;
    std::string password;
    std::unique_ptr<secure::Session> session; // Owned by the video consumer.

    struct CursorPacket { net::Endpoint from; secure::Bytes payload; };
    std::deque<CursorPacket> cursor_queue;
    net::Endpoint cursor_peer;
    std::array<std::uint8_t, 8> cursor_token{};
    std::optional<net::Endpoint> reply_to;
    bool cursor_open = false;

    std::mutex mutex;
    std::condition_variable video_ready, cursor_ready;
    std::exception_ptr error;
    std::atomic<bool> stopping{false};
    std::thread worker;
    // ENet's intercept callback has no user data. Only our worker calls service.
    inline static thread_local Impl* servicing = nullptr;

    Impl(const std::string& name, std::uint16_t port, const std::string& shared_password)
        : password(shared_password) {
        initialize();
        const auto bind = address_for(name, port);
        host = make_host(&bind, !password.empty());
        host->intercept = intercept;
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        stopping = true;
        worker.join();
        if (peer) enet_peer_disconnect_now(peer, 0);
    }

    static int ENET_CALLBACK intercept(ENetHost* host, ENetEvent*) {
        const auto* data = host->receivedData;
        const auto size = host->receivedDataLength;
        if (!cursor::packet::matches(data, size)) return 0;
        auto& self = *servicing;
        const auto prefix = cursor::packet::prefix_size;
        const auto from = net::Endpoint::ipv4(host->receivedAddress.host, host->receivedAddress.port);
        if (!self.cursor_open || !from.same_address(self.cursor_peer) ||
            size < prefix + self.cursor_token.size() || size > net::max_datagram_size ||
            !std::equal(self.cursor_token.begin(), self.cursor_token.end(), data + prefix)) return 1;
        // Raw coordinates supersede older ones. Shape fragments are retransmitted.
        if (self.cursor_queue.size() == 64) self.cursor_queue.pop_front();
        self.cursor_queue.push_back({from, {data + prefix, data + size}});
        self.cursor_ready.notify_one();
        return 1;
    }

    void handle_event(ENetEvent event) {
        Packet packet(event.packet, enet_packet_destroy);
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (event.data != (password.empty() ? protocol_id : encrypted_protocol_id)) {
                enet_peer_disconnect_now(event.peer, password_mode_mismatch);
            } else if (video != Video::idle) {
                enet_peer_disconnect_now(event.peer, 0);
            } else {
                peer = event.peer;
                configure_peer(peer);
                video_peer = net::Endpoint::ipv4(peer->address.host, peer->address.port);
                video = Video::pending;
                header_read = false;
            }
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (peer != event.peer) break;
            // Bound assembled packets while the video consumer is stalled. A dropped
            // delta is recovered via the normal sequence gap / keyframe feedback.
            if (inbox.size() >= 128 || packet->dataLength > backlog_bytes - queued_bytes) {
                if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) break;
                if (!header_read) {
                    enet_peer_disconnect_now(peer, 0);
                    peer = nullptr;
                    break;
                }
                // Make room for a new reliable picture (normally a keyframe).
                inbox.clear();
                queued_bytes = 0;
            }
            queued_bytes += packet->dataLength;
            inbox.push_back(std::move(packet));
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (event.peer == peer) peer = nullptr;
            break;
        default:
            return;
        }
        video_ready.notify_one();
    }

    void run() noexcept {
        servicing = this;
        try {
            while (!stopping) {
                {
                    std::lock_guard lock(mutex);
                    const auto event = poll(host.get(), 0);
                    if (event.type != ENET_EVENT_TYPE_NONE) {
                        handle_event(event);
                        continue;
                    }
                }
                // Wake on ingress, and service ENet's retransmission timers during silence.
                enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE | ENET_SOCKET_WAIT_INTERRUPT;
                if (enet_socket_wait(host->socket, &wait, 10) < 0)
                    throw std::runtime_error("Waiting for the UDP socket failed");
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            error = std::current_exception();
            video_ready.notify_all();
            cursor_ready.notify_all();
        }
        servicing = nullptr;
    }

    ReadResult read_packet(secure::Bytes& bytes, unsigned timeout_ms, std::size_t limit) {
        Packet packet(nullptr, enet_packet_destroy);
        {
            std::unique_lock lock(mutex);
            if (video != Video::accepted) throw std::logic_error("No active UDP session");
            video_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [&] { return !inbox.empty() || !peer || error; });
            if (error) std::rethrow_exception(error);
            if (inbox.empty()) {
                if (peer) return ReadResult::timeout;
                video = Video::idle;
                session.reset();
                return ReadResult::disconnected;
            }
            packet = std::move(inbox.front());
            inbox.pop_front();
            queued_bytes -= packet->dataLength;
        }
        if (!packet->dataLength || packet->dataLength > limit)
            throw std::runtime_error("UDP message is outside protocol size limits");
        bytes.assign(packet->data, packet->data + packet->dataLength);
        return ReadResult::message;
    }

    void send(const secure::Bytes& bytes, enet_uint32 flags, bool feedback = false) {
        std::lock_guard lock(mutex);
        if (error) std::rethrow_exception(error);
        // A graceful disconnect can arrive while the decoder drains the last queued pictures.
        if (feedback && !peer && video == Video::accepted) return;
        if (!peer || video != Video::accepted) throw std::runtime_error("UDP peer disconnected");
        send_packet(peer, bytes, flags);
    }

    void authenticate(Clock::time_point deadline, const std::function<bool()>& stop) {
        auto handshake = [&](std::size_t size) {
            secure::Bytes bytes;
            while ((!stop || !stop()) && Clock::now() < deadline) {
                const auto result = read_packet(bytes, std::min(100u, remaining(deadline)), size);
                if (result == ReadResult::timeout) continue;
                if (result == ReadResult::disconnected || bytes.size() != size)
                    throw std::runtime_error("UDP authentication failed. Check --password on both devices.");
                return bytes;
            }
            throw std::runtime_error("UDP authentication timed out or was stopped");
        };
        const auto sender_hello = handshake(secure::hello_size);
        secure::validate_hello(sender_hello, secure::Role::sender);
        const auto receiver_hello = secure::make_hello(secure::Role::receiver);
        send(receiver_hello, ENET_PACKET_FLAG_RELIABLE);
        auto candidate = std::make_unique<secure::Session>(password, sender_hello, receiver_hello, secure::Role::receiver);
        send(candidate->seal(secure::Kind::confirm, {}), ENET_PACKET_FLAG_RELIABLE);
        candidate->open(secure::Kind::confirm, handshake(secure::record_overhead));
        session = std::move(candidate);
    }

    void reply(secure::Kind kind, const secure::Bytes& bytes, enet_uint32 flags) {
        if (!password.empty() && !session) throw std::logic_error("UDP session is not authenticated");
        if (session) send(session->seal(kind, bytes), flags, kind == secure::Kind::feedback);
        else send(bytes, flags, kind == secure::Kind::feedback);
    }
};

Listener::Listener(const std::string& bind_address, std::uint16_t port, const std::string& password)
    : impl_(std::make_unique<Impl>(bind_address, port, password)) {}
Listener::~Listener() = default;
std::uint16_t Listener::local_port() const { return impl_->host->address.port; }

void Listener::open_cursor(net::Endpoint video_peer, std::uint64_t token) {
    if (video_peer.v6 || !token) throw std::invalid_argument("Invalid IPv4 cursor session");
    std::lock_guard lock(impl_->mutex);
    impl_->cursor_peer = video_peer;
    for (auto i = impl_->cursor_token.size(); i > 0; --i) {
        impl_->cursor_token[i - 1] = static_cast<std::uint8_t>(token);
        token >>= 8;
    }
    impl_->cursor_queue.clear();
    impl_->reply_to.reset();
    impl_->cursor_open = true;
}

void Listener::close_cursor() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->cursor_open = false;
    impl_->cursor_queue.clear();
    impl_->reply_to.reset();
    impl_->cursor_ready.notify_all();
}

bool Listener::receive_cursor(secure::Bytes& inner, net::Endpoint& from, unsigned timeout_ms) {
    std::unique_lock lock(impl_->mutex);
    impl_->cursor_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [&] { return !impl_->cursor_queue.empty() || !impl_->cursor_open || impl_->error; });
    if (impl_->error) std::rethrow_exception(impl_->error);
    if (impl_->cursor_queue.empty()) return false;
    auto item = std::move(impl_->cursor_queue.front());
    impl_->cursor_queue.pop_front();
    impl_->reply_to = from = item.from;
    inner = std::move(item.payload);
    return true;
}

bool Listener::send_cursor(const secure::Bytes& inner, net::Endpoint to) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->reply_to || *impl_->reply_to != to || inner.size() < impl_->cursor_token.size() ||
        !std::equal(impl_->cursor_token.begin(), impl_->cursor_token.end(), inner.begin())) return false;
    auto bytes = cursor::packet::wrap(inner);
    ENetAddress address{};
    std::memcpy(&address.host, to.address.data(), sizeof(address.host));
    address.port = to.port;
    ENetBuffer buffer{};
    buffer.data = bytes.data();
    buffer.dataLength = bytes.size();
    return enet_socket_send(impl_->host->socket, &address, &buffer, 1) == static_cast<int>(bytes.size());
}

net::Endpoint Listener::peer_endpoint() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->video != Impl::Video::accepted) throw std::logic_error("No active UDP session");
    return impl_->video_peer;
}

std::unique_ptr<secure::Datagrams> Listener::datagrams() {
    if (!impl_->password.empty() && !impl_->session) throw std::logic_error("UDP session is not authenticated");
    return impl_->session ? impl_->session->datagrams() : nullptr;
}

bool Listener::accept(unsigned timeout_ms) {
    std::unique_lock lock(impl_->mutex);
    if (impl_->video == Impl::Video::accepted) throw std::logic_error("UDP session already active");
    impl_->video_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [&] { return impl_->video == Impl::Video::pending || impl_->error; });
    if (impl_->error) std::rethrow_exception(impl_->error);
    if (impl_->video != Impl::Video::pending) return false;
    impl_->video = Impl::Video::accepted;
    return true;
}

ReadResult Listener::receive(std::vector<std::uint8_t>& message, unsigned timeout_ms) {
    if (!impl_->password.empty() && !impl_->session) throw std::logic_error("UDP session is not authenticated");
    const auto limit = wire::max_frame_bytes + (impl_->session ? secure::record_overhead : 0);
    const auto result = impl_->read_packet(message, timeout_ms, limit);
    if (result == ReadResult::message && impl_->session) message = impl_->session->open(secure::Kind::frame, message);
    return result;
}

wire::Header Listener::receive_header(const std::function<wire::DisplaySize()>& display_size,
                                      const std::function<bool()>& stopping, const wire::CursorResponder& cursor) {
    const auto deadline = Clock::now() + wire::header_timeout;
    if (!impl_->password.empty()) impl_->authenticate(deadline, stopping);
    std::vector<std::uint8_t> bytes;
    bool cursor_answered = false, display_answered = false;
    while ((!stopping || !stopping()) && Clock::now() < deadline) {
        const auto limit = wire::header_size + (impl_->session ? secure::record_overhead : 0);
        const auto result = impl_->read_packet(bytes, std::min(100u, remaining(deadline)), limit);
        if (result == ReadResult::timeout) continue;
        if (result == ReadResult::disconnected)
            throw std::runtime_error("UDP peer disconnected before sending its stream header");
        secure::Kind kind = secure::Kind::header;
        if (impl_->session) {
            kind = static_cast<secure::Kind>(bytes.front());
            bytes = impl_->session->open(kind, bytes);
        } else if (bytes == wire::cursor_query) {
            kind = secure::Kind::cursor_query;
        } else if (bytes == wire::display_query) {
            kind = secure::Kind::display_query;
        }
        if (kind == secure::Kind::cursor_query && bytes == wire::cursor_query && !cursor_answered && !display_answered) {
            impl_->reply(secure::Kind::cursor_reply, wire::encode_cursor_reply(cursor ? cursor() : wire::CursorOffer{}),
                         ENET_PACKET_FLAG_RELIABLE);
            cursor_answered = true;
        } else if (kind == secure::Kind::display_query && bytes == wire::display_query && display_size && !display_answered) {
            impl_->reply(secure::Kind::display_reply, wire::encode_display_reply(display_size()), ENET_PACKET_FLAG_RELIABLE);
            display_answered = true;
        } else if (kind == secure::Kind::header) {
            auto header = wire::decode_header(bytes);
            std::lock_guard lock(impl_->mutex);
            impl_->header_read = true;
            return header;
        } else {
            throw std::runtime_error("Unexpected UDP setup message");
        }
    }
    throw std::runtime_error("Waiting for UDP stream header timed out or was stopped");
}

void Listener::disconnect() noexcept {
    impl_->session.reset();
    std::lock_guard lock(impl_->mutex);
    if (impl_->video != Impl::Video::accepted) return;
    if (impl_->peer) enet_peer_disconnect_now(impl_->peer, 0);
    impl_->peer = nullptr;
    impl_->video = Impl::Video::idle;
    impl_->inbox.clear();
    impl_->queued_bytes = 0;
}

void Listener::send_feedback(const std::vector<std::uint8_t>& message) {
    wire::unpack_feedback(message);
    impl_->reply(secure::Kind::feedback, message, 0);
}

} // namespace udp
