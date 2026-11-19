#pragma once

#include "secure.hpp"
#include "wire.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace udp {

// ENet preserves message boundaries, including messages larger than the MTU.
class Sender {
public:
    Sender(const std::string& host, std::uint16_t port, const std::string& password = {});
    ~Sender();
    net::Endpoint peer_endpoint() const;
    std::unique_ptr<secure::Datagrams> datagrams();
    wire::CursorOffer request_cursor();
    wire::DisplaySize request_display_size(unsigned timeout_ms = wire::display_query_timeout_ms);
    void send_header(const wire::Header& header);
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

// One video consumer serializes accept, setup, receive and disconnect.
// Cursor reads and replies run independently on this same UDP socket.
class Listener {
public:
    Listener(const std::string& bind_address, std::uint16_t port, const std::string& password = {});
    ~Listener();
    std::uint16_t local_port() const;
    bool accept(unsigned timeout_ms);
    // Filter incoming cursor payloads by video peer IP and clear session token.
    // One cursor consumer validates each payload before replying to its source.
    void open_cursor(net::Endpoint video_peer, std::uint64_t token);
    void close_cursor() noexcept;
    bool receive_cursor(secure::Bytes& inner, net::Endpoint& from, unsigned timeout_ms);
    bool send_cursor(const secure::Bytes& inner, net::Endpoint to);
    net::Endpoint peer_endpoint() const;
    std::unique_ptr<secure::Datagrams> datagrams();
    wire::Header receive_header(const std::function<wire::DisplaySize()>& display_size,
                                const std::function<bool()>& stopping = {},
                                const wire::CursorResponder& cursor = {});
    ReadResult receive(std::vector<std::uint8_t>& message, unsigned timeout_ms);
    void send_feedback(const std::vector<std::uint8_t>& message);
    void disconnect() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace udp
