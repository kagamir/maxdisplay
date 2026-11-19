#include "stream_sim.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace stream_sim;

void check(bool condition, const char* message, const StreamSim& sim) {
    if (!condition)
        throw std::runtime_error(std::string(message) + " (bitrate " + std::to_string(sim.control.bitrate()) +
                                 ", fps " + std::to_string(sim.control.fps()) + ", keys " +
                                 std::to_string(sim.keys_sent) + ", presented " + std::to_string(sim.presented) + ")");
}

void periodic_key_ratchet() {
    Scenario s;
    s.key_bytes = 300'000;
    s.target_utilization = .78; // measured H.264 motion workload
    s.capacity = [](Time) { return 30'000'000u; };
    // Deliberately force periodic IDRs even on a feedback connection. This
    // tests immunity to large keys independently of disabling them by default.
    // Before adaptation was decoupled, this trace ended at 3.26/12 Mbps.
    s.periodic_frames = 120;
    StreamSim sim(s);
    sim.run_until(30 * second);
    check(sim.keys_sent >= 14 && sim.presented > 1700, "Periodic IDRs stalled motion", sim);
    check(sim.control.bitrate() == s.bitrate && sim.control.fps() == s.fps,
          "Periodic IDRs ratcheted down the stream", sim);
}

void startup_key_loop() {
    Scenario s;
    s.bitrate = 4'000'000;
    s.key_bytes = 350'000;
    s.capacity = [](Time) { return 10'000'000u; }; // key takes 280 ms vs. 200 ms ordinary deadline
    s.active = [](Time t) { return t == 0 || t > 500'000; };
    StreamSim sim(s);
    sim.run_until(3 * second);
    check(sim.keys_sent == 1 && sim.presented > 140 && sim.control.bitrate() == s.bitrate,
          "Sender replaced its initial key before delivery", sim);

    s.active = [](Time) { return true; };
    StreamSim scrolling(s);
    scrolling.run_until(3 * second);
    check(scrolling.keys_sent == 1 && scrolling.presented > 165,
          "First delta behind the startup key triggered another GOP reset", scrolling);
}

void bursty_desktop_never_recovers(bool buffered) {
    Scenario s;
    s.buffer_one_frame = buffered;
    s.capacity = [](Time t) { return t < 10 * second && t >= 6 * second ? 4'000'000u : 40'000'000u; };
    s.active = [](Time t) { return t < 10 * second || (t - 10 * second) % (4 * second) < second; };
    StreamSim sim(s);
    sim.run_until(10 * second);
    const auto reduced = sim.control.bitrate();
    check(reduced < s.bitrate * 3 / 4, "Script failed to establish a congested bitrate", sim);
    const auto before = sim.presented;
    sim.run_until(42 * second);
    if (buffered) {
        // A drain after each pause requests a new GOP on the next delta. Three
        // such requests still earn the v3 repeated-loss cut within ten seconds.
        check(sim.idle_drains >= 7 && sim.control.bitrate() > 3 * reduced &&
              sim.presented - before >= 140,
              "Buffered decoder failed to recover between scroll bursts", sim);
    } else {
        check(sim.control.bitrate() >= s.bitrate * 85 / 100 && sim.control.bitrate() > reduced,
              "Idle gaps consumed recovery credit between scroll bursts", sim);
    }
}

void isolated_udp_loss() {
    Scenario s;
    s.transport = Transport::udp;
    s.lose_delta = [](std::uint32_t seq) { return seq == 70; };
    StreamSim sim(s);
    sim.run_until(3 * second);
    check(sim.udp_drops == 1 && sim.keys_sent == 2 && sim.presented > 165,
          "Lost UDP delta did not recover on the next key", sim);
    check(sim.control.bitrate() == s.bitrate && sim.control.fps() == s.fps,
          "Isolated UDP loss cut a healthy link", sim);
}

void slow_receiver_fat_link() {
    Scenario s;
    s.capacity = [](Time) { return 150'000'000u; };
    s.decode_us = 35'000;
    StreamSim sim(s);
    sim.run_until(10 * second);
    const auto presented = sim.presented;
    sim.run_until(14 * second);
    check(sim.control.fps() == 20,
          "FPS fallback failed to accommodate slow decoder", sim);
    check(sim.presented - presented >= 76 && sim.last_presented_at > 13 * second,
          "Slow decoder failed to present sustained motion", sim);
}

void capacity_step(Transport transport) {
    Scenario s;
    s.transport = transport;
    s.capacity = [](Time t) { return t >= 6 * second && t < 14 * second ? 4'000'000u : 40'000'000u; };
    // An encoder allocating per frame sheds network load as the FPS falls.
    // The other traces exercise timestamp-based allocation.
    s.allocation = Scenario::Allocation::frame;
    StreamSim sim(s);
    sim.run_until(6 * second);
    check(sim.control.bitrate() == s.bitrate, "Clean link underfilled before step", sim);
    sim.run_until(14 * second);
    check(sim.control.bitrate() < s.bitrate * 2 / 3, "Rate did not react to capacity step", sim);
    if (transport == Transport::udp) check(sim.udp_drops > 0, "Missing UDP queue tail loss", sim);
    const auto presented = sim.presented;
    sim.run_until(22 * second);
    check(sim.control.bitrate() >= s.bitrate * 85 / 100 && sim.presented - presented >= 80,
          "Opening link failed to restore delivery and bitrate", sim);
}

void encoder_limited() {
    Scenario s;
    s.encode_us = 40'000;
    StreamSim sim(s);
    sim.run_until(5 * second);
    check(sim.control.fps() == 20 && sim.control.bitrate() == s.bitrate,
          "Encoder overload failed to choose sustainable FPS independently of bitrate", sim);
    check(sim.presented >= 90 && sim.keys_sent == 1, "Slow encoder lost useful frames", sim);
}

void static_after_motion(unsigned fps, bool buffered) {
    Scenario s;
    s.fps = fps;
    s.buffer_one_frame = buffered;
    s.active = [](Time t) { return t < 2 * second; };
    s.psnr = [](unsigned repeats) { return repeats < 12 ? 36. : 43.; };
    StreamSim sim(s);
    sim.run_until(4 * second);
    if (buffered) check(sim.idle_drains == 1 && sim.keys_sent == 1,
                        "Buffered decoder drained before refinement arrived", sim);
    check(sim.repeats_encoded >= 12 && sim.presented_psnr >= 42.,
          "Small skipped pictures prevented a later quality refinement", sim);
    const auto count = sim.presented;
    sim.run_until(8 * second);
    check(sim.presented == count && sim.repeats_encoded < 25, "Static refinement failed to stop", sim);
}
} // namespace

int main() {
    try {
        periodic_key_ratchet();
        startup_key_loop();
        bursty_desktop_never_recovers(false);
        bursty_desktop_never_recovers(true);
        isolated_udp_loss();
        slow_receiver_fat_link();
        capacity_step(Transport::tcp);
        capacity_step(Transport::udp);
        encoder_limited();
        static_after_motion(60, false);
        static_after_motion(48, true);
        static_after_motion(60, true);
        static_after_motion(120, true);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
