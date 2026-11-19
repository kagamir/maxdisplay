#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

#ifdef _WIN32
using NativeSocket = std::uintptr_t;
#else
using NativeSocket = int;
#endif

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
    // Sender-side probe for the one-way protocol. The peer never sends data.
    bool peer_closed() const;

    // Returns false on EOF before the first byte. A partial read at EOF throws.
    bool read_exact(void* destination, std::size_t size) const;
    void write_all(const void* source, std::size_t size) const;

    NativeSocket native_handle() const noexcept { return handle_; }
    std::uint16_t local_port() const;

private:
    static constexpr auto invalid = static_cast<NativeSocket>(-1);
    NativeSocket handle_ = invalid;
    mutable std::atomic<bool> stopped_{false};
};

Socket connect_tcp(const std::string& host, std::uint16_t port);
Socket listen_tcp(const std::string& bind_address, std::uint16_t port);
// After wait_readable(), a connection can still be withdrawn. Returns an
// empty socket in that case so the caller can resume its event loop.
Socket accept_tcp(Socket& listener);
bool wait_readable(Socket& socket, int timeout_ms);

} // namespace net
