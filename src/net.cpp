#include "net.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace net {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto io_timeout = std::chrono::seconds(5);
constexpr auto invalid = static_cast<NativeSocket>(-1);

#ifdef _WIN32
using Length = int;
int last_error() { return WSAGetLastError(); }
bool interrupted(int error) { return error == WSAEINTR; }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool connecting(int error) { return would_block(error) || error == WSAEINPROGRESS; }
bool withdrawn(int error) { return error == WSAECONNRESET || error == WSAECONNABORTED; }
void close_socket(NativeSocket handle) { closesocket(handle); }
constexpr int shutdown_both = SD_BOTH;
#else
using Length = socklen_t;
int last_error() { return errno; }
bool interrupted(int error) { return error == EINTR; }
bool would_block(int error) { return error == EWOULDBLOCK || error == EAGAIN; }
bool connecting(int error) { return error == EINPROGRESS || interrupted(error); }
bool withdrawn(int error) { return error == ECONNRESET || error == ECONNABORTED; }
void close_socket(NativeSocket handle) { ::close(handle); }
constexpr int shutdown_both = SHUT_RDWR;
#endif

std::system_error socket_error(const char* message, int error = last_error()) {
    return std::system_error(error, std::system_category(), message);
}

void start_network() {
#ifdef _WIN32
    struct Winsock {
        Winsock() {
            WSADATA data{};
            const int error = WSAStartup(MAKEWORD(2, 2), &data);
            if (error != 0) throw socket_error("WSAStartup", error);
        }
        ~Winsock() { WSACleanup(); }
    };
    static Winsock winsock;
#endif
}

void make_nonblocking(NativeSocket handle) {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(handle, FIONBIO, &enabled) != 0) throw socket_error("ioctlsocket");
#else
    const int flags = fcntl(handle, F_GETFL);
    if (flags == -1 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) == -1)
        throw socket_error("fcntl(O_NONBLOCK)");
#endif
}

int remaining_ms(Clock::time_point deadline) {
    auto duration = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    return static_cast<int>(std::clamp<std::int64_t>(duration.count(), 0, INT_MAX));
}

// Poll on Unix to support descriptors above FD_SETSIZE. Winsock's select uses
// SOCKET values and reports nonblocking connect failures in its exception set.
bool wait_ready(NativeSocket handle, bool writing, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const int remaining = remaining_ms(deadline);
#ifdef _WIN32
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(handle, &ready);
        fd_set errors = ready;
        timeval timeout{remaining / 1000, (remaining % 1000) * 1000};
        const int result = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr,
                                  writing ? &errors : nullptr, &timeout);
#else
        pollfd fd{handle, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int result = poll(&fd, 1, remaining);
#endif
        if (result >= 0) return result != 0;
        const int error = last_error();
        if (!interrupted(error)) throw socket_error("Waiting for a socket", error);
        if (Clock::now() >= deadline) return false;
    }
}

void set_flag(NativeSocket handle, int level, int option, const char* name) {
    const int enabled = 1;
    if (setsockopt(handle, level, option, reinterpret_cast<const char*>(&enabled), sizeof(enabled)))
        throw socket_error(name);
}

void configure_stream(NativeSocket handle) {
    set_flag(handle, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY");
#ifdef SO_NOSIGPIPE
    set_flag(handle, SOL_SOCKET, SO_NOSIGPIPE, "SO_NOSIGPIPE");
#endif
}

using Addresses = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;

Addresses resolve(const std::string& host, std::uint16_t port, bool passive) {
    start_network();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    addrinfo* raw = nullptr;
    const int error = getaddrinfo(passive && host.empty() ? nullptr : host.c_str(),
                                  std::to_string(port).c_str(), &hints, &raw);
    if (error != 0) {
#ifdef _WIN32
        throw std::system_error(error, std::system_category(), "Resolving " + host);
#else
        throw std::runtime_error("Resolving " + host + ": " + gai_strerror(error));
#endif
    }
    return Addresses(raw, &freeaddrinfo);
}

Socket open_socket(const addrinfo& address) {
    const auto handle = ::socket(address.ai_family, address.ai_socktype, address.ai_protocol);
    if (handle == invalid) throw socket_error("socket");
    Socket socket(handle);
    make_nonblocking(handle);
    return socket;
}

void complete_connect(NativeSocket handle) {
    const int error = last_error();
    if (!connecting(error)) throw socket_error("connect", error);
    if (!wait_ready(handle, true, static_cast<int>(std::chrono::milliseconds(io_timeout).count())))
        throw std::runtime_error("Connection timed out");
    int status = 0;
    Length size = sizeof(status);
    if (getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &size))
        throw socket_error("getsockopt(SO_ERROR)");
    if (status != 0) throw socket_error("connect", status);
}

} // namespace

Socket::~Socket() {
    if (handle_ != invalid) close_socket(handle_);
}

Socket::Socket(Socket&& other) noexcept
    : handle_(std::exchange(other.handle_, invalid)), stopped_(other.stopped_.load()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (handle_ != invalid) close_socket(handle_);
        handle_ = std::exchange(other.handle_, invalid);
        stopped_.store(other.stopped_.load());
    }
    return *this;
}

void Socket::shutdown() const noexcept {
    if (handle_ != invalid && !stopped_.exchange(true))
        ::shutdown(handle_, shutdown_both);
}

bool Socket::peer_closed() const {
    if (handle_ == invalid || stopped_.load()) return true;
    char byte;
    const auto result = recv(handle_, &byte, 1, MSG_PEEK);
    if (result >= 0) return result == 0;
    const int error = last_error();
    return !would_block(error) && !interrupted(error);
}

std::size_t Socket::try_read(void* destination, std::size_t size) const {
    if (!size) return 0;
    const int chunk = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
    for (;;) {
        if (stopped_.load()) throw std::runtime_error("Socket was shut down");
        const auto count = recv(handle_, static_cast<char*>(destination), chunk, 0);
        if (count > 0) return static_cast<std::size_t>(count);
        if (count == 0) throw std::runtime_error("Peer disconnected");
        const int error = last_error();
        if (would_block(error)) return 0;
        if (!interrupted(error)) throw socket_error("recv", error);
    }
}

std::uint16_t Socket::local_port() const {
    sockaddr_storage address{};
    Length size = sizeof(address);
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        throw socket_error("getsockname");
    if (address.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
}

bool Socket::read_exact(void* destination, std::size_t size, const std::function<unsigned()>& poll_interval) const {
    auto* bytes = static_cast<char*>(destination);
    std::size_t received = 0;
    while (received < size) {
        if (stopped_.load()) throw std::runtime_error("Socket was shut down");
        const auto timeout = poll_interval ? std::min<unsigned>(poll_interval(), INT_MAX) : 250u;
        const int chunk = static_cast<int>(std::min<std::size_t>(size - received, INT_MAX));
        const auto count = recv(handle_, bytes + received, chunk, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count == 0) {
            if (received == 0) return false;
            throw std::runtime_error("Peer disconnected in the middle of a message");
        } else {
            const int error = last_error();
            if (would_block(error)) wait_ready(handle_, false, static_cast<int>(timeout));
            else if (!interrupted(error)) throw socket_error("recv", error);
        }
    }
    return true;
}

std::size_t Socket::write_some_until(const void* source, std::size_t size, Clock::time_point deadline) const {
    if (!size) return 0;
    const int chunk = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    for (;;) {
        if (stopped_.load()) throw std::runtime_error("Socket was shut down");
        if (Clock::now() >= deadline) return 0;
        const int remaining = remaining_ms(deadline);
        if (!remaining) return 0;
        const auto count = send(handle_, static_cast<const char*>(source), chunk, flags);
        if (count > 0) return static_cast<std::size_t>(count);
        if (count == 0) throw std::runtime_error("Peer disconnected during send");
        const int error = last_error();
        if (would_block(error)) wait_ready(handle_, true, remaining);
        else if (!interrupted(error)) throw socket_error("send", error);
    }
}

void Socket::write_all(const void* source, std::size_t size) const {
    const auto* bytes = static_cast<const char*>(source);
    const auto deadline = Clock::now() + io_timeout;
    for (std::size_t sent = 0; sent < size;) {
        const auto count = write_some_until(bytes + sent, size - sent, deadline);
        if (!count) throw std::runtime_error("Sending video timed out");
        sent += count;
    }
}

Socket connect_tcp(const std::string& host, std::uint16_t port) {
    const auto addresses = resolve(host, port, false);
    std::string reason = "No matching address";
    for (auto* address = addresses.get(); address; address = address->ai_next) {
        try {
            auto socket = open_socket(*address);
            const auto handle = socket.native_handle();
            if (connect(handle, address->ai_addr, static_cast<Length>(address->ai_addrlen)) != 0)
                complete_connect(handle);
            configure_stream(handle);
            return socket;
        } catch (const std::runtime_error& e) {
            reason = e.what();
        }
    }
    throw std::runtime_error("Connecting to " + host + ":" + std::to_string(port) + ": " + reason);
}

Socket listen_tcp(const std::string& bind_address, std::uint16_t port) {
    const auto addresses = resolve(bind_address, port, true);
    std::string reason = "No matching address";
    for (auto* address = addresses.get(); address; address = address->ai_next) {
        try {
            auto socket = open_socket(*address);
            const auto handle = socket.native_handle();
#ifdef _WIN32
            set_flag(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, "SO_EXCLUSIVEADDRUSE");
#else
            set_flag(handle, SOL_SOCKET, SO_REUSEADDR, "SO_REUSEADDR");
#endif
            if (bind(handle, address->ai_addr, static_cast<Length>(address->ai_addrlen)) != 0)
                throw socket_error("bind");
            if (listen(handle, 1) != 0) throw socket_error("listen");
            return socket;
        } catch (const std::runtime_error& e) {
            reason = e.what();
        }
    }
    throw std::runtime_error("Listening on " + bind_address + ":" + std::to_string(port) + ": " + reason);
}

Socket accept_tcp(Socket& listener) {
    for (;;) {
        const auto handle = accept(listener.native_handle(), nullptr, nullptr);
        if (handle != invalid) {
            Socket socket(handle);
            make_nonblocking(handle);
            configure_stream(handle);
            return socket;
        }
        const int error = last_error();
        if (would_block(error) || withdrawn(error)) return {};
        if (!interrupted(error)) throw socket_error("accept", error);
    }
}

bool wait_readable(Socket& socket, int timeout_ms) {
    return wait_ready(socket.native_handle(), false, timeout_ms);
}

} // namespace net
