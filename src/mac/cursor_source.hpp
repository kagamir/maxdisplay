#pragma once

#include "cursor_link.hpp"
#include "net.hpp"
#include "secure.hpp"

#include <cstdint>
#include <memory>
#include <optional>

// Times are monotonic milliseconds. An isolated ACK after an outage must not
// immediately switch the video off again on a flapping network.
class CursorRoute {
public:
    explicit CursorRoute(std::uint64_t start) : start_(start) {}

    void ack(std::uint64_t now) {
        if (!last_) first_ack_in_time_ = now - start_ <= 1000;
        if (!last_ || now - *last_ > 500) since_ = now;
        last_ = now;
    }

    bool ready(std::uint64_t now, bool active) const {
        if (!last_ || now - *last_ >= 2000) return false;
        if (active) return true;
        return now - *last_ <= 500 && (since_ - start_ <= 1000 || *last_ - since_ >= 1000);
    }

    bool initial_timeout(std::uint64_t now) const {
        return !first_ack_in_time_ && now - start_ >= 1000;
    }

private:
    std::uint64_t start_, since_ = 0;
    std::optional<std::uint64_t> last_;
    bool first_ack_in_time_ = false;
};

// Starts probing before the virtual display exists. The worker owns sampling,
// side-channel crypto and ARQ for the lifetime of this video session.
class CursorSource {
public:
    CursorSource(net::Socket socket, net::Endpoint peer, std::uint64_t token,
                 std::unique_ptr<secure::Datagrams> cipher);
    ~CursorSource();
    void set_display(std::uint32_t id, std::uint32_t width, std::uint32_t height);
    bool ready(bool active) const;
    bool initial_timeout() const;
    std::uint64_t movement() const;
    void show_overlay(bool show, std::optional<std::uint32_t> video_floor = {});
    cursor::SenderStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
