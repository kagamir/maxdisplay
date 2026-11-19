#pragma once

#include "secure.hpp"
#include "wire.hpp"

#include <optional>

namespace secure {

// A password authenticates the peer before the first display or video record.
class TcpChannel {
public:
    TcpChannel(net::Socket& socket, const std::string& password, Role role);
    net::Endpoint peer_endpoint() const { return socket_.peer_endpoint(); }
    std::unique_ptr<Datagrams> datagrams() { return session_ ? session_->datagrams() : nullptr; }
    wire::CursorOffer request_cursor(std::uint16_t port);
    wire::DisplaySize request_display_size();
    void send_header(wire::Header header);
    wire::Header receive_header(const std::function<wire::DisplaySize()>& display_size = {},
                                const wire::CursorResponder& cursor = {});
    bool send_frame_before(const Bytes& frame, std::chrono::steady_clock::time_point deadline);
    bool receive_frame(Bytes& frame, const std::function<unsigned()>& poll_interval = {});
    void send_feedback(const Bytes& feedback);
    std::optional<Bytes> take_feedback();

private:
    Bytes exchange_query(Kind query_kind, const Bytes& query, Kind reply_kind, std::size_t reply_size);
    net::Socket& socket_;
    std::optional<Session> session_;
    Bytes feedback_;
    std::size_t feedback_used_ = 0;
};

} // namespace secure
