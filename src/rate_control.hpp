#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

// Times are microseconds on the sender's monotonic clock. Callers serialize
// capture, encode and feedback events before entering the controller.
class StreamControl {
public:
    enum class Cause { stale_capture, slow_encode, backlog, queue_expired, send_blocked,
                       unacknowledged, receiver_request, ack_delay, count };
    using Counts = std::array<std::uint64_t, static_cast<std::size_t>(Cause::count)>;
    struct Stats {
        Counts cuts{}, key_requests{};
        std::uint64_t frames = 0, bytes = 0, keyframes = 0, key_bytes = 0, key_rtt_us = 0;
        std::uint64_t rtt_us = 0, queue_us = 0, encode_us = 0, delivery_bps = 0;
    };

    StreamControl(std::uint32_t ceiling, std::uint32_t fps, std::uint32_t timeout_ms,
                  bool adaptive_bitrate, bool adaptive_fps, bool feedback)
        : ceiling_(ceiling), floor_(std::min(ceiling, std::max(250'000u, ceiling / 8))),
          bitrate_(ceiling), verified_(floor_), max_fps_(fps),
          min_fps_(std::min(10u, (fps + 3) / 4)), fps_(fps),
          budget_(std::uint64_t(timeout_ms) * 1000),
          key_budget_(std::clamp(4 * budget_, min_key_budget, max_key_budget)),
          adaptive_bitrate_(adaptive_bitrate && feedback), adaptive_fps_(adaptive_fps && feedback),
          feedback_(feedback) {}

    std::uint32_t bitrate() const { return bitrate_; }
    std::uint32_t fps() const { return fps_; }
    bool needs_key() const { return needs_key_; }
    Stats stats() const { return stats_; }
    bool refining(std::uint64_t now) const {
        return topoff_since_ && now - *topoff_since_ < topoff_window;
    }

    // Anchor the first refresh tick to each real capture. Subdividing 1.4
    // capture periods puts a tick after the 1.25-period jitter allowance and
    // before a buffered decoder drains at two periods (also at 48 Hz).
    static std::uint64_t refresh_tick_us(std::uint32_t max_fps) {
        const auto span = refresh_span / max_fps;
        const auto ticks = (span + refresh_max_tick - 1) / refresh_max_tick;
        return span / ticks;
    }

    std::uint64_t deadline(std::uint64_t capture, bool keyframe = false) const {
        return capture + (keyframe ? key_budget_ : budget_);
    }
    bool expired(std::uint64_t now, std::uint64_t capture, bool keyframe = false) const {
        return now >= deadline(capture, keyframe);
    }

    // A missing final frame has no later sequence gap to reveal it. Allow the
    // startup IDR its serialization time, including the deltas waiting behind it.
    bool poll(std::uint64_t now) {
        if (flight_.empty()) return false;
        const auto& head = flight_.front();
        // Deltas already on the wire when a large key is acknowledged have
        // also waited for its serialization. Give that cohort one ordinary
        // drain budget from the cumulative key ACK, even if ACKs coalesced.
        const bool draining_key = !head.keyframe && key_ack_ && head.sent <= *key_ack_;
        const auto start = draining_key ? *key_ack_ : head.sent;
        const auto end = deadline(start, head.keyframe);
        const auto frame_limit = (max_fps_ * (end - head.sent) + 999'999) / 1'000'000 + 2;
        if (now < end &&
            flight_.size() <= std::clamp<std::uint64_t>(frame_limit, 4, head.keyframe || draining_key ? 512 : 128))
            return false;
        loss(now, Cause::unacknowledged);
        return true;
    }

    void loss(std::uint64_t now, Cause cause) {
        topoff_since_.reset();
        const auto index = static_cast<std::size_t>(cause);
        if (cause == Cause::stale_capture || cause == Cause::slow_encode) {
            reset_health();
            if (last_local_drop_ && now - *last_local_drop_ <= local_window) {
                if (step_fps_down()) ++stats_.cuts[index];
                last_local_drop_.reset();
            } else {
                last_local_drop_ = now;
            }
        } else if (cause == Cause::receiver_request &&
                   (!last_delay_ || now - *last_delay_ > cut_interval)) {
            // A late request after an idle drain looks just like UDP loss in v3.
            // Pause the ACK clock without erasing credit earned before the idle.
            last_healthy_ack_.reset();
            while (!requests_.empty() && now - requests_.front() > request_window)
                requests_.pop_front();
            requests_.push_back(now);
            if (requests_.size() >= 3) {
                if (cut_bitrate(7, 8)) {
                    reset_health();
                    ++stats_.cuts[index];
                }
                requests_.clear();
            }
        } else {
            network_congestion(now, cause);
        }
        if (!needs_key_) ++stats_.key_requests[index];
        flight_.clear();
        needs_key_ = true;
    }

    enum class Capture { skip, encode, force_key };
    Capture next_capture(std::uint64_t now, bool changed) {
        if (changed) {
            pending_change_ = true;
            topoff_since_ = now;
            quality_start_.reset();
        }
        if (needs_key_) {
            if (last_request_ && now - *last_request_ < key_retry_interval) return Capture::skip;
            last_request_ = now;
        } else {
            const auto capture_period = 1'000'000u / max_fps_;
            const auto interval = 1'000'000u / fps_;
            if (!pending_change_) {
                // Allow capture jitter, then let the phased refresh timer feed
                // the receiver before a buffered decoder's two-period idle drain.
                if (!refining(now) || now - *topoff_since_ < capture_period + capture_period / 4)
                    return Capture::skip;
            }
            const auto slack = changed ? capture_period / 2 : 0;
            if (last_slot_ && now - *last_slot_ + slack < interval) return Capture::skip;
        }
        last_slot_ = now;
        submitted_change_ = pending_change_;
        pending_change_ = false;
        return needs_key_ ? Capture::force_key : Capture::encode;
    }

    // Encoders may omit an output without corrupting the preceding GOP.
    void encode_skipped() {
        pending_change_ |= submitted_change_;
        submitted_change_ = false;
        topoff_since_.reset();
    }

    void observe_quality(std::uint64_t now, std::optional<double> psnr,
                         std::optional<int> quantizer) {
        if (!topoff_since_) return;
        if ((psnr && *psnr >= quality_target) || (quantizer && *quantizer <= quantizer_target)) {
            topoff_since_.reset();
        } else if (psnr) {
            if (!quality_start_) {
                quality_start_ = now;
                initial_quality_ = *psnr;
            } else if (now - *quality_start_ >= quality_window) {
                if (*psnr - initial_quality_ < quality_gain) topoff_since_.reset();
                quality_start_ = now;
                initial_quality_ = *psnr;
            }
        }
    }

    void observe_encode_time(std::uint64_t us, bool keyframe) {
        stats_.encode_us = us;
        if (!adaptive_fps_ || keyframe) return;
        encode_us_ = encode_us_ ? (7 * encode_us_ + us) / 8 : us;
        if (encode_us_ <= 1'000'000u / fps_) {
            slow_encodes_ = 0;
        } else if (++slow_encodes_ >= 3) {
            const auto limit = encoder_fps_limit();
            if (fps_ > limit) {
                fps_ = limit;
                ++stats_.cuts[static_cast<std::size_t>(Cause::slow_encode)];
                reset_health();
                topoff_since_.reset();
            }
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

    // Register before releasing the writer lock so a concurrent loss can
    // invalidate this sample even if the subsequent socket write blocks.
    void start_send(std::uint32_t sequence, std::uint64_t now, std::size_t bytes, bool keyframe) {
        if (keyframe) last_key_sent_ = now;
        if (feedback_) flight_.push_back({sequence, now, bytes, keyframe,
                                         last_key_sent_ && now - *last_key_sent_ < budget_});
    }

    void confirm_send(std::size_t bytes, bool keyframe) {
        ++stats_.frames;
        stats_.bytes += bytes;
        if (keyframe) {
            ++stats_.keyframes;
            stats_.key_bytes = bytes;
        }
    }

    void sent(std::uint32_t sequence, std::uint64_t now, std::size_t bytes, bool keyframe) {
        start_send(sequence, now, bytes, keyframe);
        confirm_send(bytes, keyframe);
    }

    // True tells the queue to discard dependent frames. A separate key request
    // may follow an ordinary ACK for the same sequence after an idle drain.
    bool acknowledge(std::uint32_t sequence, bool request_key, std::uint64_t now) {
        if (!feedback_) return false;
        const bool valid_request = request_key && latest_key_ &&
            (sequence - *latest_key_) < 0x80000000u && !needs_key_;
        auto it = std::find_if(flight_.begin(), flight_.end(),
                               [sequence](const Sample& sample) { return sample.sequence == sequence; });
        if (it != flight_.end()) {
            const auto sample = *it;
            const auto rtt = now - sample.sent;
            std::uint64_t bytes = 0;
            do {
                bytes += flight_.front().bytes;
                if (flight_.front().keyframe) {
                    key_ack_ = now;
                    stats_.key_rtt_us = now - flight_.front().sent;
                }
                const auto popped = flight_.front().sequence;
                flight_.pop_front();
                if (popped == sequence) break;
            } while (!flight_.empty());
            record_delivery(bytes, now, request_key);
            stats_.rtt_us = rtt;
            if (sample.keyframe) {
                stats_.queue_us = 0;
            } else {
                stats_.queue_us = rtt - baseline_rtt(rtt, now);
            }
            const bool delayed = sample.keyframe ? rtt >= key_budget_ :
                rtt >= budget_ || (!sample.behind_key &&
                    stats_.queue_us > std::max<std::uint64_t>(12'000, budget_ / 3));
            if (delayed) {
                last_delay_ = now;
                if (!valid_request) network_congestion(now, Cause::ack_delay);
            } else if (!request_key && !sample.keyframe && stats_.queue_us <= budget_ / 6)
                recover_rate(now);
            else
                last_healthy_ack_.reset();
        }
        if (valid_request) loss(now, Cause::receiver_request);
        return valid_request;
    }

private:
    static constexpr std::uint64_t refresh_span = 1'400'000, refresh_max_tick = 25'000;
    static constexpr std::uint64_t min_key_budget = 500'000, max_key_budget = 2'000'000;
    static constexpr std::uint64_t key_retry_interval = 100'000, cut_interval = 400'000;
    static constexpr std::uint64_t local_window = 2'000'000, request_window = 10'000'000;
    static constexpr std::uint64_t min_hold = 10'000'000, max_hold = 300'000'000;
    static constexpr std::uint64_t fast_interval = 500'000, probe_interval = 1'500'000;
    static constexpr std::uint64_t verify_interval = 5'000'000, credit_cap = 250'000, idle_gap = 1'000'000;
    static constexpr std::uint64_t topoff_window = 2'000'000, quality_window = 500'000;
    static constexpr double quality_target = 42.0, quality_gain = 0.2;
    static constexpr int quantizer_target = 24;
    struct Sample { std::uint32_t sequence; std::uint64_t sent; std::size_t bytes; bool keyframe, behind_key; };
    std::uint32_t ceiling_, floor_, bitrate_, verified_, max_fps_, min_fps_, fps_;
    std::uint64_t budget_, key_budget_;
    bool adaptive_bitrate_, adaptive_fps_, feedback_, needs_key_ = true;
    bool pending_change_ = false, submitted_change_ = false, fast_rising_ = false;
    Stats stats_;
    std::optional<std::uint64_t> last_slot_, last_request_, last_key_sent_, key_ack_, topoff_since_, quality_start_;
    double initial_quality_ = 0;
    std::uint64_t encode_us_ = 0;
    unsigned slow_encodes_ = 0, consecutive_cuts_ = 0;
    std::uint64_t hold_until_ = 0, next_hold_ = min_hold;
    std::deque<Sample> flight_;
    std::deque<std::uint64_t> requests_;
    std::optional<std::uint32_t> latest_key_;
    std::optional<std::uint64_t> last_cut_, last_delay_, last_local_drop_, last_healthy_ack_;
    std::uint64_t healthy_credit_ = 0, rate_credit_ = 0, fps_credit_ = 0;
    std::uint32_t verification_rate_ = 0;
    std::optional<std::uint64_t> rtt_window_start_, previous_min_rtt_, delivery_start_;
    std::uint64_t current_min_rtt_ = 0, delivered_ = 0;

    // For prime capture rates there may be no lower admissible divisor.
    std::uint32_t rung_at_most(std::uint32_t limit) const {
        for (auto candidate = std::min(max_fps_, limit); candidate >= min_fps_; --candidate)
            if (max_fps_ % candidate == 0) return candidate;
        for (auto candidate = min_fps_; candidate < max_fps_; ++candidate)
            if (max_fps_ % candidate == 0) return candidate;
        return max_fps_;
    }

    bool step_fps_down() {
        if (!adaptive_fps_) return false;
        const auto next = rung_at_most(fps_ - 1);
        if (next >= fps_) return false;
        fps_ = next;
        return true;
    }

    std::uint32_t encoder_fps_limit() const {
        return encode_us_ ? rung_at_most(static_cast<std::uint32_t>(900'000 / encode_us_)) : max_fps_;
    }

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
        if (!delivery_start_ || loss_reported || now - *delivery_start_ > idle_gap) {
            delivery_start_ = now;
            delivered_ = 0;
            stats_.delivery_bps = 0;
            return;
        }
        delivered_ += bytes;
        const auto elapsed = now - *delivery_start_;
        if (elapsed >= credit_cap) {
            stats_.delivery_bps = delivered_ * 8'000'000 / elapsed;
            delivery_start_ = now;
            delivered_ = 0;
        }
    }

    void reset_health() {
        last_healthy_ack_.reset();
        healthy_credit_ = rate_credit_ = fps_credit_ = 0;
    }

    bool cut_bitrate(unsigned numerator, unsigned denominator) {
        if (!adaptive_bitrate_ || bitrate_ <= floor_) return false;
        if (fast_rising_) verified_ = std::max(floor_, static_cast<std::uint32_t>(std::uint64_t(verified_) * 7 / 8));
        fast_rising_ = false;
        bitrate_ = std::max(floor_, static_cast<std::uint32_t>(std::uint64_t(bitrate_) * numerator / denominator));
        return true;
    }

    void network_congestion(std::uint64_t now, Cause cause) {
        topoff_since_.reset();
        reset_health();
        last_delay_ = now;
        if (last_cut_ && now - *last_cut_ < cut_interval) return;
        last_cut_ = now;
        bool cut = cut_bitrate(3, 4);
        // Still overloaded after two rate reductions. Count opportunities even
        // at the bitrate floor so that a decoder bottleneck can be relieved.
        if (++consecutive_cuts_ > 2 && step_fps_down()) {
            hold_until_ = now + next_hold_;
            next_hold_ = std::min(max_hold, next_hold_ * 2);
            consecutive_cuts_ = 0;
            cut = true;
        }
        if (cut) ++stats_.cuts[static_cast<std::size_t>(cause)];
    }

    void recover_rate(std::uint64_t now) {
        const auto elapsed = last_healthy_ack_ ? now - *last_healthy_ack_ : 0;
        last_healthy_ack_ = now;
        // Exclude the first ACK after an idle period, retaining already earned
        // credit. An isolated ACK every few seconds must never raise the target.
        const auto credit = elapsed > idle_gap ? 0 : std::min(credit_cap, elapsed);
        if (!healthy_credit_) verification_rate_ = bitrate_;
        healthy_credit_ += credit;
        rate_credit_ += credit;
        fps_credit_ += credit;
        if (healthy_credit_ >= fast_interval) consecutive_cuts_ = 0;
        if (healthy_credit_ >= verify_interval) {
            if (verification_rate_ >= verified_) {
                verified_ = verification_rate_;
                fast_rising_ = false;
            }
            verification_rate_ = bitrate_;
            healthy_credit_ = 0;
        }
        const bool fast = std::uint64_t(bitrate_) * 100 < std::uint64_t(verified_) * 85;
        if (adaptive_bitrate_ && bitrate_ < ceiling_ && rate_credit_ >= (fast ? fast_interval : probe_interval)) {
            const auto increase = std::max(1u, bitrate_ / (fast ? 4 : 48));
            bitrate_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(fast ? verified_ : ceiling_,
                                                                     std::uint64_t(bitrate_) + increase));
            fast_rising_ = fast;
            rate_credit_ = 0;
        }
        if (adaptive_fps_ && fps_ < max_fps_ && now >= hold_until_ && fps_credit_ >= probe_interval) {
            const auto limit = encoder_fps_limit();
            for (auto next = fps_ + 1; next <= limit; ++next) {
                if (max_fps_ % next == 0) { fps_ = next; break; }
            }
            fps_credit_ = 0;
        }
    }
};
