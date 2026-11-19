#include "udp.hpp"
#include "wire.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
constexpr std::size_t backlog_bytes = 2 * wire::max_frame_bytes;
constexpr std::size_t backlog_messages = 32;
constexpr std::size_t live_backlog_messages = 3;
enum class Delivery { reliable, keyframe, delta };

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

Host make_host(const ENetAddress* bind) {
    Host host(enet_host_create(bind, 1, 1, 0, 0), enet_host_destroy);
    if (!host) throw std::runtime_error("Cannot open UDP socket (is the address already in use?)");
    // Leave room for a burst of video fragments while the receiver is decoding.
    // ENet has already installed its defaults if the OS declines the larger buffer.
    enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, 1024 * 1024);
    host->maximumPacketSize = wire::max_frame_bytes;
    host->maximumWaitingData = backlog_bytes;
    return host;
}

void configure_peer(ENetPeer* peer) {
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

} // namespace

struct Sender::Impl {
    Host host{nullptr, enet_host_destroy};
    ENetPeer* peer = nullptr;
    std::mutex mutex;
    struct Message {
        std::vector<std::uint8_t> bytes;
        Delivery delivery;
    };
    std::deque<Message> queue;
    std::optional<std::vector<std::uint8_t>> feedback;
    std::condition_variable display_ready;
    bool awaiting_display = false;
    std::optional<wire::DisplaySize> display;
    std::size_t outstanding_bytes = 0;
    std::size_t outstanding_messages = 0;
    bool key_in_flight = false;
    std::exception_ptr error;
    std::atomic<bool> stopping{false};
    std::atomic<bool> closed{false};
    std::thread worker;

    Impl(const std::string& name, std::uint16_t port) {
        initialize();
        const auto address = address_for(name, port);
        host = make_host(nullptr);
        peer = enet_host_connect(host.get(), &address, 1, protocol_id);
        if (!peer) throw std::runtime_error("Cannot start UDP connection");
        configure_peer(peer);
        const auto event = poll(host.get(), 5000);
        Packet unexpected(event.packet, enet_packet_destroy);
        if (event.type != ENET_EVENT_TYPE_CONNECT) {
            enet_peer_reset(peer);
            throw std::runtime_error("UDP connection failed. Check receiver transport, address and firewall.");
        }
        // Connect on the calling thread, then transfer exclusive ENet ownership
        // to this worker. Keep servicing acknowledgments even on a static desktop.
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        stopping = true;
        if (worker.joinable()) worker.join();
    }

    bool enqueue(const std::vector<std::uint8_t>& bytes, Delivery delivery) {
        if (bytes.empty() || bytes.size() > wire::max_frame_bytes)
            throw std::runtime_error("UDP message is outside protocol size limits");
        std::lock_guard lock(mutex);
        if (error) std::rethrow_exception(error);
        if (stopping || closed) throw std::runtime_error("UDP connection closed");
        const bool live = delivery != Delivery::reliable;
        if (delivery == Delivery::keyframe && key_in_flight) return false;
        if (outstanding_messages >= (live ? live_backlog_messages : backlog_messages) ||
            bytes.size() > backlog_bytes - outstanding_bytes) {
            if (!live) throw std::runtime_error("UDP receiver is too slow. Try a lower bitrate or frame rate.");
            return false;
        }
        queue.push_back({bytes, delivery});
        if (delivery == Delivery::keyframe) key_in_flight = true;
        ++outstanding_messages;
        outstanding_bytes += bytes.size();
        return true;
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
        for (const auto& message : pending) {
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
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    if (!packet || (packet->dataLength != wire::feedback_size &&
                                    packet->dataLength != wire::display_reply_size))
                        throw std::runtime_error("Invalid UDP receiver reply length");
                    std::vector<std::uint8_t> bytes(packet->data, packet->data + packet->dataLength);
                    std::lock_guard lock(mutex);
                    if (awaiting_display) {
                        display = wire::decode_display_reply(bytes);
                        awaiting_display = false;
                        display_ready.notify_one();
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
        display_ready.notify_one();
        enet_peer_disconnect_now(peer, 0);
        host.reset(); // release packet callbacks while their counters and mutex exist
    }
};

Sender::Sender(const std::string& host, std::uint16_t port) : impl_(std::make_unique<Impl>(host, port)) {}
Sender::~Sender() = default;
wire::DisplaySize Sender::request_display_size(unsigned timeout_ms) {
    try {
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->awaiting_display || impl_->display)
                throw std::logic_error("Display resolution was already requested");
            impl_->awaiting_display = true;
        }
        send(wire::display_query);
        std::unique_lock lock(impl_->mutex);
        if (!impl_->display_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                [&] { return impl_->display || impl_->closed || impl_->stopping; }))
            throw std::runtime_error("Waiting for UDP reply timed out");
        if (impl_->error) std::rethrow_exception(impl_->error);
        if (!impl_->display) throw std::runtime_error("Receiver disconnected");
        return *impl_->display;
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(std::string(wire::display_query_hint) + e.what());
    }
}
void Sender::send(const std::vector<std::uint8_t>& message) {
    impl_->enqueue(message, Delivery::reliable);
}
bool Sender::send_frame(const std::vector<std::uint8_t>& message, bool keyframe) {
    return impl_->enqueue(message, keyframe ? Delivery::keyframe : Delivery::delta);
}
std::optional<std::vector<std::uint8_t>> Sender::take_feedback() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->error) std::rethrow_exception(impl_->error);
    return std::exchange(impl_->feedback, std::nullopt);
}
bool Sender::peer_closed() const { return impl_->closed || impl_->stopping; }
void Sender::shutdown() noexcept { impl_->stopping = true; }

struct Listener::Impl {
    Host host{nullptr, enet_host_destroy};
    ENetPeer* peer = nullptr;
    Impl(const std::string& name, std::uint16_t port) {
        initialize();
        const auto bind = address_for(name, port);
        host = make_host(&bind);
    }
    void reply(const std::vector<std::uint8_t>& message, enet_uint32 flags) {
        if (!peer) throw std::logic_error("No active UDP session");
        Packet packet(enet_packet_create(message.data(), message.size(), flags), enet_packet_destroy);
        if (!packet || enet_peer_send(peer, 0, packet.get()) != 0)
            throw std::runtime_error("Could not send UDP receiver reply");
        packet.release();
        enet_host_flush(host.get());
    }
};

Listener::Listener(const std::string& bind_address, std::uint16_t port)
    : impl_(std::make_unique<Impl>(bind_address, port)) {}
Listener::~Listener() { disconnect(); }
std::uint16_t Listener::local_port() const { return impl_->host->address.port; }

bool Listener::accept(unsigned timeout_ms) {
    if (impl_->peer) throw std::logic_error("UDP session already active");
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        const auto event = poll(impl_->host.get(), remaining(deadline));
        Packet unexpected(event.packet, enet_packet_destroy);
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            if (event.data == protocol_id) {
                impl_->peer = event.peer;
                configure_peer(event.peer);
                return true;
            }
            enet_peer_disconnect_now(event.peer, 0);
        }
    } while (Clock::now() < deadline);
    return false;
}

ReadResult Listener::receive(std::vector<std::uint8_t>& message, unsigned timeout_ms) {
    if (!impl_->peer) throw std::logic_error("No active UDP session");
    const auto event = poll(impl_->host.get(), timeout_ms);
    Packet packet(event.packet, enet_packet_destroy);
    switch (event.type) {
    case ENET_EVENT_TYPE_RECEIVE:
        if (!packet || packet->dataLength == 0)
            throw std::runtime_error("Received an empty UDP video message");
        message.assign(packet->data, packet->data + packet->dataLength);
        return ReadResult::message;
    case ENET_EVENT_TYPE_DISCONNECT:
        impl_->peer = nullptr;
        return ReadResult::disconnected;
    default:
        return ReadResult::timeout;
    }
}

wire::Header Listener::receive_header(const std::function<wire::DisplaySize()>& display_size,
                                      const std::function<bool()>& stopping) {
    const auto deadline = Clock::now() + wire::header_timeout;
    std::vector<std::uint8_t> bytes;
    bool answered = false;
    while ((!stopping || !stopping()) && Clock::now() < deadline) {
        const auto result = receive(bytes, std::min(100u, remaining(deadline)));
        if (result == ReadResult::timeout) continue;
        if (result == ReadResult::disconnected)
            throw std::runtime_error("UDP peer disconnected before sending its stream header");
        if (!answered && bytes == wire::display_query && display_size) {
            impl_->reply(wire::encode_display_reply(display_size()), ENET_PACKET_FLAG_RELIABLE);
            answered = true;
        } else {
            return wire::decode_header(bytes);
        }
    }
    throw std::runtime_error("Waiting for UDP stream header timed out or was stopped");
}

void Listener::disconnect() noexcept {
    if (impl_->peer) {
        enet_peer_disconnect_now(impl_->peer, 0);
        impl_->peer = nullptr;
    }
}

void Listener::send_feedback(const std::vector<std::uint8_t>& message) {
    wire::unpack_feedback(message);
    impl_->reply(message, 0);
}

} // namespace udp
