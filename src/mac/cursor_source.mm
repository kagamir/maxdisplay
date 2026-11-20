#include "cursor_source.hpp"
#include "cursor_image.hpp"
#include "cursor_packet.hpp"

#import <Foundation/Foundation.h>
#include <pthread/qos.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
std::uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
}

struct CursorSource::Impl {
    net::Socket socket;
    net::Endpoint peer;
    std::unique_ptr<secure::Datagrams> cipher;
    cursor::Sender sender;
    std::atomic<bool> stopping{false};
    mutable std::mutex mutex;
    // Protected by mutex. Only the worker accesses the bitmap and ARQ state.
    std::uint32_t display = 0, width = 0, height = 0;
    CursorRoute route{now_ms()};
    cursor::SenderStats stats{};
    std::uint64_t movement = 0;
    bool available = false, image_ready = false, show = false;
    std::optional<std::uint32_t> video_floor;
    std::thread worker;

    Impl(net::Socket s, net::Endpoint p, std::uint64_t token, std::unique_ptr<secure::Datagrams> keys)
        : socket(std::move(s)), peer(p), cipher(std::move(keys)), sender(token), worker([this] {
            try { run(); }
            catch (const std::exception& e) {
                std::lock_guard lock(mutex);
                available = false;
                std::cerr << "Cursor UDP worker stopped: " << e.what() << '\n';
            }
        }) {}

    ~Impl() {
        stopping = true;
        worker.join();
    }

    void run() {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        std::optional<CursorImageSource> source;
        cursor::State previous{};
        auto next = Clock::now();
        cursor::Bytes packet;
        net::Endpoint from;
        while (!stopping) {
            @autoreleasepool {
                if (!source) {
                    std::lock_guard lock(mutex);
                    if (display) source.emplace(display, width, height);
                }
                const auto now = now_ms();
                const auto previous_ack = sender.last_ack();
                // Bound work per tick so a stream of unrelated UDP traffic
                // cannot indefinitely postpone sampling or shutdown.
                for (unsigned n = 0; n < 128 && socket.receive_from(from, packet); ++n) {
                    if (from != peer || !cursor::packet::unwrap(packet)) continue;
                    if (cipher) {
                        auto plain = cipher->open(packet);
                        if (plain) sender.acknowledge(now, *plain);
                    } else {
                        sender.acknowledge(now, packet);
                    }
                }
                const auto sample = source ? source->sample(now) : cursor::View{};
                const auto& state = sample.state;
                cursor::State outgoing{};
                {
                    std::lock_guard lock(mutex);
                    if (show) {
                        outgoing = state;
                        outgoing.video_floor = video_floor;
                    }
                }
                sender.update(now, outgoing, sample.image, [&](const cursor::Bytes& bytes) {
                    socket.send_to(peer, cursor::packet::wrap(cipher ? cipher->seal(bytes) : bytes));
                });
                std::lock_guard lock(mutex);
                if (sender.last_ack() != previous_ack) route.ack(now);
                available = source && source->available();
                // A hidden cursor needs no bitmap for the overlay handoff.
                image_ready = !state.visible || sender.shape_ready();
                if (state.visible != previous.visible || state.x != previous.x || state.y != previous.y)
                    ++movement;
                previous = state;
                stats = sender.stats();
            }
            next += 4ms;
            if (next <= Clock::now()) next = Clock::now() + 4ms;
            std::this_thread::sleep_until(next);
        }
    }
};

CursorSource::CursorSource(net::Socket socket, net::Endpoint peer, std::uint64_t token,
                           std::unique_ptr<secure::Datagrams> cipher)
    : impl_(std::make_unique<Impl>(std::move(socket), peer, token, std::move(cipher))) {}
CursorSource::~CursorSource() = default;

void CursorSource::set_display(std::uint32_t id, std::uint32_t width, std::uint32_t height) {
    std::lock_guard lock(impl_->mutex);
    impl_->display = id;
    impl_->width = width;
    impl_->height = height;
}

bool CursorSource::ready(bool active) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->available && (active || impl_->image_ready) && impl_->route.ready(now_ms(), active);
}

bool CursorSource::initial_timeout() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->route.initial_timeout(now_ms());
}

std::uint64_t CursorSource::movement() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->movement;
}

void CursorSource::show_overlay(bool show, std::optional<std::uint32_t> video_floor) {
    std::lock_guard lock(impl_->mutex);
    impl_->show = show;
    impl_->video_floor = video_floor;
}

cursor::SenderStats CursorSource::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}
