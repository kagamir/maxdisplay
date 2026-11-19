#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace udp {

// Each message is one reliable, ordered ENet packet. ENet fragments messages
// into MTU-sized UDP datagrams; caller-supplied message boundaries survive.
class Sender {
public:
    Sender(const std::string& host, std::uint16_t port);
    ~Sender();
    void send(const std::vector<std::uint8_t>& message);
    bool peer_closed() const;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class ReadResult { message, timeout, disconnected };

// Service accept/receive on the same thread. Bounded polls let the caller
// stop the receiver without closing an ENet host from another thread.
class Listener {
public:
    Listener(const std::string& bind_address, std::uint16_t port);
    ~Listener();
    std::uint16_t local_port() const;
    bool accept(unsigned timeout_ms);
    ReadResult receive(std::vector<std::uint8_t>& message, unsigned timeout_ms);
    void disconnect() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace udp
