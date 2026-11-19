#include "secure_tcp.hpp"

#include <algorithm>
#include <stdexcept>

namespace secure {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto max_sealed_frame = wire::max_frame_bytes + record_overhead;

unsigned poll_until(Clock::time_point deadline) {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) throw std::runtime_error("Encrypted TCP handshake timed out");
    return static_cast<unsigned>(std::min<std::int64_t>(remaining, 250));
}

Bytes read_hello(net::Socket& socket, Role role, Clock::time_point deadline) {
    Bytes hello(hello_size);
    auto poll = [&] { return poll_until(deadline); };
    // Reject a short cleartext display query/header before waiting for the nonce.
    constexpr std::size_t prefix = 8;
    if (!socket.read_exact(hello.data(), prefix, poll))
        throw std::runtime_error("Peer disconnected during encrypted TCP handshake");
    validate_hello(hello, role);
    if (!socket.read_exact(hello.data() + prefix, hello.size() - prefix, poll))
        throw std::runtime_error("Incomplete encrypted TCP handshake");
    return hello;
}

void send_confirm(net::Socket& socket, Session& session) {
    const auto proof = session.seal(Kind::confirm, {});
    socket.write_all(proof.data(), proof.size());
}

void verify_confirm(net::Socket& socket, Session& session, Clock::time_point deadline) {
    Bytes proof(record_overhead);
    if (!socket.read_exact(proof.data(), proof.size(), [&] { return poll_until(deadline); }))
        throw std::runtime_error("Peer disconnected before password confirmation");
    session.open(Kind::confirm, proof);
}

Bytes read_control(net::Socket& socket, std::size_t max_payload, Clock::time_point deadline) {
    Bytes bytes;
    if (!wire::receive_frame(socket, bytes, [&] { return poll_until(deadline); }, max_payload + record_overhead))
        throw std::runtime_error("Peer disconnected during encrypted display negotiation");
    return bytes;
}
} // namespace

TcpChannel::TcpChannel(net::Socket& socket, const std::string& password, Role role)
    : socket_(socket), feedback_(wire::feedback_size + (password.empty() ? 0 : record_overhead)) {
    if (password.empty()) return;
    const auto deadline = Clock::now() + wire::header_timeout;
    if (role == Role::sender) {
        const auto local = make_hello(role);
        socket_.write_all(local.data(), local.size());
        const auto peer = read_hello(socket_, Role::receiver, deadline);
        session_.emplace(password, local, peer, role);
        verify_confirm(socket_, *session_, deadline);
        send_confirm(socket_, *session_);
    } else {
        const auto peer = read_hello(socket_, Role::sender, deadline);
        const auto local = make_hello(role);
        socket_.write_all(local.data(), local.size());
        session_.emplace(password, peer, local, role);
        send_confirm(socket_, *session_);
        verify_confirm(socket_, *session_, deadline);
    }
}

Bytes TcpChannel::exchange_query(Kind query_kind, const Bytes& query, Kind reply_kind, std::size_t reply_size) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(wire::display_query_timeout_ms);
    if (!wire::send_frame_before(socket_, session_->seal(query_kind, query), deadline))
        throw std::runtime_error("Sending encrypted query timed out");
    return session_->open(reply_kind, read_control(socket_, reply_size, deadline));
}

wire::CursorOffer TcpChannel::request_cursor(std::uint16_t port) {
    if (!session_) return wire::request_cursor(socket_, port);
    return wire::decode_cursor_reply(exchange_query(Kind::cursor_query, wire::encode_cursor_query(port),
                                                  Kind::cursor_reply, wire::cursor_reply_size));
}

wire::DisplaySize TcpChannel::request_display_size() {
    if (!session_) return wire::request_display_size(socket_);
    return wire::decode_display_reply(exchange_query(Kind::display_query, wire::display_query,
                                                     Kind::display_reply, wire::display_reply_size));
}

void TcpChannel::send_header(wire::Header header) {
    if (session_) wire::send_frame(socket_, session_->seal(Kind::header, wire::encode_header(header)));
    else wire::send_header(socket_, header);
}

wire::Header TcpChannel::receive_header(const std::function<wire::DisplaySize()>& display_size,
                                        const wire::CursorResponder& cursor) {
    if (!session_) return wire::receive_header(socket_, display_size, cursor);
    const auto deadline = Clock::now() + wire::header_timeout;
    auto record = read_control(socket_, wire::header_size, deadline);
    if (record.front() == static_cast<std::uint8_t>(Kind::cursor_query)) {
        const auto port = wire::decode_cursor_query(session_->open(Kind::cursor_query, record));
        const auto reply = wire::encode_cursor_reply(cursor ? cursor(port) : wire::CursorOffer{});
        wire::send_frame(socket_, session_->seal(Kind::cursor_reply, reply));
        record = read_control(socket_, wire::header_size, deadline);
    }
    if (record.front() == static_cast<std::uint8_t>(Kind::display_query)) {
        const auto query = session_->open(Kind::display_query, record);
        if (query != wire::display_query || !display_size)
            throw std::runtime_error("Invalid encrypted display query");
        const auto reply = wire::encode_display_reply(display_size());
        wire::send_frame(socket_, session_->seal(Kind::display_reply, reply));
        record = read_control(socket_, wire::header_size, deadline);
    }
    return wire::decode_header(session_->open(Kind::header, record));
}

bool TcpChannel::send_frame_before(const Bytes& frame, Clock::time_point deadline) {
    if (!session_) return wire::send_frame_before(socket_, frame, deadline);
    if (Clock::now() >= deadline) return false;
    return wire::send_frame_before(socket_, session_->seal(Kind::frame, frame), deadline, max_sealed_frame);
}

bool TcpChannel::receive_frame(Bytes& frame, const std::function<unsigned()>& poll_interval) {
    if (!session_) return wire::receive_frame(socket_, frame, poll_interval);
    Bytes record;
    if (!wire::receive_frame(socket_, record, poll_interval, max_sealed_frame)) return false;
    frame = session_->open(Kind::frame, record);
    return true;
}

void TcpChannel::send_feedback(const Bytes& feedback) {
    if (feedback.size() != wire::feedback_size) throw std::runtime_error("Incorrect TCP feedback size");
    if (session_) {
        const auto record = session_->seal(Kind::feedback, feedback);
        socket_.write_all(record.data(), record.size());
    } else {
        socket_.write_all(feedback.data(), feedback.size());
    }
}

std::optional<Bytes> TcpChannel::take_feedback() {
    feedback_used_ += socket_.try_read(feedback_.data() + feedback_used_, feedback_.size() - feedback_used_);
    if (feedback_used_ != feedback_.size()) return std::nullopt;
    feedback_used_ = 0;
    return session_ ? session_->open(Kind::feedback, feedback_) : feedback_;
}

} // namespace secure
