#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

// All times are microseconds on the sender's monotonic clock. Only a bounded
// history of transmitted frames is needed, even if the receiver stops replying.
class StreamControl {
public:
    StreamControl(std::uint32_t ceiling, std::uint32_t fps, std::uint32_t timeout_ms,
                  bool adaptive_bitrate, bool adaptive_fps, bool feedback)
        : ceiling_(ceiling), floor_(std::min(ceiling, std::max(250'000u, ceiling / 8))),
          bitrate_(ceiling), max_fps_(fps), min_fps_(std::min(10u, (fps + 3) / 4)), fps_(fps),
          budget_(std::uint64_t(timeout_ms) * 1000),
          flight_limit_(std::clamp((std::uint64_t(fps) * timeout_ms + 999) / 1000 + 2,
                                   std::uint64_t(4), std::uint64_t(128))),
          adaptive_bitrate_(adaptive_bitrate && feedback), adaptive_fps_(adaptive_fps && feedback),
          feedback_(feedback) {}

    std::uint32_t bitrate() const { return bitrate_; }
    std::uint32_t fps() const { return fps_; }
    bool needs_key() const { return needs_key_; }
    std::uint64_t deadline(std::uint64_t capture) const { return capture + budget_; }
    bool expired(std::uint64_t now, std::uint64_t capture) const { return now >= deadline(capture); }

    // Call periodically even on a stationary screen. A single missing final
    // frame cannot be discovered from the next sequence number on such screens.
    bool poll(std::uint64_t now) {
        if (flight_.empty()) return false;
        if (!expired(now, flight_.front().sent) && flight_.size() <= flight_limit_) return false;
        loss(now);
        return true;
    }

    void loss(std::uint64_t now) {
        congestion(now);
        flight_.clear();
        needs_key_ = true;
    }

    // A later capture replaces an earlier unencoded change. A timer may consume
    // the final change once its slot opens, even when the desktop stops moving.
    enum class Capture { skip, encode, force_key };
    Capture next_capture(std::uint64_t now, bool changed) {
        pending_change_ |= changed;
        if (needs_key_) {
            if (last_request_ && now - *last_request_ < 100'000) return Capture::skip;
            last_request_ = now;
            last_slot_ = now;
            pending_change_ = false;
            return Capture::force_key;
        }
        if (!pending_change_) return Capture::skip;
        const auto interval = 1'000'000u / fps_;
        if (fps_ < max_fps_ && last_slot_) {
            if (now - *last_slot_ < interval) return Capture::skip;
            // Preserve fractional cadence (e.g. 60 captures into 45 slots)
            // without issuing a burst to catch up after a pause.
            *last_slot_ = now - *last_slot_ >= 2 * interval ? now : *last_slot_ + interval;
        } else {
            // ScreenCaptureKit already enforces the configured upper limit.
            last_slot_ = now;
        }
        pending_change_ = false;
        return Capture::encode;
    }

    // A large independent frame is not representative of sustained encode cost.
    void observe_encode_time(std::uint64_t us, bool keyframe) {
        if (!adaptive_fps_ || keyframe) return;
        encode_us_ = encode_us_ ? (7 * encode_us_ + us) / 8 : us;
        if (encode_us_ <= 1'000'000u / fps_) {
            slow_encodes_ = 0;
        } else if (++slow_encodes_ >= 3) {
            fps_ = std::min(fps_, encoder_fps_limit());
            healthy_since_.reset();
            slow_encodes_ = 0;
        }
    }

    bool accept_encoded(std::uint32_t sequence, bool keyframe) {
        if (keyframe) {
            latest_key_ = sequence;
            needs_key_ = false;
        }
        return !needs_key_;
    }

    void sent(std::uint32_t sequence, std::uint64_t now, std::size_t bytes) {
        if (feedback_) flight_.push_back({sequence, now, bytes});
    }

    // True tells the queue to discard dependent frames. A request can arrive
    // separately after an ordinary ACK for the exact same sequence number.
    bool acknowledge(std::uint32_t sequence, bool request_key, std::uint64_t now) {
        if (!feedback_) return false;
        auto it = std::find_if(flight_.begin(), flight_.end(),
                               [sequence](const Sample& sample) { return sample.sequence == sequence; });
        if (it != flight_.end()) {
            const auto rtt = now - it->sent;
            std::uint64_t bytes = 0;
            do {
                const auto sample = flight_.front();
                flight_.pop_front();
                bytes += sample.bytes;
                if (sample.sequence == sequence) break;
            } while (!flight_.empty());
            record_delivery(bytes, now, request_key);
            const auto queued_delay = rtt - baseline_rtt(rtt, now);
            if (rtt >= budget_ || queued_delay > std::max<std::uint64_t>(12'000, budget_ / 3))
                congestion(now);
            else if (!request_key && queued_delay <= budget_ / 6)
                recover_rate(now);
            else
                healthy_since_.reset();
        }
        // Ignore delayed requests for a GOP already superseded by a fresh IDR.
        if (request_key && latest_key_ && (sequence - *latest_key_) < 0x80000000u && !needs_key_) {
            loss(now);
            return true;
        }
        return false;
    }

private:
    struct Sample { std::uint32_t sequence; std::uint64_t sent; std::size_t bytes; };
    std::uint32_t ceiling_, floor_, bitrate_, max_fps_, min_fps_, fps_;
    std::uint64_t budget_, flight_limit_;
    bool adaptive_bitrate_, adaptive_fps_, feedback_, needs_key_ = true;
    bool pending_change_ = false;
    std::optional<std::uint64_t> last_slot_;
    std::uint64_t encode_us_ = 0;
    unsigned slow_encodes_ = 0;
    std::deque<Sample> flight_;
    std::optional<std::uint32_t> latest_key_;
    std::optional<std::uint64_t> last_request_, last_cut_, healthy_since_;
    std::optional<std::uint64_t> rtt_window_start_, previous_min_rtt_;
    std::uint64_t current_min_rtt_ = 0;
    std::optional<std::uint64_t> delivery_start_;
    std::uint64_t delivered_ = 0;
    std::optional<std::uint32_t> delivery_bps_;

    std::uint32_t encoder_fps_limit() const {
        if (!encode_us_) return max_fps_;
        // Leave ten percent of each frame interval for capture and queueing.
        return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(900'000 / encode_us_, min_fps_, max_fps_));
    }

    // Keep a rolling minimum across two windows so a lasting change of route
    // below the frame budget eventually establishes a new latency baseline.
    std::uint64_t baseline_rtt(std::uint64_t rtt, std::uint64_t now) {
        constexpr std::uint64_t window = 5'000'000;
        if (!rtt_window_start_ || now - *rtt_window_start_ >= window) {
            if (rtt_window_start_ && now - *rtt_window_start_ < 2 * window)
                previous_min_rtt_ = current_min_rtt_;
            else
                previous_min_rtt_.reset();
            rtt_window_start_ = now;
            current_min_rtt_ = rtt;
        } else {
            current_min_rtt_ = std::min(current_min_rtt_, rtt);
        }
        return std::min(current_min_rtt_, previous_min_rtt_.value_or(current_min_rtt_));
    }

    void record_delivery(std::uint64_t bytes, std::uint64_t now, bool loss_reported) {
        if (!delivery_start_ || loss_reported || now - *delivery_start_ > 1'000'000) {
            healthy_since_.reset();
            delivery_start_ = now;
            delivered_ = 0;
            delivery_bps_.reset();
            return;
        }
        delivered_ += bytes;
        const auto interval = now - *delivery_start_;
        if (interval >= 250'000) {
            const auto bps = std::min<std::uint64_t>(ceiling_, delivered_ * 8'000'000 / interval);
            delivery_bps_ = static_cast<std::uint32_t>(bps);
            delivery_start_ = now;
            delivered_ = 0;
        }
    }

    void congestion(std::uint64_t now) {
        healthy_since_.reset();
        if (last_cut_ && now - *last_cut_ < 400'000) return;
        last_cut_ = now;
        if (adaptive_fps_) fps_ = std::max(min_fps_, fps_ * 3 / 4);
        if (!adaptive_bitrate_) return;
        // Only consult delivered throughput during congestion. Quiet desktop
        // imagery can encode far below the configured rate on a healthy link.
        auto target = std::uint64_t(bitrate_) * 3 / 4;
        if (delivery_bps_ && delivery_start_ && now - *delivery_start_ <= 1'000'000)
            target = std::min(target, std::max(std::uint64_t(bitrate_) / 2,
                                               std::uint64_t(*delivery_bps_) * 9 / 10));
        bitrate_ = std::max(floor_, static_cast<std::uint32_t>(target));
    }

    void recover_rate(std::uint64_t now) {
        if ((!adaptive_bitrate_ || bitrate_ == ceiling_) && (!adaptive_fps_ || fps_ == max_fps_)) return;
        if (!healthy_since_) healthy_since_ = now;
        if (now - *healthy_since_ < 1'500'000) return;
        if (adaptive_bitrate_) {
            const auto increase = std::max(50'000u, bitrate_ / 12);
            bitrate_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(ceiling_, std::uint64_t(bitrate_) + increase));
        }
        if (adaptive_fps_)
            fps_ = std::max(fps_, std::min(encoder_fps_limit(), fps_ + std::max(1u, fps_ / 10)));
        healthy_since_ = now;
    }
};
