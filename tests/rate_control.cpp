#include "rate_control.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void idle_recovery() {
    StreamControl control(8'000'000, 60, 200, true, true);
    expect(control.take_key_request(0), "Initial frame must be a key");
    expect(!control.accept_encoded(5, false), "Predicted startup frame escaped");
    expect(control.accept_encoded(6, true), "Actual keyframe was rejected");
    control.sent(6, 10'000, 200'000);
    expect(!control.poll(209'999), "Frame expired too early");
    expect(control.poll(210'000), "Lost final frame never expired");
    expect(!control.accept_encoded(7, false), "Broken GOP escaped");
    expect(control.take_key_request(210'000), "Idle recovery needs a new key");
    expect(!control.take_key_request(220'000), "Key request repeated too early");
    expect(control.accept_encoded(8, true), "Recovery IDR was rejected");
    control.sent(8, 240'000, 80'000);
    expect(!control.acknowledge(6, true, 241'000), "Old feedback discarded current GOP");
    expect(!control.acknowledge(8, false, 242'000), "Normal ACK interrupted the GOP");
    expect(!control.poll(600'000), "Acknowledged final frame incorrectly expired");
    expect(control.acknowledge(8, true, 601'000), "Late request after an ACK was ignored");
}

void adaptation() {
    constexpr std::uint32_t ceiling = 8'000'000;
    StreamControl control(ceiling, 60, 200, true, true);
    control.accept_encoded(1, true);
    control.sent(1, 0, 16'000);
    control.acknowledge(1, false, 4'000);
    control.sent(2, 30'000, 16'000);
    control.acknowledge(2, false, 110'000);
    const auto reduced = control.bitrate();
    expect(reduced == 6'000'000, "Queuing delay should trigger multiplicative reduction");
    control.loss(120'000);
    expect(control.bitrate() == reduced, "Multiple congestion signals caused repeated reductions");
    control.accept_encoded(3, true);
    std::uint64_t now = 200'000;
    std::uint32_t seq = 3;
    for (; now < 1'600'000; now += 20'000, ++seq) {
        control.sent(seq, now, 4'000);
        control.acknowledge(seq, false, now + 5'000);
    }
    expect(control.bitrate() == reduced, "Rate increased before sustained healthy delivery");
    for (; now < 1'900'000; now += 20'000, ++seq) {
        control.sent(seq, now, 4'000);
        control.acknowledge(seq, false, now + 5'000);
    }
    expect(control.bitrate() > reduced && control.bitrate() < ceiling, "Missing gradual rate recovery");
    const auto increased = control.bitrate();
    control.loss(now + 10'000);
    expect(control.bitrate() < increased * 3 / 4, "Measured delivery rate did not constrain congestion target");
    for (int i = 0; i < 20; ++i) control.loss(now += 500'000);
    expect(control.bitrate() == ceiling / 8, "Bitrate floor violated");
    for (int i = 0; i < 8000; ++i, ++seq) {
        now += 20'000;
        control.sent(seq, now, 4'000);
        control.acknowledge(seq, false, now + 5'000);
    }
    expect(control.bitrate() == ceiling, "Recovery failed to respect the configured ceiling");

    control.loss(now += 300'000);
    const auto idle_rate = control.bitrate();
    for (int i = 0; i < 3; ++i, ++seq) {
        now += 8'000'000;
        control.sent(seq, now, 500);
        control.acknowledge(seq, false, now + 5'000);
    }
    expect(control.bitrate() == idle_rate, "Sparse ACKs incorrectly counted as sustained delivery");
}

void latency_baseline() {
    StreamControl control(4'000'000, 60, 200, true, true);
    control.sent(1, 1'000, 8'000);
    control.acknowledge(1, false, 5'000);
    std::uint32_t floor = control.bitrate();
    // The route changes from 4ms to a stable 90ms, still within the deadline.
    for (unsigned seq = 2; seq <= 160; ++seq) {
        const auto sent = seq * 100'000u;
        control.sent(seq, sent, 8'000);
        control.acknowledge(seq, false, sent + 90'000);
        floor = std::min(floor, control.bitrate());
    }
    expect(floor < 4'000'000 && control.bitrate() > floor, "RTT baseline never adapted to the new route");
}

void fixed_and_wrapped() {
    StreamControl fixed(120'000, 30, 150, false, true);
    fixed.accept_encoded(0xffffffffu, true);
    fixed.sent(0xffffffffu, 100, 1000);
    expect(fixed.acknowledge(0, true, 130'000), "Wraparound broke key requests");
    expect(fixed.bitrate() == 120'000, "Fixed bitrate was adjusted");
    expect(fixed.accept_encoded(0, true), "Wrapped IDR was rejected");
    expect(!fixed.acknowledge(0xffffffffu, true, 130'001), "Pre-wrap request reset new GOP");

    StreamControl legacy(6'000'000, 30, 100, true, false);
    legacy.accept_encoded(1, true);
    legacy.sent(1, 0, 1000);
    expect(!legacy.poll(10'000'000), "Legacy stream expected an ACK");
    legacy.loss(10'000'000);
    expect(legacy.needs_key() && legacy.bitrate() == 6'000'000, "Legacy drop lost its recovery key");

    StreamControl congested(1'000'000, 10, 100, true, true);
    congested.accept_encoded(1, true);
    for (unsigned i = 1; i <= 5; ++i) congested.sent(i, 0, 1000);
    expect(congested.poll(500), "Excessive in-flight frames failed to trigger recovery");
}

} // namespace

int main() {
    try {
        idle_recovery();
        adaptation();
        latency_baseline();
        fixed_and_wrapped();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
