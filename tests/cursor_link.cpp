#include "cursor_link.hpp"
#include "cursor_packet.hpp"
#include "secure.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace cursor;
using Packets = std::vector<Bytes>;
constexpr std::uint64_t token = 0x0102030405060708ull;
constexpr State visible{true, -11, 173};

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

Image bitmap(unsigned size = 8, unsigned seed = 1) {
    auto result = std::make_shared<Bitmap>();
    result->width = result->height = static_cast<std::uint16_t>(size);
    result->hot_x = -1;
    result->hot_y = 3;
    result->pixels.resize(size * size * 4);
    for (auto& byte : result->pixels) {
        seed = 1664525u * seed + 1013904223u;
        byte = static_cast<std::uint8_t>(seed >> 24);
    }
    return result;
}

Packets tick(Sender& sender, std::uint64_t now, State state, const Image& image) {
    Packets packets;
    sender.update(now, state, image, [&](const Bytes& bytes) { packets.push_back(bytes); });
    return packets;
}
Bytes accept(Receiver& receiver, std::uint64_t now, const Bytes& bytes) {
    auto ack = receiver.receive(now, bytes);
    require(ack.has_value(), "Rejected a valid cursor packet");
    return *ack;
}
void exchange(Sender& sender, Receiver& receiver, std::uint64_t now, State state, const Image& image) {
    for (const auto& packet : tick(sender, now, state, image))
        sender.acknowledge(now, accept(receiver, now, packet));
}
std::vector<unsigned> indices(const Packets& packets) {
    std::vector<unsigned> list;
    for (const auto& p : packets)
        if (p[8] == detail::shape_type) list.push_back(static_cast<unsigned>(detail::get(p, 27, 2)));
    return list;
}

void layout_and_visibility() {
    Sender sender(token);
    Receiver receiver(token);
    const auto image = bitmap();
    const auto packets = tick(sender, 0, visible, image);
    require(packets.size() == 2 && packets[0].size() == 30 && packets[1].size() == 29 + 8*8*4, "Wire sizes");
    const Bytes prefix{1,2,3,4,5,6,7,8, 1,1, 0,0,0,1, 0xff,0xff,0xff,0xf5, 0,0,0,173};
    require(std::equal(prefix.begin(), prefix.end(), packets[0].begin()), "STATE byte order");
    require(detail::get(packets[0], 22, 8) == shape_id(*image), "Missing content ID");
    auto before = accept(receiver, 1, packets[0]);
    require(before.size() == 31 && detail::get(before, 9, 4) == 1 && !receiver.view().state.visible, "Missing bitmap");
    sender.acknowledge(2, before);
    auto ack = accept(receiver, 2, packets[1]);
    require(detail::get(ack, 21, 2) == 1 && sender.acknowledge(3, ack) && sender.shape_ready(), "Failed shape ACK");
    const auto view = receiver.view();
    require(same(view.state, visible) && view.image->pixels == image->pixels && view.image->hot_x == -1,
            "Incorrect absolute position, signed hotspot or image");
    const auto revision = receiver.revision();
    accept(receiver, 3, packets[1]);
    require(receiver.revision() == revision && !sender.acknowledge(100, ack) && sender.last_ack() == 3,
            "Duplicate ACK extended liveness or duplicate shape repainted");
    const auto tail = tick(sender, 4, visible, image);
    require(tail.size() == 1 && tail[0] == packets[0], "Did not send the unconditional final state");
    require(tick(sender, 253, visible, image).empty(), "Emitted before heartbeat");
    const auto beat = tick(sender, 254, visible, image);
    require(beat.size() == 1 && detail::get(beat[0], 10, 4) == 2, "Idle probe did not carry a fresh sequence");
    receiver.expire(2002);
    require(receiver.view().state.visible, "Expired active view early");
    receiver.expire(2003);
    require(!receiver.view().state.visible, "Stale pointer still visible");
    accept(receiver, 2004, beat[0]);
    require(receiver.view().state.visible, "New heartbeat did not restore view");
    accept(receiver, 2005, tick(sender, 2005, {}, {})[0]);
    require(!receiver.view().state.visible && !receiver.view().image, "Hidden state retains overlay");

    auto variant = std::make_shared<Bitmap>(*image);
    ++variant->hot_x;
    require(shape_id(*image) != shape_id(*variant), "Hotspot missing from shape identity");
}

void video_handoff_floor() {
    Sender sender(token);
    Receiver receiver(token);
    const auto image = bitmap();
    exchange(sender, receiver, 0, visible, image);
    State gated = visible;
    gated.video_floor = 0xaabbccddu;
    auto change = tick(sender, 4, gated, image);
    require(change.size() == 1 && change[0].size() == 34 && change[0][9] == 3 &&
            detail::get(change[0], 30, 4) == *gated.video_floor, "Handoff floor wire layout");
    auto final = tick(sender, 8, gated, image);
    require(final == change, "Handoff tail lost its floor");
    auto ack = accept(receiver, 9, final[0]); // First STATE was lost.
    require(receiver.view().state.video_floor == gated.video_floor, "Lost floor after tail repair");
    sender.acknowledge(9, ack);

    State moved = gated;
    ++moved.x;
    exchange(sender, receiver, 12, moved, image);
    require(same(receiver.view().state, moved), "Movement lost its outstanding handoff floor");
    auto next_shape = bitmap(8, 456);
    auto replacing = tick(sender, 16, moved, next_shape);
    accept(receiver, 16, replacing[0]);
    require(receiver.view().image->pixels == image->pixels && same(receiver.view().state, moved),
            "Pending image bypassed handoff fence");
    accept(receiver, 17, replacing[1]);
    require(receiver.view().image->pixels == next_shape->pixels && same(receiver.view().state, moved),
            "Newly assembled image lost its handoff floor");
    moved.video_floor.reset();
    auto clear = tick(sender, 20, moved, next_shape);
    require(clear[0].size() == 30 && clear[0][9] == 1, "Normal packet still includes a handoff floor");
    accept(receiver, 20, clear[0]);
    accept(receiver, 21, replacing[0]); // A delayed fenced STATE cannot undo the clear.
    require(!receiver.view().state.video_floor, "Old handoff floor reinstated on reordering");
    receiver.expire(2021);
    require(!receiver.view().state.visible && !receiver.view().state.video_floor, "Expired overlay retained a floor");

    auto bad = change[0];
    bad.pop_back();
    require(!receiver.receive(2022, bad), "Truncated handoff floor accepted");
    bad = clear[0];
    bad.push_back(0);
    require(!receiver.receive(2022, bad), "Unflagged handoff extension accepted");
    auto wrapped = visible;
    wrapped.video_floor = 0;
    require(detail::state_packet(token, 99, wrapped, 1).size() == 34, "Zero sequence floor lost on wrap");
}

void tail_rto_and_wrap() {
    Sender sender(token);
    Receiver receiver(token);
    const auto first = tick(sender, 0, {}, {});
    const auto last = tick(sender, 4, {false, 24, -16}, {});
    require(last.size() == 1 && first[0] != last[0], "No fresh absolute state");
    require(tick(sender, 8, {false, 24, -16}, {}) == last, "Final tail retransmitted stale coordinates");
    require(tick(sender, 39, {false, 24, -16}, {}).empty(), "Early RTO");
    require(tick(sender, 40, {false, 24, -16}, {}) == last, "Tail lost without a retry");
    require(tick(sender, 87, {false, 24, -16}, {}).empty() && tick(sender, 88, {false, 24, -16}, {}) == last,
            "RTO did not back off by 1.5");
    require(sender.acknowledge(91, accept(receiver, 90, last[0])), "Unacknowledged tail");
    require(!sender.acknowledge(101, accept(receiver, 100, first[0])), "Stale sequence revived liveness");
    require(tick(sender, 102, {false, 24, -16}, {}).empty(), "Acknowledged tail sent again");

    const auto image = bitmap();
    Receiver wrapped(token);
    auto packet = [&](std::uint32_t seq, int x) {
        return detail::state_packet(token, seq, {true, x, 2}, shape_id(*image));
    };
    accept(wrapped, 0, packet(0xfffffffe, 0));
    accept(wrapped, 0, detail::fragment_packet(token, shape_id(*image), *image, 0));
    accept(wrapped, 4, packet(0, 8));
    accept(wrapped, 8, packet(0xffffffff, 4));
    require(wrapped.view().state.x == 8 && wrapped.stats().gaps == 1, "Position regressed across wrap");
}

void selective_and_fast() {
    Sender sender(token);
    Receiver receiver(token);
    auto image = bitmap(80);
    auto launch = tick(sender, 0, visible, image);
    auto no_fragments = accept(receiver, 1, launch[0]);
    sender.acknowledge(1, no_fragments);
    accept(receiver, 1, launch[2]);
    auto crossed = accept(receiver, 1, launch[3]);
    require(detail::get(crossed, 21, 2) == 0 && detail::get(crossed, 23, 8) == 6, "Incorrect SACK bitmap");
    sender.acknowledge(2, crossed);
    auto retry = tick(sender, 4, visible, image);
    require(indices(retry) == std::vector<unsigned>{0}, "Two later fragments did not cause fast retransmit");
    auto repaired = accept(receiver, 5, retry.back());
    sender.acknowledge(6, repaired);
    sender.acknowledge(7, no_fragments);
    auto after = indices(tick(sender, 70, visible, image));
    require(!after.empty() && after.front() == 3, "Reordered SACK resurrected an acknowledged fragment");
}

void rtt_and_current_tail() {
    Sender sender(token);
    Receiver receiver(token);
    auto state = State{};
    tick(sender, 0, state, {});
    state.x = 1;
    auto latest = tick(sender, 4, state, {});
    sender.acknowledge(5, accept(receiver, 5, latest[0]));
    require(sender.stats().rtt_ms == 1, "Wrong RTT from directly acknowledged state");
    state.x = 2;
    auto moved = tick(sender, 8, state, {});
    require(tick(sender, 12, state, {}) == moved, "New final position not repeated");
    require(tick(sender, 19, state, {}).empty() && tick(sender, 20, state, {}) == moved,
            "RTO did not respect 8 ms floor");
}

void corrupted_assembly_recovers() {
    Sender sender(token);
    Receiver receiver(token);
    const auto image = bitmap(32); // Four fragments.
    auto initial = tick(sender, 0, visible, image);
    auto old_ack = accept(receiver, 1, initial[0]);
    sender.acknowledge(1, old_ack);
    initial[1].back() ^= 1;
    for (std::size_t i = 1; i + 1 < initial.size(); ++i)
        sender.acknowledge(1, accept(receiver, 1, initial[i]));
    accept(receiver, 1, initial.back()); // Drop the first ACK indicating hash failure.
    require(!receiver.view().state.visible, "Presented corrupt shape");
    exchange(sender, receiver, 4, visible, image); // Tail recovers the new assembly epoch.
    sender.acknowledge(5, old_ack);
    exchange(sender, receiver, 8, visible, image);
    require(sender.shape_ready() && receiver.view().image->pixels == image->pixels, "Lost hash-reset ACK froze shape");
}

void cache_switch_and_eviction() {
    Sender sender(token);
    Receiver receiver(token);
    auto a = bitmap(8, 1), b = bitmap(8, 2);
    exchange(sender, receiver, 0, visible, a);
    exchange(sender, receiver, 4, visible, b);
    require(sender.stats().shape_transfers == 2, "Missing initial transfer");
    auto cached = tick(sender, 8, visible, a);
    require(cached.size() == 1, "Cached arrow was transferred again");
    sender.acknowledge(9, accept(receiver, 8, cached[0]));
    require(receiver.view().image->pixels == a->pixels && sender.shape_ready(), "Cached shape lost");
    Receiver cold(token);
    auto assumed = tick(sender, 12, visible, b);
    require(assumed.size() == 1 && !sender.shape_ready(), "Cache guess marked confirmed");
    sender.acknowledge(13, accept(cold, 12, assumed[0]));
    exchange(sender, cold, 16, visible, b);
    require(sender.shape_ready() && cold.view().image->pixels == b->pixels, "Missing cached shape did not self-repair");

    Receiver lru(token);
    std::uint32_t seq = 0;
    for (unsigned i = 0; i < 65; ++i) {
        const auto img = i ? bitmap(8, i + 1) : a;
        accept(lru, i, detail::state_packet(token, ++seq, visible, shape_id(*img)));
        accept(lru, i, detail::fragment_packet(token, shape_id(*img), *img, 0));
    }
    auto evicted = accept(lru, 70, detail::state_packet(token, ++seq, visible, shape_id(*a)));
    require(detail::get(evicted, 21, 2) == 0, "LRU did not evict its oldest image");
}

void lossy_saturated_window() {
    Sender sender(token);
    Receiver receiver(token);
    auto image = bitmap(256);
    auto initial = tick(sender, 0, visible, image);
    require(initial.size() == 33, "Initial window was not 32 fragments");
    for (const auto& p : initial)
        require(p.size() + secure::datagram_overhead <= packet::max_payload, "Protected datagram exceeds MTU budget");
    std::size_t packet_number = 0;
    Bytes stale_state = initial[0];
    State state = visible;
    for (unsigned t = 4; t <= 4000; t += 4) {
        if (t < 900) state.x += 5; // Continuously moving cursor at 250 Hz.
        auto packets = tick(sender, t, state, image);
        std::reverse(packets.begin(), packets.end());
        Packets acks;
        for (auto& p : packets) {
            ++packet_number;
            if (packet_number % 5 == 0) continue;
            auto ack = accept(receiver, t, p);
            if (packet_number % 7) acks.push_back(std::move(ack));
        }
        std::reverse(acks.begin(), acks.end());
        for (const auto& ack : acks) sender.acknowledge(t, ack);
        if (t % 32 == 0) accept(receiver, t, stale_state);
    }
    require(sender.shape_ready() && receiver.view().image && receiver.view().image->pixels == image->pixels,
            "Maximum shape failed to converge under loss/reordering");
    require(same(receiver.view().state, state) && sender.stats().retransmits > 0 && receiver.stats().gaps > 0,
            "Final position did not converge");
}

void malformed_packets() {
    const auto image = bitmap(40);
    Receiver receiver(token);
    auto state = detail::state_packet(token, 1, visible, shape_id(*image));
    auto chunk = detail::fragment_packet(token, shape_id(*image), *image, 0);
    require(!receiver.receive(0, {}) && !receiver.receive(0, Bytes(1201)), "Accepted unrelated traffic");
    auto other = state;
    other[0] ^= 1;
    require(!receiver.receive(0, other), "Accepted wrong session token");
    state[9] = 0xfe;
    require(!receiver.receive(0, state), "Accepted invalid flags");
    for (auto idx : {17,19,25,27}) {
        auto bad = chunk;
        bad[idx] = 0xff;
        require(!receiver.receive(0, bad), "Accepted invalid fragment geometry or index");
    }
    chunk.pop_back();
    require(!receiver.receive(0, chunk), "Accepted truncated fragment");
    Sender sender(token);
    auto hello = tick(sender, 0, {}, {})[0];
    auto ack = accept(receiver, 0, hello);
    ack[12] = 50;
    require(!sender.acknowledge(0, ack) && !sender.last_ack(), "Unsent sequence credited");
    ack[12] = 1;
    ack[22] = 1;
    require(!sender.acknowledge(0, ack) && !sender.last_ack(), "Unsent shape credited");
}
} // namespace

int main() {
    try {
        layout_and_visibility();
        video_handoff_floor();
        tail_rto_and_wrap();
        selective_and_fast();
        rtt_and_current_tail();
        corrupted_assembly_recovers();
        cache_switch_and_eviction();
        lossy_saturated_window();
        malformed_packets();
        std::cout << "Cursor state, shape transfer, cache and loss recovery passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
