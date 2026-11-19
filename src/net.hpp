#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace net {

#ifdef _WIN32
using NativeSocket = std::uintptr_t;
#else
using NativeSocket = int;
#endif

struct Endpoint {
    std::array<std::uint8_t, 16> address{};
    std::uint32_t scope = 0;
    std::uint16_t port = 0; // Host byte order.
    bool v6 = false;

    static Endpoint ipv4(std::uint32_t network_order_host, std::uint16_t port);
    bool same_address(const Endpoint& other) const {
        return v6 == other.v6 && address == other.address && scope == other.scope;
    }
    bool operator==(const Endpoint& other) const { return same_address(other) && port == other.port; }
    bool operator!=(const Endpoint& other) const { return !(*this == other); }
};

inline constexpr std::size_t max_datagram_size = 1200;

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(NativeSocket handle) noexcept : handle_(handle) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    explicit operator bool() const noexcept { return handle_ != invalid; }

    // Safe alongside reads/writes. Keep the Socket alive until those calls return.
    void shutdown() const noexcept;

    // Returns false on EOF before the first byte. A partial read at EOF throws.
    // Before each read, poll_interval can service pending video and choose how
    // long to wait if the next bytes haven't arrived yet.
    bool read_exact(void* destination, std::size_t size,
                    const std::function<unsigned()>& poll_interval = {}) const;
    // Returns zero if no data is ready. EOF and connection failures throw.
    std::size_t try_read(void* destination, std::size_t size) const;
    void write_all(const void* source, std::size_t size) const;
    // Returns the number committed, or zero when the deadline expires first.
    std::size_t write_some_until(const void* source, std::size_t size,
                                 std::chrono::steady_clock::time_point deadline) const;

    NativeSocket native_handle() const noexcept { return handle_; }
    std::uint16_t local_port() const;
    Endpoint peer_endpoint() const;

    // Nonblocking UDP. False means no datagram was sent or delivered to the
    // caller; transient route errors and oversized packets are treated as loss.
    bool send_to(const Endpoint& peer, const std::vector<std::uint8_t>& bytes) const;
    bool receive_from(Endpoint& source, std::vector<std::uint8_t>& bytes) const;

private:
    static constexpr auto invalid = static_cast<NativeSocket>(-1);
    NativeSocket handle_ = invalid;
    mutable std::atomic<bool> stopped_{false};
};

Socket connect_tcp(const std::string& host, std::uint16_t port);
Socket bind_udp(const std::string& bind_address, std::uint16_t port = 0);
Socket listen_tcp(const std::string& bind_address, std::uint16_t port);
// After wait_readable(), a connection can still be withdrawn. Returns an
// empty socket in that case so the caller can resume its event loop.
Socket accept_tcp(Socket& listener);
bool wait_readable(Socket& socket, int timeout_ms);

} // namespace net
