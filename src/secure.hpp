#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace secure {

using Bytes = std::vector<std::uint8_t>;
enum class Role : std::uint8_t { sender = 1, receiver = 2 };
enum class Kind : std::uint8_t { confirm = 1, display_query, display_reply, header, frame, feedback };
inline constexpr std::size_t hello_size = 40;
inline constexpr std::size_t record_overhead = 25;

Bytes make_hello(Role role);
void validate_hello(const Bytes& hello, Role expected);

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace secure
