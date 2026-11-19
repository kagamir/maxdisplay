#include "rate_control.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using Cause = StreamControl::Cause;
using Capture = StreamControl::Capture;
constexpr std::uint32_t ceiling = 8'000'000;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Link {
    StreamControl control{ceiling, 60, 200, true, true, true};
    std::uint64_t now = 0;
    std::uint32_t sequence = 0;

    void ack(std::uint64_t spacing = 20'000, std::uint64_t rtt = 4'000) {
        now += spacing;
        control.sent(++sequence, now, 8'000, false);
        control.acknowledge(sequence, false, now + rtt);
    }
    void healthy(unsigned frames) {
        for (unsigned i = 0; i < frames; ++i) ack();
    }
    void cut() { control.loss(now += 500'000, Cause::send_blocked); }
};

void key_deadline_extended() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    expect(control.next_capture(0, true) == Capture::force_key, "Initial frame must be a key");
    expect(!control.accept_encoded(5, false), "Predicted startup frame escaped");
    expect(control.accept_encoded(6, true), "Actual keyframe was rejected");
    control.sent(6, 10'000, 200'000, true);
    for (unsigned i = 1; i <= 20; ++i) control.sent(6 + i, 10'000 + 16'667 * i, 4'000, false);
    expect(!control.poll(809'999), "Startup key was discarded at the ordinary flight limit");
    expect(control.poll(810'000), "Lost final key never expired");
    expect(!control.accept_encoded(27, false), "Broken GOP escaped");
    expect(control.next_capture(810'000, false) == Capture::force_key, "Idle recovery needs a key");
    expect(control.next_capture(820'000, false) == Capture::skip, "Key request repeated too early");
    expect(control.accept_encoded(28, true), "Recovery IDR was rejected");
    control.sent(28, 840'000, 80'000, true);
    expect(!control.acknowledge(26, true, 841'000), "Old feedback discarded current GOP");
    expect(!control.acknowledge(28, false, 842'000), "Normal ACK interrupted the GOP");
    expect(!control.poll(1'900'000), "Acknowledged final frame incorrectly expired");
    expect(control.acknowledge(28, true, 1'901'000), "Late request after an ACK was ignored");

    // A coalesced ACK for a delta can cover a large startup key. A lost
    // following delta still gets a bounded recovery deadline on an idle screen.
    control.accept_encoded(29, true);
    control.sent(29, 2'000'000, 320'000, true);
    control.sent(30, 2'017'000, 4000, false);
    control.sent(31, 2'034'000, 4000, false);
    control.acknowledge(30, false, 2'300'000);
    expect(!control.poll(2'499'999), "Delta expired before it could drain behind the key");
    expect(control.poll(2'500'000), "Missing final delta escaped after a large key");
}

void key_rtt_is_neutral() {
    Link link;
    link.ack();
    link.control.sent(2, 50'000, 320'000, true);
    link.control.sent(3, 70'000, 16'000, false);
    link.control.acknowledge(2, false, 210'000);
    link.control.acknowledge(3, false, 220'000);
    expect(link.control.bitrate() == ceiling && link.control.fps() == 60, "Key serialization cut rates");
    const auto stats = link.control.stats();
    expect(stats.frames == 3 && stats.keyframes == 1 && stats.key_bytes == 320'000 &&
           stats.key_rtt_us == 160'000 && stats.rtt_us == 150'000 && stats.queue_us == 146'000,
           "Wrong key/latency telemetry");
    link.control.sent(4, 230'000, 16'000, false);
    link.control.acknowledge(4, false, 430'000);
    expect(link.control.bitrate() == ceiling * 3 / 4, "Full-budget delta behind a key escaped");
    link.control.sent(5, 1'000'000, 100'000, true);
    link.control.acknowledge(5, false, 1'801'000);
    expect(link.control.bitrate() == ceiling * 9 / 16, "Unbounded key RTT ignored");
}

void local_drop_keeps_bitrate() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    control.accept_encoded(1, true);
    control.loss(0, Cause::stale_capture);
    control.accept_encoded(2, true);
    control.loss(2'100'000, Cause::slow_encode);
    expect(control.fps() == 60, "Unrelated local losses counted together");
    control.accept_encoded(3, true);
    control.loss(2'200'000, Cause::stale_capture);
    expect(control.needs_key() && control.bitrate() == ceiling && control.fps() == 30,
           "Local drops did not independently adapt FPS");
    auto i = static_cast<std::size_t>(Cause::stale_capture);
    expect(control.stats().cuts[i] == 1 && control.stats().key_requests[i] == 2, "Wrong local cause counters");
}

void key_request_without_delay_keeps_rates() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    control.accept_encoded(1, true);
    control.sent(1, 0, 2000, true);
    expect(control.acknowledge(1, true, 10'000), "Isolated request did not start a fresh GOP");
    expect(control.bitrate() == ceiling && control.fps() == 60, "Isolated request cut rates");
}

void repeated_loss_trims_bitrate() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    for (unsigned i = 0; i < 4; ++i) {
        control.accept_encoded(i + 1, true);
        const auto now = i ? 11'000'000 + i * 100'000 : 0;
        expect(control.acknowledge(i + 1, true, now), "Receiver request lost");
        if (i < 3) expect(control.bitrate() == ceiling, "Losses outside 10s counted together");
    }
    expect(control.bitrate() == ceiling * 7 / 8 && control.fps() == 60, "Wrong repeated-loss response");
    control.accept_encoded(5, true);
    control.sent(6, 12'000'000, 2'000, false);
    control.acknowledge(6, true, 12'201'000);
    expect(control.bitrate() == ceiling * 7 / 8 * 3 / 4, "Delayed key request missed network congestion");
}

void congestion_keeps_fps() {
    Link link;
    link.control.accept_encoded(1, true);
    link.ack();
    link.ack(100'000, 90'000);
    expect(link.control.bitrate() == ceiling * 3 / 4 && link.control.fps() == 60 &&
           !link.control.needs_key(), "ACK delay must affect only bitrate");
    link.control.loss(link.now + 100'000, Cause::backlog);
    expect(link.control.bitrate() == ceiling * 3 / 4 && link.control.needs_key(), "Burst of losses cut twice");
}

void unrelieved_delay_steps_fps() {
    Link link;
    link.cut();
    link.cut();
    expect(link.control.fps() == 60, "Fallback did not wait for two rate reductions");
    link.cut();
    expect(link.control.fps() == 30, "Unrelieved congestion did not reduce FPS");
    link.healthy(490);
    expect(link.control.fps() == 30, "Fallback ended before 10s");
    link.healthy(15);
    expect(link.control.fps() == 60, "FPS never recovered after hold");
    link.cut();
    link.cut();
    link.cut();
    link.healthy(980);
    expect(link.control.fps() == 30, "Second fallback did not double its hold");
    link.healthy(30);
    expect(link.control.fps() == 60, "Second fallback never released");
}

void fixed_and_wrapped() {
    StreamControl fixed(120'000, 30, 150, false, false, true);
    fixed.accept_encoded(0xffffffffu, true);
    fixed.sent(0xffffffffu, 100, 1000, true);
    expect(fixed.acknowledge(0, true, 130'000), "Wraparound broke key requests");
    expect(fixed.bitrate() == 120'000, "Fixed bitrate was adjusted");
    expect(fixed.accept_encoded(0, true), "Wrapped IDR was rejected");
    expect(!fixed.acknowledge(0xffffffffu, true, 130'001), "Pre-wrap request reset new GOP");

    StreamControl legacy(6'000'000, 30, 100, true, true, false);
    legacy.accept_encoded(1, true);
    legacy.sent(1, 0, 1000, true);
    expect(!legacy.poll(10'000'000), "Legacy stream expected an ACK");
    legacy.loss(10'000'000, Cause::backlog);
    expect(legacy.needs_key() && legacy.bitrate() == 6'000'000, "Legacy drop lost its recovery key");

    StreamControl congested(1'000'000, 10, 100, true, true, true);
    for (unsigned i = 1; i <= 5; ++i) congested.sent(i, 0, 1000, false);
    expect(congested.poll(500), "Excessive in-flight deltas failed to trigger recovery");
}

void latency_baseline() {
    Link link;
    link.ack();
    auto floor = ceiling;
    for (unsigned i = 0; i < 165; ++i) {
        link.ack(100'000, 90'000);
        floor = std::min(floor, link.control.bitrate());
    }
    expect(floor < ceiling && link.control.bitrate() > floor, "RTT baseline never adapted to the new route");
    expect(link.control.stats().delivery_bps == 640'000, "Wrong ACK delivery rate");
}

void credit_survives_idle_gaps() {
    Link link;
    link.healthy(260);
    link.cut();
    const auto reduced = link.control.bitrate();
    for (unsigned i = 0; i < 3; ++i) link.ack(8'000'000);
    expect(link.control.bitrate() == reduced, "Sparse ACKs incorrectly counted as sustained delivery");
    link.healthy(15);
    link.control.accept_encoded(link.sequence, true);
    expect(link.control.acknowledge(link.sequence, true, link.now + 6'000),
           "Receiver idle drain did not request a new key");
    link.ack(3'000'000);
    link.healthy(11);
    expect(link.control.bitrate() > reduced, "Idle pause discarded earned ACK credit");
}

void ramp_reaches_known_good_in_5s() {
    Link link;
    link.healthy(260);
    for (unsigned i = 0; i < 12; ++i) link.cut();
    expect(link.control.bitrate() == ceiling / 8, "Bitrate floor violated");
    link.healthy(250);
    expect(link.control.bitrate() >= ceiling * 85 / 100, "Recovery below 85% of verified rate after 5s");
    link.healthy(1500);
    expect(link.control.bitrate() == ceiling, "Controller failed to reach bitrate ceiling");
}

void stale_known_good_decays() {
    Link link;
    link.healthy(260);
    link.cut();                       // 6M
    link.healthy(26);                 // Fast recovery to 7.5M
    link.cut();                       // New verified target = 7M, current = 5.625M
    link.healthy(26);
    expect(link.control.bitrate() == 7'000'000, "Repeated congestion did not decay the verified target");
}

void plateau_slows_probing() {
    Link link;
    link.cut();                       // No prior 5s validation.
    link.healthy(75);
    expect(link.control.bitrate() == 6'000'000, "An unverified rate was treated as known good");
    link.healthy(1);
    expect(link.control.bitrate() == 6'125'000, "Slow probe failed to use target/48 at 1.5s");
}

void rungs_are_divisors() {
    StreamControl control(ceiling, 60, 200, false, true, true);
    std::uint64_t now = 0;
    for (auto expected : {30u, 20u, 15u, 12u, 10u, 10u}) {
        control.loss(now += 100'000, Cause::slow_encode);
        control.loss(now += 100'000, Cause::stale_capture);
        expect(control.fps() == expected && control.bitrate() == ceiling, "FPS did not follow capture divisors");
    }
    StreamControl cpu(ceiling, 60, 200, true, true, true);
    cpu.observe_encode_time(150'000, true);
    expect(cpu.fps() == 60, "Single slow IDR cut FPS");
    for (unsigned i = 0; i < 3; ++i) cpu.observe_encode_time(40'000, false);
    expect(cpu.fps() == 20 && cpu.bitrate() == ceiling, "Encode cost did not round down to a divisor");
    for (auto max_fps : {1u, 17u}) {
        StreamControl prime(ceiling, max_fps, 200, true, true, true);
        prime.loss(0, Cause::slow_encode);
        prime.loss(1, Cause::slow_encode);
        expect(prime.fps() == max_fps, "Prime input fell below the FPS floor");
    }
    StreamControl fixed(ceiling, 60, 200, true, false, true);
    for (unsigned i = 0; i < 16; ++i) fixed.loss(i * 500'000, Cause::backlog);
    expect(fixed.fps() == 60 && fixed.bitrate() == ceiling / 8, "Fixed FPS prevented bitrate adaptation");
}

void pacer_tolerates_capture_jitter() {
    StreamControl control(ceiling, 60, 200, false, true, true);
    control.loss(0, Cause::slow_encode);
    control.loss(10, Cause::slow_encode);
    expect(control.next_capture(100, true) == Capture::force_key && control.accept_encoded(1, true),
           "Missing initial sync");
    for (unsigned i = 1; i <= 60; ++i) {
        const auto now = 100 + i * 1'000'000 / 60 + (i % 3) * 600;
        const auto action = control.next_capture(now, true);
        expect((action == Capture::encode) == (i % 2 == 0), "Capture jitter disturbed 30 FPS cadence");
    }
    expect(control.next_capture(1'017'000, true) == Capture::skip, "Pacer ignored frame interval");
    expect(control.next_capture(1'034'000, false) == Capture::encode, "Lost last change on static screen");
}

void pacer_with_refresh_timer() {
    for (auto timer_phase : {0u, 1'000u, 5'000u, 10'000u, 16'000u, 20'000u}) {
        StreamControl control(ceiling, 60, 200, true, true, true);
        control.loss(0, Cause::slow_encode);
        control.loss(1, Cause::slow_encode);
        control.next_capture(10, true);
        control.accept_encoded(1, true);
        std::uint64_t tick = timer_phase + 10, last_encode = 10;
        unsigned count = 0;
        for (unsigned i = 1; i <= 120; ++i) {
            const std::uint64_t capture = 10 + i * 1'000'000 / 60;
            while (tick < capture) {
                if (control.next_capture(tick, false) == Capture::encode) {
                    expect(tick - last_encode >= 32'000, "Timer stole a slot during motion");
                    ++count;
                    last_encode = tick;
                }
                tick += 25'000;
            }
            if (control.next_capture(capture, true) == Capture::encode) {
                expect(capture - last_encode >= 32'000, "Timer disrupted capture cadence");
                ++count;
                last_encode = capture;
            }
        }
        expect(count == 60, "Wrong average FPS with interleaved refresh and capture");
    }
}

void skipped_encode_rearms_change() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    control.next_capture(0, true);
    control.accept_encoded(1, true);
    expect(control.next_capture(20'000, true) == Capture::encode, "Lost original change");
    control.encode_skipped();
    expect(control.next_capture(40'000, false) == Capture::encode, "Encoder drop lost the final desktop update");
    expect(control.next_capture(100'000, false) == Capture::skip, "Drop did not stop top-off");
}

void topoff_arms_after_change() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    control.next_capture(0, true);
    control.accept_encoded(1, true);
    expect(control.next_capture(19'000, false) == Capture::skip, "Timer inserted an extra motion frame");
    expect(control.next_capture(50'000, false) == Capture::encode, "Timer did not refine idle image");
    control.observe_quality(54'000, 43.0, {});
    expect(control.next_capture(75'000, false) == Capture::skip, "High-quality frame kept refining");
    expect(control.next_capture(100'000, true) == Capture::encode, "New change was suppressed");
    expect(control.next_capture(150'000, false) == Capture::encode, "New content did not rearm top-off");
}

void topoff_before_idle_drain() {
    for (auto fps : {1u, 24u, 30u, 48u, 60u, 120u}) {
        StreamControl control(ceiling, fps, 200, true, true, true);
        control.next_capture(0, true);
        control.accept_encoded(1, true);
        const auto period = 1'000'000u / fps;
        const auto tick = StreamControl::refresh_tick_us(fps);
        expect(tick > 0 && tick <= 25'000, "Watchdog slept past refresh deadline");
        auto first = tick;
        while (first < 2 * period && control.next_capture(first, false) == Capture::skip)
            first += tick;
        expect(first > period && first < 2 * period,
               "First refinement unavailable before two frame periods");
    }
}

void topoff_stops_on_quality() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    control.next_capture(0, true);
    control.accept_encoded(1, true);
    control.observe_quality(1'000, 30.0, {});
    control.observe_quality(501'000, 31.0, {});
    expect(control.next_capture(525'000, false) == Capture::encode, "Growing PSNR stopped too early");
    control.observe_quality(1'001'000, 31.1, {});
    expect(control.next_capture(1'025'000, false) == Capture::skip, "PSNR plateau did not stop top-off");
    control.next_capture(2'000'000, true);
    control.observe_quality(2'001'000, {}, 25);
    expect(control.next_capture(2'050'000, false) == Capture::encode, "AV1 stopped before target quantizer");
    control.observe_quality(2'051'000, {}, 24);
    expect(control.next_capture(2'075'000, false) == Capture::skip, "AV1 ignored target quantizer");
}

void topoff_stops_on_congestion() {
    Link link;
    link.control.next_capture(0, true);
    link.control.accept_encoded(1, true);
    link.ack();
    link.ack(100'000, 90'000);
    expect(link.control.next_capture(250'000, false) == Capture::skip, "Top-off continued through congestion");
    link.control.next_capture(300'000, true);
    link.control.loss(310'000, Cause::receiver_request);
    expect(link.control.next_capture(320'000, false) == Capture::force_key, "Top-off masked recovery");
    link.control.accept_encoded(5, true);
    expect(link.control.next_capture(360'000, false) == Capture::skip, "Recovery key restarted top-off");
}

void topoff_bounded_without_signal() {
    StreamControl control(ceiling, 60, 200, true, true, true);
    control.next_capture(0, true);
    control.accept_encoded(1, true);
    for (unsigned i = 2; i < 80; ++i)
        expect(control.next_capture(i * 25'000, false) == Capture::encode, "Signal-less top-off ended early");
    expect(control.next_capture(2'000'000, false) == Capture::skip, "Signal-less top-off exceeded 2s");
}

} // namespace

int main() {
    try {
        key_deadline_extended();
        key_rtt_is_neutral();
        local_drop_keeps_bitrate();
        key_request_without_delay_keeps_rates();
        repeated_loss_trims_bitrate();
        congestion_keeps_fps();
        unrelieved_delay_steps_fps();
        fixed_and_wrapped();
        latency_baseline();
        credit_survives_idle_gaps();
        ramp_reaches_known_good_in_5s();
        stale_known_good_decays();
        plateau_slows_probing();
        rungs_are_divisors();
        pacer_tolerates_capture_jitter();
        pacer_with_refresh_timer();
        skipped_encode_rearms_change();
        topoff_arms_after_change();
        topoff_before_idle_drain();
        topoff_stops_on_quality();
        topoff_stops_on_congestion();
        topoff_bounded_without_signal();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
