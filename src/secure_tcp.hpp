#pragma once

#include "secure.hpp"
#include "wire.hpp"

#include <optional>

namespace secure {

// Uses the original wire protocol when the password is empty. Otherwise the
// constructor authenticates the peer before the first display or video record.
class TcpChannel {
public:
    TcpChannel(net::Socket& socket, const std::string& password, Role role);
    wire::DisplaySize request_display_size();
    void send_header(wire::Header header);
    wire::Header receive_header(const std::function<wire::DisplaySize()>& display_size = {});
    void send_frame(const Bytes& frame);
    bool send_frame_before(const Bytes& frame, std::chrono::steady_clock::time_point deadline);
    bool receive_frame(Bytes& frame, const std::function<unsigned()>& poll_interval = {});
    void send_feedback(const Bytes& feedback);
    std::optional<Bytes> take_feedback();

private:
    net::Socket& socket_;
    std::optional<Session> session_;
    Bytes feedback_;
    std::size_t feedback_used_ = 0;
};

} // namespace secure
