#pragma once

#include "frame_recovery.hpp"
#include "rate_control.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

// Deterministic 1 ms clock. Packets represent complete access units. TCP waits
// for buffer space; UDP can drop a delta at the tail or lose one in transit.
// Both directions have propagation delay and ACKs wait for the decoder.
namespace stream_sim {

using Time = std::uint64_t;
constexpr Time second = 1'000'000;

struct Scenario {
    std::uint32_t bitrate = 12'000'000, fps = 60, timeout_ms = 200;
    std::size_t key_bytes = 75'000, queue_bytes = 512'000;
    unsigned periodic_frames = 0;
    Time propagation_us = 3'000, encode_us = 2'000, decode_us = 2'000;
    Time bitrate_lag_us = 2 * second;
    double target_utilization = 1.;
    enum class Allocation { time, frame } allocation = Allocation::time;
    Transport transport = Transport::tcp;
    bool buffer_one_frame = false;
    std::function<std::uint32_t(Time)> capacity = [](Time) { return 40'000'000u; };
    std::function<bool(Time)> active = [](Time) { return true; };
    std::function<bool(std::uint32_t)> lose_delta = [](std::uint32_t) { return false; };
    // Number of repeat encodes of the same captured image, zero on new input.
    std::function<double(unsigned)> psnr = [](unsigned) { return 45.; };
};

class StreamSim {
public:
    explicit StreamSim(const Scenario& scenario)
        : control(scenario.bitrate, scenario.fps, scenario.timeout_ms, true, true),
          options_(scenario), receiver_(scenario.timeout_ms), encoder_bps_(scenario.bitrate) {}

    void run_until(Time end) {
        for (; now_ < end; now_ += 1'000) {
            feedback();
            if (control.poll(now_)) pending_.clear();
            encode();
            write();
            transmit();
            receive();
        }
    }

    StreamControl control;
    unsigned keys_sent = 0, presented = 0, udp_drops = 0, repeats_encoded = 0, idle_drains = 0;
    Time last_presented_at = 0;
    double presented_psnr = 0;

private:
    struct Frame {
        std::uint32_t seq;
        Time capture;
        std::size_t bytes;
        bool key;
        double psnr;
    };
    struct Picture { double psnr; Time deadline; };
    struct Transfer { Frame frame; std::size_t remaining; };
    struct Arrival { Frame frame; Time at; };
    struct Ack { std::uint32_t seq; bool request; Time at; };

    Scenario options_;
    FrameRecovery receiver_;
    Time now_ = 0, next_capture_ = 0, last_encode_ = 0, encode_done_ = 0, receiver_free_ = 0;
    Time drain_at_ = 0, next_refresh_ = 0;
    std::uint64_t capture_tick_ = 0;
    unsigned picture_ = 0, encoded_picture_ = 0, repeats_ = 0, since_key_ = 0;
    std::uint32_t sequence_ = 0;
    bool input_changed_ = false, timer_follows_capture_ = false;
    std::optional<double> decoding_psnr_;
    std::optional<Picture> buffered_;
    double encoder_bps_;
    std::optional<Frame> encoding_, writing_;
    std::deque<Frame> pending_;
    std::deque<Transfer> wire_;
    std::size_t queued_bytes_ = 0;
    std::deque<Arrival> arrivals_;
    std::deque<Ack> acks_;

    void feedback() {
        // The same writer services feedback between writes.
        while (!writing_ && !acks_.empty() && acks_.front().at <= now_) {
            const auto ack = acks_.front();
            acks_.pop_front();
            if (control.acknowledge(ack.seq, ack.request, now_)) pending_.clear();
        }
    }

    void encode() {
        // A time constant of several seconds models codecs that take many
        // frames to settle after their target changes.
        const auto step = 1'000. / std::max<Time>(1'000, options_.bitrate_lag_us);
        encoder_bps_ += (control.bitrate() - encoder_bps_) * step;
        if (encoding_ && now_ >= encode_done_) {
            const auto frame = *encoding_;
            encoding_.reset();
            control.observe_encode_time(now_ - frame.capture, frame.key);
            if (control.expired(now_, frame.capture, frame.key)) {
                pending_.clear();
                control.loss(now_, StreamControl::Cause::slow_encode);
            } else {
                if (pending_.size() >= 4) {
                    pending_.clear();
                    control.loss(now_, StreamControl::Cause::backlog);
                }
                if (control.accept_encoded(frame.seq, frame.key)) {
                    control.observe_quality(now_, frame.psnr, std::nullopt);
                    pending_.push_back(frame);
                }
            }
        }

        if (now_ >= next_capture_) {
            next_capture_ = ++capture_tick_ * second / options_.fps;
            if (options_.active(now_)) {
                ++picture_;
                input_changed_ = true;
            }
        }
        if (encoding_ || !picture_ || (!input_changed_ && now_ < next_refresh_)) return;
        const auto action = control.next_capture(now_, input_changed_);
        const auto tick = StreamControl::refresh_tick_us(options_.fps);
        if (input_changed_) {
            timer_follows_capture_ = true;
            next_refresh_ = now_ + tick;
        } else if (timer_follows_capture_ && !control.refining(now_)) {
            timer_follows_capture_ = false;
            next_refresh_ = now_ + 25'000;
        } else {
            const auto interval = timer_follows_capture_ ? tick : 25'000;
            next_refresh_ += ((now_ - next_refresh_) / interval + 1) * interval;
        }
        input_changed_ = false;
        if (action == StreamControl::Capture::skip) return;
        const bool key = action == StreamControl::Capture::force_key ||
                         (options_.periodic_frames && since_key_ + 1 >= options_.periodic_frames);
        since_key_ = key ? 0 : since_key_ + 1;
        const bool repeat = picture_ == encoded_picture_;
        repeats_ = repeat ? repeats_ + 1 : 0;
        if (repeat) ++repeats_encoded;
        encoded_picture_ = picture_;

        // Cap the time allocation after a long static pause. A codec that
        // allocates by frame count continues to assume its configured input FPS.
        const auto nominal = second / control.fps();
        const auto duration = options_.allocation == Scenario::Allocation::frame ? second / options_.fps :
                              std::min(now_ - last_encode_, 2 * nominal);
        auto bytes = static_cast<std::size_t>(encoder_bps_ * options_.target_utilization * duration / (8 * second));
        // Static text may initially produce tiny skipped pictures before a
        // larger refinement; size alone cannot indicate convergence.
        if (repeat && repeats_ % 12) bytes = 300;
        encoding_ = Frame{++sequence_, now_, key ? options_.key_bytes : std::max<std::size_t>(1, bytes),
                          key, options_.psnr(repeats_)};
        last_encode_ = now_;
        encode_done_ = now_ + options_.encode_us;
    }

    void write() {
        if (!pending_.empty() && control.expired(now_, pending_.front().capture, pending_.front().key)) {
            pending_.clear();
            control.loss(now_, StreamControl::Cause::queue_expired);
        }
        if (!writing_) {
            if (pending_.empty()) return;
            writing_ = pending_.front();
            pending_.pop_front();
            control.start_send(writing_->seq, now_, writing_->bytes, writing_->key);
        }
        const auto& frame = *writing_;
        if (control.expired(now_, frame.capture, frame.key)) {
            writing_.reset();
            pending_.clear();
            control.loss(now_, StreamControl::Cause::send_blocked);
            return;
        }
        // Allow one oversized TCP write or reliable UDP key into an empty
        // queue. All its bytes still have to traverse the bottleneck.
        const bool full = queued_bytes_ && queued_bytes_ + frame.bytes > options_.queue_bytes;
        if (full && (options_.transport == Transport::tcp || frame.key)) return;
        control.confirm_send(frame.bytes, frame.key);
        if (frame.key) ++keys_sent;
        if (full) ++udp_drops;
        else {
            wire_.push_back({frame, frame.bytes});
            queued_bytes_ += frame.bytes;
        }
        writing_.reset();
    }

    void transmit() {
        std::size_t available = options_.capacity(now_) / 8'000; // bytes per millisecond
        while (available && !wire_.empty()) {
            auto& head = wire_.front();
            const auto bytes = std::min(available, head.remaining);
            available -= bytes;
            queued_bytes_ -= bytes;
            head.remaining -= bytes;
            if (head.remaining) break;
            if (options_.transport == Transport::udp && !head.frame.key && options_.lose_delta(head.frame.seq))
                ++udp_drops;
            else
                arrivals_.push_back({head.frame, now_ + options_.propagation_us});
            wire_.pop_front();
        }
    }

    void queue_picture(const Picture& picture) {
        if (receiver_free_ > picture.deadline) receiver_.recover();
        else decoding_psnr_ = picture.psnr;
    }

    void acknowledge() {
        acks_.push_back({receiver_.sequence(), receiver_.waiting_for_keyframe(),
                         receiver_free_ + options_.propagation_us});
    }

    void receive() {
        if (now_ < receiver_free_) return;
        if (decoding_psnr_) {
            ++presented;
            last_presented_at = now_;
            presented_psnr = *decoding_psnr_;
            decoding_psnr_.reset();
        }

        if (arrivals_.empty() || arrivals_.front().at > now_) {
            if (buffered_ && !receiver_.waiting_for_keyframe() && now_ >= drain_at_) {
                ++idle_drains;
                receiver_free_ = now_ + options_.decode_us;
                queue_picture(*buffered_);
                buffered_.reset();
                // As in receive_live(), a successful drain displays the last
                // picture; the next delta then requests a fresh key.
                if (receiver_.waiting_for_keyframe()) acknowledge();
                receiver_.recover();
            }
            return;
        }
        const auto frame = arrivals_.front().frame;
        arrivals_.pop_front();
        if (buffered_ && now_ > buffered_->deadline) receiver_.recover();
        const auto action = receiver_.inspect(frame.seq, frame.capture, frame.key, now_);
        receiver_free_ = now_;
        if (action != FrameRecovery::Action::drop) {
            if (action == FrameRecovery::Action::restart) buffered_.reset();
            receiver_free_ += options_.decode_us;
            const auto remaining = receiver_.remaining_us();
            const Picture current{frame.psnr, now_ + remaining};
            if (options_.buffer_one_frame) {
                if (buffered_) queue_picture(*buffered_);
                buffered_ = current;
                drain_at_ = now_ + receiver_.idle_drain_us(options_.fps, remaining);
            } else queue_picture(current);
        }
        acknowledge();
    }
};

} // namespace stream_sim
