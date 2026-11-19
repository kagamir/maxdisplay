#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace secure {

using Bytes = std::vector<std::uint8_t>;
enum class Role : std::uint8_t { sender = 1, receiver = 2 };
enum class Kind : std::uint8_t { confirm = 1, display_query, display_reply, header, frame, feedback, cursor_query, cursor_reply };
inline constexpr std::size_t hello_size = 40;
inline constexpr std::size_t record_overhead = 25;
inline constexpr std::size_t datagram_overhead = 24;

std::uint64_t random_token();
Bytes make_hello(Role role);
void validate_hello(const Bytes& hello, Role expected);

// Independently keyed, unordered cursor traffic. The leading 8 token bytes
// stay in the clear and are authenticated along with the 64-bit counter.
class Datagrams {
public:
    ~Datagrams();
    Bytes seal(const Bytes& packet);
    // Returns the entire original packet, or nullopt for invalid/replayed input.
    std::optional<Bytes> open(const Bytes& record);

private:
    friend class Session;
    struct Impl;
    explicit Datagrams(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// A fresh hello from each endpoint is required on every connection. The two
// directions have separate state; callers serialize operations in each direction.
class Session {
public:
    Session(const std::string& password, const Bytes& sender_hello, const Bytes& receiver_hello, Role local);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Bytes seal(Kind kind, const Bytes& plaintext);
    Bytes open(Kind expected, const Bytes& record);
    // One independent cursor channel per authenticated session. Transfers
    // ownership to the side thread, which can outlive the video connection.
    std::unique_ptr<Datagrams> datagrams();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace secure
