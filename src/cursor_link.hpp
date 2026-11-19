#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cursor {

using Bytes = std::vector<std::uint8_t>;
struct Bitmap {
    std::uint16_t width = 0, height = 0;
    std::int16_t hot_x = 0, hot_y = 0;
    Bytes pixels; // Top-down, tightly packed premultiplied BGRA.
};
using Image = std::shared_ptr<const Bitmap>;

struct State {
    bool visible = false;
    std::int32_t x = 0, y = 0; // Hotspot in eighths of a video pixel.
    std::optional<std::uint32_t> video_floor{}; // First cursor-free video frame at handoff.
};
struct View { State state; Image image; };
struct SenderStats {
    double rtt_ms = 0;
    std::uint64_t sent = 0, retransmits = 0, shape_transfers = 0;
};
struct ReceiverStats { std::uint64_t updates = 0, gaps = 0; };

inline bool same(State a, State b) {
    return a.visible == b.visible && a.x == b.x && a.y == b.y && a.video_floor == b.video_floor;
}

namespace detail {
// All integers are big-endian. The common prefix is token:u64, type:u8.
// STATE (30 or 34 bytes): flags:u8, seq:u32, x:i32, y:i32, shape:u64,
// optional video_floor:u32. Flags bit 0 is visibility, bit 1 includes the floor.
// SHAPE (29 + data): shape:u64, width/height:u16, hot_x/hot_y:i16, total/index:u16.
// ACK (31 bytes): seq:u32, shape:u64, una:u16, received[una..una+63]:u64.
// The upper six bits of its type carry the assembly generation (hash retry).
constexpr std::uint8_t state_type = 1, shape_type = 2, ack_type = 3;
constexpr std::size_t chunk_size = 1100, window = 32, cache_limit = 64;
constexpr std::uint64_t heartbeat_ms = 250, expiry_ms = 2000;

inline bool valid_dimensions(unsigned width, unsigned height) {
    return width && height && width <= 256 && height <= 256;
}
inline std::size_t count(const Bitmap& bitmap) {
    return (std::size_t(bitmap.width) * bitmap.height * 4 + chunk_size - 1) / chunk_size;
}
inline bool valid(const Bitmap& bitmap) {
    return valid_dimensions(bitmap.width, bitmap.height) &&
           bitmap.pixels.size() == std::size_t(bitmap.width) * bitmap.height * 4;
}
inline void put(Bytes& bytes, std::uint64_t value, unsigned length) {
    for (unsigned i = length; i > 0; --i) bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
}
inline std::uint64_t get(const Bytes& bytes, std::size_t start, unsigned length) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < length; ++i) value = (value << 8) | bytes[start + i];
    return value;
}
inline std::int64_t signed_value(std::uint64_t value, unsigned bits) {
    const auto sign = std::uint64_t{1} << (bits - 1);
    return static_cast<std::int64_t>(value ^ sign) - static_cast<std::int64_t>(sign);
}
inline Bytes prefix(std::uint64_t token, std::uint8_t type, std::size_t size) {
    Bytes result;
    result.reserve(size);
    put(result, token, 8);
    result.push_back(type);
    return result;
}
inline bool newer(std::uint32_t seq, std::uint32_t previous) {
    const auto distance = std::uint32_t(seq - previous);
    return distance && distance < 0x80000000u;
}
inline Bytes state_packet(std::uint64_t token, std::uint32_t seq, State state, std::uint64_t id) {
    auto bytes = prefix(token, state_type, state.video_floor ? 34 : 30);
    bytes.push_back((state.visible ? 1 : 0) | (state.video_floor ? 2 : 0));
    put(bytes, seq, 4);
    put(bytes, static_cast<std::uint32_t>(state.x), 4);
    put(bytes, static_cast<std::uint32_t>(state.y), 4);
    put(bytes, id, 8);
    if (state.video_floor) put(bytes, *state.video_floor, 4);
    return bytes;
}
inline Bytes fragment_packet(std::uint64_t token, std::uint64_t id, const Bitmap& bmp, std::size_t index) {
    const auto offset = index * chunk_size;
    const auto length = std::min(chunk_size, bmp.pixels.size() - offset);
    auto bytes = prefix(token, shape_type, 29 + length);
    put(bytes, id, 8);
    put(bytes, bmp.width, 2);
    put(bytes, bmp.height, 2);
    put(bytes, static_cast<std::uint16_t>(bmp.hot_x), 2);
    put(bytes, static_cast<std::uint16_t>(bmp.hot_y), 2);
    put(bytes, count(bmp), 2);
    put(bytes, index, 2);
    bytes.insert(bytes.end(), bmp.pixels.begin() + offset, bmp.pixels.begin() + offset + length);
    return bytes;
}
} // namespace detail

// Include geometry and hotspot in the canonical content to distinguish cursor
// variants that share their BGRA bytes. Zero denotes a state with no image.
inline std::uint64_t shape_id(const Bitmap& bmp) {
    std::uint64_t hash = 14695981039346656037ull;
    auto add = [&](std::uint8_t value) { hash = (hash ^ value) * 1099511628211ull; };
    for (auto value : {bmp.width, bmp.height, static_cast<std::uint16_t>(bmp.hot_x),
                       static_cast<std::uint16_t>(bmp.hot_y)}) {
        add(static_cast<std::uint8_t>(value >> 8));
        add(static_cast<std::uint8_t>(value));
    }
    for (auto byte : bmp.pixels) add(byte);
    return hash ? hash : 1;
}

class Sender {
public:
    explicit Sender(std::uint64_t token) : token_(token) {}

    SenderStats stats() const { return stats_; }
    std::optional<std::uint64_t> last_ack() const { return last_ack_; }
    bool shape_ready() const { return image_ && complete_ && complete_seq_.has_value(); }

    template<class Emit>
    void update(std::uint64_t now, State state, const Image& image, Emit&& emit) {
        const auto previous_shape = shape_;
        if (image != image_) {
            image_ = image && detail::valid(*image) ? image : Image{};
            shape_ = image_ ? shape_id(*image_) : 0;
        }
        if (!image_) state.visible = false;
        const bool changed_shape = shape_ != previous_shape;
        const bool changed = samples_.empty() || !same(state, state_) || changed_shape;
        const bool heartbeat = !changed && acknowledged_ && now - last_send_ >= detail::heartbeat_ms;
        if (changed || heartbeat) {
            state_ = state;
            ++seq_;
            if (samples_.size() == 1024) samples_.pop_front();
            samples_.push_back({seq_, shape_, image_ ? detail::count(*image_) : 0, now, false});
            acknowledged_ = false;
            tail_ = changed;
            state_rto_ = rto_;
            if (changed_shape) {
                shape_seq_ = seq_;
                fragments_.assign(samples_.back().count, {});
                complete_ = std::find(known_.begin(), known_.end(), shape_) != known_.end();
                complete_seq_.reset();
                generation_.reset();
                transfer_started_ = false;
            }
        } else if (tail_ || (!acknowledged_ && now - last_send_ >= state_rto_)) {
            if (!tail_) state_rto_ = backoff(state_rto_);
            tail_ = false;
            samples_.back().retried = true;
            ++stats_.retransmits;
        } else {
            send_fragments(now, emit);
            return;
        }
        last_send_ = now;
        ++stats_.sent;
        emit(detail::state_packet(token_, seq_, state_, shape_));
        send_fragments(now, emit);
    }

    bool acknowledge(std::uint64_t now, const Bytes& bytes) {
        if (bytes.size() != 31 || (bytes[8] & 3) != detail::ack_type || detail::get(bytes, 0, 8) != token_) return false;
        const auto seq = static_cast<std::uint32_t>(detail::get(bytes, 9, 4));
        const auto id = detail::get(bytes, 13, 8);
        const auto una = static_cast<std::size_t>(detail::get(bytes, 21, 2));
        const auto bits = detail::get(bytes, 23, 8);
        // Only credit states actually sent on this session. Duplicates may
        // repair a missing fragment but cannot indefinitely keep the link up.
        const auto age = std::uint32_t(seq_ - seq);
        if (age >= samples_.size()) return false;
        const auto& sample = samples_[samples_.size() - 1 - age];
        if (sample.seq != seq || sample.shape != id || una > sample.count || (bits & 1)) return false;
        const auto remaining = sample.count - una;
        if (remaining < 64 && (bits >> remaining)) return false;

        bool fresh = !newest_ack_ || detail::newer(seq, *newest_ack_);
        if (fresh) {
            newest_ack_ = seq;
            if (!sample.retried) observe_rtt(now - sample.time);
        }
        if (seq == seq_) acknowledged_ = true;
        if (image_ && id == shape_ && !detail::newer(shape_seq_, seq))
            fresh = accept_fragments(seq, una, bits, bytes[8] >> 2) || fresh;
        if (fresh) last_ack_ = now;
        return fresh;
    }

private:
    struct Sample { std::uint32_t seq; std::uint64_t shape; std::size_t count; std::uint64_t time; bool retried; };
    struct Fragment {
        bool sent = false, acked = false;
        unsigned crossed = 0;
        std::uint64_t time = 0, rto = 0;
    };
    std::uint64_t token_, shape_ = 0, last_send_ = 0, rto_ = 32, state_rto_ = 32;
    std::uint32_t seq_ = 0, shape_seq_ = 0;
    std::optional<std::uint32_t> newest_ack_, complete_seq_;
    std::optional<unsigned> generation_;
    std::optional<std::uint64_t> last_ack_;
    bool acknowledged_ = false, tail_ = false, complete_ = false, transfer_started_ = false;
    State state_;
    Image image_;
    SenderStats stats_;
    double variance_ = 0;
    bool timed_ = false;
    std::deque<Sample> samples_;
    std::deque<std::uint64_t> known_;
    std::vector<Fragment> fragments_;

    static std::uint64_t backoff(std::uint64_t interval) {
        return std::min<std::uint64_t>(200, interval + (interval + 1) / 2);
    }
    void observe_rtt(std::uint64_t elapsed) {
        const double sample = static_cast<double>(elapsed);
        if (!timed_) {
            timed_ = true;
            stats_.rtt_ms = sample;
            variance_ = sample / 2;
        } else {
            variance_ += (std::abs(stats_.rtt_ms - sample) - variance_) / 4;
            stats_.rtt_ms += (sample - stats_.rtt_ms) / 8;
        }
        rto_ = static_cast<std::uint64_t>(std::clamp(std::ceil(stats_.rtt_ms + 4 * variance_), 8., 200.));
    }
    bool accept_fragments(std::uint32_t seq, std::size_t una, std::uint64_t bits, unsigned generation) {
        if (generation_ && *generation_ != generation) {
            // A failed content hash invalidates all old selective ACKs. The
            // next epoch is repeated on every ACK, including after ACK loss.
            const auto distance = (generation - *generation_) & 63u;
            if (distance >= 32) return false;
            fragments_.assign(fragments_.size(), {});
            complete_ = false;
            complete_seq_.reset();
            transfer_started_ = false;
        }
        generation_ = generation;
        if (una == fragments_.size()) {
            if (complete_ && complete_seq_) return false;
            complete_ = true;
            complete_seq_ = seq;
            auto it = std::find(known_.begin(), known_.end(), shape_);
            if (it != known_.end()) known_.erase(it);
            if (known_.size() == detail::cache_limit) known_.pop_front();
            known_.push_back(shape_);
            return true;
        }
        if (complete_) {
            // An incomplete ACK from before completion can arrive late. A
            // cache miss on a later activation must restart the transfer.
            if (complete_seq_ && !detail::newer(seq, *complete_seq_)) return false;
            fragments_.assign(fragments_.size(), {});
            complete_ = false;
            complete_seq_.reset();
            transfer_started_ = false;
            known_.erase(std::remove(known_.begin(), known_.end(), shape_), known_.end());
        }
        bool progress = false;
        unsigned newly_ahead = 0;
        for (std::size_t i = fragments_.size(); i-- > 0;) {
            auto& frag = fragments_[i];
            const bool reported = i < una || (i - una < 64 && ((bits >> (i - una)) & 1));
            if (reported) {
                if (!frag.acked) {
                    frag.acked = true;
                    ++newly_ahead;
                    progress = true;
                }
            } else if (frag.sent) {
                frag.crossed = std::min(2u, frag.crossed + newly_ahead);
            }
        }
        return progress;
    }

    template<class Emit>
    void send_fragments(std::uint64_t now, Emit& emit) {
        if (!image_ || complete_) return;
        auto hole = std::find_if(fragments_.begin(), fragments_.end(), [](const Fragment& f) { return !f.acked; });
        auto first = static_cast<std::size_t>(hole - fragments_.begin());
        const auto end = std::min(fragments_.size(), first + detail::window);
        for (std::size_t i = first; i < end; ++i) {
            auto& frag = fragments_[i];
            if (frag.acked) continue;
            if (frag.sent) {
                const bool timeout = now - frag.time >= frag.rto;
                if (!timeout && frag.crossed < 2) continue;
                if (timeout) frag.rto = backoff(frag.rto);
                ++stats_.retransmits;
            } else {
                frag.sent = true;
                frag.rto = rto_;
            }
            if (!transfer_started_) {
                transfer_started_ = true;
                ++stats_.shape_transfers;
            }
            frag.time = now;
            frag.crossed = 0;
            ++stats_.sent;
            emit(detail::fragment_packet(token_, shape_, *image_, i));
        }
    }
};

class Receiver {
public:
    explicit Receiver(std::uint64_t token) : token_(token) {}

    ReceiverStats stats() const { return stats_; }
    View view() const { return view_; }
    std::uint64_t revision() const { return revision_; }
    void expire(std::uint64_t now) {
        if (last_rx_ && now - *last_rx_ >= detail::expiry_ms) {
            expired_ = true;
            publish();
        }
    }

    std::optional<Bytes> receive(std::uint64_t now, const Bytes& bytes) {
        if (bytes.size() < 9 || detail::get(bytes, 0, 8) != token_) return {};
        if (bytes[8] == detail::state_type) {
            if (bytes.size() < 30 || bytes[9] > 3) return {};
            const bool visible = (bytes[9] & 1) != 0;
            const bool has_floor = (bytes[9] & 2) != 0;
            if (bytes.size() != (has_floor ? 34u : 30u)) return {};
            const auto id = detail::get(bytes, 22, 8);
            if (visible && !id) return {};
            const auto seq = static_cast<std::uint32_t>(detail::get(bytes, 10, 4));
            if (!seq_ || detail::newer(seq, *seq_)) {
                State next{visible, static_cast<std::int32_t>(detail::signed_value(detail::get(bytes, 14, 4), 32)),
                                    static_cast<std::int32_t>(detail::signed_value(detail::get(bytes, 18, 4), 32))};
                if (has_floor) next.video_floor = static_cast<std::uint32_t>(detail::get(bytes, 30, 4));
                if (seq_) stats_.gaps += std::uint32_t(seq - *seq_ - 1);
                if (!seq_ || !same(next, state_) || id != shape_) ++stats_.updates;
                state_ = next;
                seq_ = seq;
                expired_ = false;
                last_rx_ = now;
                if (shape_ != id) select(id);
                publish();
            } else if (*seq_ == seq) {
                last_rx_ = now;
            }
        } else if (bytes[8] == detail::shape_type) {
            if (!receive_fragment(bytes)) return {};
            if (seq_ && detail::get(bytes, 9, 8) == shape_) last_rx_ = now;
        } else {
            return {};
        }
        return ack();
    }

private:
    struct Cached { std::uint64_t id; Image image; };
    std::uint64_t token_, shape_ = 0, revision_ = 0;
    std::optional<std::uint32_t> seq_;
    std::optional<std::uint64_t> last_rx_;
    State state_;
    View view_;
    bool expired_ = false;
    ReceiverStats stats_;
    std::deque<Cached> cache_;
    Image current_;
    std::shared_ptr<Bitmap> partial_;
    std::vector<bool> received_;
    std::size_t una_ = 0;
    unsigned generation_ = 0;

    void select(std::uint64_t id) {
        shape_ = id;
        generation_ = 0;
        partial_.reset();
        current_.reset();
        received_.clear();
        una_ = 0;
        auto it = std::find_if(cache_.begin(), cache_.end(), [id](const Cached& c) { return c.id == id; });
        if (it != cache_.end()) {
            current_ = it->image;
            cache_.erase(it);
            cache_.push_back({id, current_});
        }
    }
    void publish() {
        View next{};
        if (!expired_ && state_.visible) {
            // Keep drawing the previous image at the new position until all
            // parts of a changed shape are ready, to avoid a visible blink.
            next.image = current_ ? current_ : view_.image;
            if (next.image) next.state = state_;
        }
        if (!same(next.state, view_.state) || next.image != view_.image) {
            view_ = std::move(next);
            ++revision_;
        }
    }
    bool receive_fragment(const Bytes& bytes) {
        if (bytes.size() <= 29 || bytes.size() > 29 + detail::chunk_size) return false;
        const auto id = detail::get(bytes, 9, 8);
        const auto width = static_cast<std::uint16_t>(detail::get(bytes, 17, 2));
        const auto height = static_cast<std::uint16_t>(detail::get(bytes, 19, 2));
        const auto hx = static_cast<std::int16_t>(detail::signed_value(detail::get(bytes, 21, 2), 16));
        const auto hy = static_cast<std::int16_t>(detail::signed_value(detail::get(bytes, 23, 2), 16));
        const auto total = static_cast<std::size_t>(detail::get(bytes, 25, 2));
        const auto idx = static_cast<std::size_t>(detail::get(bytes, 27, 2));
        if (!id || !detail::valid_dimensions(width, height)) return false;
        const auto size = std::size_t(width) * height * 4;
        if (total != (size + detail::chunk_size - 1) / detail::chunk_size || idx >= total) return false;
        const auto offset = idx * detail::chunk_size;
        if (bytes.size() != 29 + std::min(detail::chunk_size, size - offset)) return false;
        if (!seq_ || id != shape_ || current_) return true;
        if (!partial_) {
            partial_ = std::make_shared<Bitmap>(Bitmap{width, height, hx, hy, Bytes(size)});
            received_.assign(total, false);
        } else if (partial_->width != width || partial_->height != height ||
                   partial_->hot_x != hx || partial_->hot_y != hy) {
            return false;
        }
        if (!received_[idx]) {
            std::copy(bytes.begin() + 29, bytes.end(), partial_->pixels.begin() + offset);
            received_[idx] = true;
            while (una_ < total && received_[una_]) ++una_;
            if (una_ == total) {
                if (shape_id(*partial_) == id) {
                    current_ = partial_;
                    if (cache_.size() == detail::cache_limit) cache_.pop_front();
                    cache_.push_back({id, current_});
                    publish();
                } else {
                    generation_ = (generation_ + 1) & 63u;
                }
                partial_.reset();
                received_.clear();
                una_ = 0;
            }
        }
        return true;
    }
    Bytes ack() const {
        auto bytes = detail::prefix(token_, static_cast<std::uint8_t>((generation_ << 2) | detail::ack_type), 31);
        detail::put(bytes, seq_.value_or(0), 4);
        detail::put(bytes, shape_, 8);
        detail::put(bytes, current_ ? detail::count(*current_) : una_, 2);
        std::uint64_t mask = 0;
        for (std::size_t i = una_; i < std::min(received_.size(), una_ + 64); ++i)
            if (received_[i]) mask |= std::uint64_t{1} << (i - una_);
        detail::put(bytes, mask, 8);
        return bytes;
    }
};

} // namespace cursor
