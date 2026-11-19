#include "frame_recovery.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using Action = FrameRecovery::Action;
using Reason = FrameRecovery::Reason;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void loss_and_reordering() {
    FrameRecovery stream(100);
    require(stream.inspect(1, 0, false, 9000000) == Action::drop, "initial delta entered decoder");
    require(stream.waiting_for_keyframe(), "initial delta did not request a key");
    require(stream.reason() == Reason::startup, "Initial key request misclassified");
    require(stream.inspect(2, 1000, true, 9001000) == Action::restart, "first random access unit rejected");
    require(stream.inspect(3, 2000, false, 9002000) == Action::decode, "contiguous delta rejected");
    require(stream.inspect(5, 4000, false, 9004000) == Action::drop, "missing reference went undetected");
    require(stream.reason() == Reason::sequence_gap, "Gap key request misclassified");
    require(stream.inspect(4, 3000, true, 9010000) == Action::drop, "out-of-order key reopened decoder");
    require(stream.sequence() == 5 && stream.waiting_for_keyframe(), "late packet changed ACK or key request");
    require(stream.inspect(6, 5000, false, 9005000) == Action::drop, "dependent frame entered damaged GOP");
    require(stream.reason() == Reason::sequence_gap, "Waiting for a key lost the gap reason");
    require(stream.inspect(7, 6000, true, 9006000) == Action::restart, "fresh key rejected after gap");
    require(stream.inspect(7, 6000, true, 9007000) == Action::drop, "duplicate key was decoded");
    require(!stream.waiting_for_keyframe(), "duplicate invalidated a healthy GOP");
    require(stream.inspect(10, 9000, true, 9009000) == Action::restart, "gap before key did not reset decoder");
    stream.recover();
    require(stream.inspect(11, 10000, false, 9010000) == Action::drop, "decode error failed to suppress deltas");

    FrameRecovery wrap(25);
    require(wrap.inspect(0xfffffffeu, 1000, true, 0) == Action::restart, "wrap setup failed");
    require(wrap.inspect(0xffffffffu, 2000, false, 1000) == Action::decode, "premature sequence wrap");
    require(wrap.inspect(0, 3000, false, 2000) == Action::decode, "wrapped sequence treated as loss");
    require(wrap.inspect(0xffffffffu, 2000, true, 9000) == Action::drop, "duplicate across wrap accepted");
}

void deadlines_and_clocks() {
    FrameRecovery stream(50);
    constexpr std::uint64_t epoch = 9000000000000ULL;
    require(stream.inspect(20, epoch, true, 999) == Action::restart, "sender epoch affected initial latency");
    require(stream.inspect(21, epoch + 100000, false, 151009) == Action::decode, "frame at budget dropped");
    require(stream.remaining_us() == 0, "incorrect presentation allowance at budget");
    require(stream.inspect(22, epoch + 101000, true, 152010) == Action::drop, "overdue key entered decoder");
    require(stream.waiting_for_keyframe(), "timeout did not request resync");
    require(stream.reason() == Reason::transit_deadline, "Late key request misclassified");
    require(stream.inspect(23, epoch + 151000, false, 152011) == Action::drop, "delta reopened GOP after timeout");
    require(stream.inspect(24, epoch + 152050, true, 152059) == Action::restart, "fresh key remained blocked");
    require(stream.remaining_us() == 50000, "new best transit did not restore presentation allowance");
    require(stream.inspect(25, 100, false, 153000) == Action::drop, "sender clock reset did not end GOP");
    require(stream.reason() == Reason::clock_reset, "Clock reset key request misclassified");
    require(stream.inspect(26, 1100, true, 154000) == Action::restart, "clock reset stalled recovery");
    require(stream.inspect(27, 2100, false, 200) == Action::drop, "receiver clock reset kept old references");
    require(stream.inspect(28, 3100, true, 1200) == Action::restart, "receiver clock reset stalled recovery");

    // Windows advanced during sleep while the Mac timestamp origin stood still.
    // A single stale IDR must be skipped. After a sustained change a new IDR
    // establishes a new epoch even if the old transit minimum is unreachable.
    require(stream.inspect(29, 4100, true, 5000000) == Action::drop, "stale key shown on resume");
    require(stream.inspect(30, 504100, true, 5500000) == Action::drop, "clock rebased on the first stale GOP");
    require(stream.inspect(31, 2004100, false, 7000000) == Action::drop, "clock rebased on dependent data");
    require(stream.inspect(32, 2014100, true, 7010000) == Action::restart, "permanent clock offset stalled decoder");
    require(stream.inspect(33, 2015100, false, 7011000) == Action::decode, "resume failed to restore the stream");
}

void clock_drift() {
    FrameRecovery stream(200);
    constexpr std::uint32_t samples = 2 * 3600 * 60;
    for (std::uint32_t i = 0; i <= samples; ++i) {
        const auto sent = std::uint64_t(i) * 1'000'000 / 60;
        const auto arrival = sent + sent / 20'000; // Receiver runs 50 ppm fast.
        const auto action = stream.inspect(i, sent, i == 0, arrival);
        require(action == (i ? Action::decode : Action::restart), "clock skew broke a healthy GOP");
    }
    constexpr std::uint64_t next = 7'200'016'667ULL;
    require(stream.inspect(samples + 1, next, false, next + next / 20'000 + 210'000) == Action::drop,
            "clock drift allowance masked a real 210 ms queue");
}

void recovery_pictures() {
    const std::vector<std::uint8_t> avc_key{0,0,0,1,0x67,0x42,0,0,1,0x68,0xaa,0,0,0,1,0x65,0x88};
    require(has_recovery_picture(Codec::h264, avc_key), "AVC IDR with SPS/PPS rejected");
    auto delta = avc_key;
    delta[15] = 0x41;
    require(!has_recovery_picture(Codec::h264, delta), "AVC delta accepted as IDR");
    require(!has_recovery_picture(Codec::h264, {0,0,1,0x65,0x88}), "AVC IDR missing sets accepted");
    require(!has_recovery_picture(Codec::h264, {0,0,1,0x65}), "truncated AVC NAL accepted");

    const std::vector<std::uint8_t> hevc_key{0,0,1,64,1,128, 0,0,0,1,66,1,128, 0,0,1,68,1,128,
                                             0,0,1,38,1,128};
    require(has_recovery_picture(Codec::h265, hevc_key), "HEVC IDR with VPS/SPS/PPS rejected");
    auto invalid = hevc_key;
    invalid[22] = 2;
    require(!has_recovery_picture(Codec::h265, invalid), "HEVC delta accepted as IRAP");
    invalid = hevc_key;
    invalid[3] = 70; // Replace VPS with an access unit delimiter.
    require(!has_recovery_picture(Codec::h265, invalid), "HEVC IRAP without VPS accepted");
}

void variable_cadence() {
    FrameRecovery stream(200);
    stream.inspect(1, 0, true, 0);
    require(stream.idle_drain_us(60, 200'000) < 34'000, "Strong initial delay for single static frame");
    stream.inspect(2, 80'000, false, 80'000);
    require(stream.idle_drain_us(60, 200'000) == 160'000, "Slow stream would flush before the next picture");
    require(stream.idle_drain_us(60, 10'000) == 5'000, "Idle drain exceeded remaining deadline");
    stream.inspect(3, 6'000'000, false, 6'000'000);
    require(stream.idle_drain_us(60, 200'000) < 34'000, "Idle pause permanently enlarged presentation delay");
}
}

int main() {
    loss_and_reordering();
    deadlines_and_clocks();
    clock_drift();
    recovery_pictures();
    variable_cadence();
}
