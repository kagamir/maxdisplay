#include "udp.hpp"
#include "wire.hpp"

#include <enet/enet.h>

#include <atomic>
#include <chrono>
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
    std::deque<std::vector<std::uint8_t>> queue;
    std::size_t outstanding_bytes = 0;
    std::size_t outstanding_messages = 0;
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

    void enqueue(const std::vector<std::uint8_t>& bytes) {
        if (bytes.empty() || bytes.size() > wire::max_frame_bytes)
            throw std::runtime_error("UDP message is outside protocol size limits");
        std::lock_guard lock(mutex);
        if (error) std::rethrow_exception(error);
        if (stopping || closed) throw std::runtime_error("UDP connection closed");
        if (outstanding_messages >= backlog_messages || bytes.size() > backlog_bytes - outstanding_bytes)
            throw std::runtime_error("UDP receiver is too slow. Try a lower bitrate or frame rate.");
        queue.push_back(bytes);
        ++outstanding_messages;
        outstanding_bytes += bytes.size();
    }

    static void ENET_CALLBACK acknowledged(ENetPacket* packet) {
        // ENet calls this on acknowledgment or teardown. Count retained messages
        // as well as queued ones to bound memory if connectivity deteriorates.
        auto* self = static_cast<Impl*>(packet->userData);
        std::lock_guard lock(self->mutex);
        --self->outstanding_messages;
        self->outstanding_bytes -= packet->dataLength;
    }

    void submit_pending() {
        std::deque<std::vector<std::uint8_t>> pending;
        {
            std::lock_guard lock(mutex);
            pending.swap(queue);
        }
        for (const auto& bytes : pending) {
            Packet packet(enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE),
                          enet_packet_destroy);
            if (!packet) throw std::runtime_error("Cannot allocate UDP video packet");
            packet->userData = this;
            packet->freeCallback = acknowledged;
            if (enet_peer_send(peer, 0, packet.get()) != 0)
                throw std::runtime_error("Could not send UDP video packet");
            packet.release(); // ENet owns the packet until all fragments are acknowledged.
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
                Packet unexpected(event.packet, enet_packet_destroy);
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
                if (event.type == ENET_EVENT_TYPE_RECEIVE)
                    throw std::runtime_error("Unexpected data from UDP receiver");
                if (Clock::now() >= close_deadline) break;
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            error = std::current_exception();
        }
        closed = true;
        enet_peer_disconnect_now(peer, 0);
        host.reset(); // release packet callbacks while their counters and mutex exist
    }
};

Sender::Sender(const std::string& host, std::uint16_t port) : impl_(std::make_unique<Impl>(host, port)) {}
Sender::~Sender() = default;
void Sender::send(const std::vector<std::uint8_t>& message) { impl_->enqueue(message); }
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

void Listener::disconnect() noexcept {
    if (impl_->peer) {
        enet_peer_disconnect_now(impl_->peer, 0);
        impl_->peer = nullptr;
    }
}

} // namespace udp
