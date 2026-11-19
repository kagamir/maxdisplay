#pragma once

#include "media.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

// Our H.26x sender includes the parameter sets in each independently decodable
// access unit. Verifying the first VCL NAL prevents a mislabeled delta from
// seeding a new decoder. The AV1 decoder validates the key and sequence header
// together with libaom when it receives the first temporal unit after reset.
inline bool has_recovery_picture(Codec codec, const std::vector<std::uint8_t>& bytes) {
    if (codec == Codec::av1) return !bytes.empty();
    bool vps = false, sps = false, pps = false;
    for (std::size_t i = 2; i + 2 < bytes.size(); ++i) {
        if (bytes[i - 2] != 0 || bytes[i - 1] != 0 || bytes[i] != 1) continue;
        const auto header = bytes[i + 1];
        if (header & 0x80) return false;
        if (codec == Codec::h264) {
            const unsigned type = header & 0x1f;
            if (type == 7) sps = true;
            if (type == 8) pps = true;
            if (type >= 1 && type <= 5) return type == 5 && sps && pps;
        } else {
            if ((bytes[i + 2] & 7) == 0 || i + 3 >= bytes.size()) return false;
            const unsigned type = (header >> 1) & 0x3f;
            if (type == 32) vps = true;
            if (type == 33) sps = true;
            if (type == 34) pps = true;
            if (type <= 31) return type >= 16 && type <= 21 && vps && sps && pps;
        }
    }
    return false;
}

class FrameRecovery {
public:
    enum class Action { drop, restart, decode };

    explicit FrameRecovery(std::uint32_t timeout_ms) : budget_us_(std::uint64_t(timeout_ms) * 1000) {}

    Action inspect(std::uint32_t sequence, std::uint64_t timestamp_us, bool keyframe,
                   std::uint64_t arrival_us) {
        if (started_) {
            const std::uint32_t advance = sequence - sequence_;
            if (advance == 0 || advance >= 0x80000000u) return Action::drop;
            if (advance != 1) recover();
            if (arrival_us < arrival_us_ || timestamp_us < timestamp_us_) {
                // New clock origin. Reference pictures may also have been lost.
                recover();
                delay_us_ = 0;
                cadence_us_ = 0;
                tracking_late_ = false;
            } else {
                // Difference of elapsed times removes the arbitrary epochs on
                // the two hosts. Clamping at zero tracks the best transit seen.
                const auto received = arrival_us - arrival_us_;
                const auto sent = timestamp_us - timestamp_us_;
                if (advance == 1) {
                    // Follow a lower send rate immediately. A long static pause
                    // and the next recovery key are not a new stream cadence.
                    if (sent > 1'000'000) cadence_us_ = 0;
                    else cadence_us_ = std::max(sent, (3 * cadence_us_ + sent) / 4);
                }
                if (received > sent) delay_us_ += received - sent;
                else delay_us_ -= std::min(delay_us_, sent - received);
            }
        }
        started_ = true;
        sequence_ = sequence;
        timestamp_us_ = timestamp_us;
        arrival_us_ = arrival_us;

        if (delay_us_ > budget_us_) {
            recover();
            if (!tracking_late_) {
                late_since_ = arrival_us;
                tracking_late_ = true;
            }
            // Suspend/resume and persistent route changes can invalidate the
            // old minimum. Re-establish the clock on a new random access point
            // only after allowing queued pictures and recovery requests to pass.
            if (!keyframe || arrival_us - late_since_ < std::max<std::uint64_t>(1500000, 4 * budget_us_))
                return Action::drop;
            delay_us_ = 0;
        }
        tracking_late_ = false;
        if (!waiting_) return Action::decode;
        if (!keyframe) return Action::drop;
        waiting_ = false;
        return Action::restart;
    }

    void recover() { waiting_ = true; }
    bool waiting_for_keyframe() const { return waiting_; }
    std::uint32_t sequence() const { return sequence_; }
    std::uint64_t remaining_us() const { return budget_us_ - std::min(budget_us_, delay_us_); }
    std::uint64_t idle_drain_us(std::uint32_t max_fps, std::uint64_t remaining) const {
        const auto period = std::max<std::uint64_t>(1'000'000 / max_fps, cadence_us_);
        const auto readback_margin = std::min<std::uint64_t>(20'000, remaining / 2);
        return std::min(2 * period, remaining - readback_margin);
    }

private:
    std::uint64_t budget_us_;
    std::uint64_t arrival_us_ = 0, timestamp_us_ = 0, delay_us_ = 0, late_since_ = 0;
    std::uint64_t cadence_us_ = 0;
    std::uint32_t sequence_ = 0;
    bool started_ = false, waiting_ = true, tracking_late_ = false;
};
