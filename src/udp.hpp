#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace udp {

// ENet preserves message boundaries, including messages larger than the MTU.
class Sender {
public:
    Sender(const std::string& host, std::uint16_t port);
    ~Sender();
    void send(const std::vector<std::uint8_t>& message);
    // Sync pictures can retransmit missing fragments. Delta pictures are sequenced/unreliable.
    // False reports a full local queue or a preceding sync picture still in flight.
    bool send_frame(const std::vector<std::uint8_t>& message, bool keyframe);
    std::optional<std::vector<std::uint8_t>> take_feedback();
    bool peer_closed() const;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class ReadResult { message, timeout, disconnected };

// Service accept/receive on the same thread. Bounded polls let the caller
// stop the receiver without closing an ENet host from another thread.
class Listener {
public:
    Listener(const std::string& bind_address, std::uint16_t port);
    ~Listener();
    std::uint16_t local_port() const;
    bool accept(unsigned timeout_ms);
    ReadResult receive(std::vector<std::uint8_t>& message, unsigned timeout_ms);
    void send_feedback(const std::vector<std::uint8_t>& message);
    void disconnect() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace udp
