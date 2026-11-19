#include "net.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}

void exchange(const std::string& loopback, net::Socket listener) {
    auto video_sender = net::connect_tcp(loopback, listener.local_port());
    require(net::wait_readable(listener, 1000), "Missing TCP connection");
    auto video_receiver = net::accept_tcp(listener);
    auto receiver_endpoint = video_sender.peer_endpoint();
    auto sender_endpoint = video_receiver.peer_endpoint();
    require(receiver_endpoint.port == listener.local_port() && sender_endpoint.port == video_sender.local_port(),
            "Incorrect video peer endpoint");
    auto sender = net::bind_udp(loopback);
    auto receiver = net::bind_udp(loopback);
    sender_endpoint.port = sender.local_port();
    receiver_endpoint.port = receiver.local_port();
    require(sender_endpoint.port && receiver_endpoint.port && sender_endpoint != receiver_endpoint,
            "Side sockets were not assigned separate ephemeral ports");

    std::vector<std::uint8_t> bytes;
    net::Endpoint source;
    require(!sender.receive_from(source, bytes), "Unexpected datagram on idle socket");
    require(receiver.send_to(sender_endpoint, {}) && net::wait_readable(sender, 1000) &&
            sender.receive_from(source, bytes) && source == receiver_endpoint && bytes.empty(),
            "UDP firewall punch lost its source or datagram boundary");
    std::vector<std::uint8_t> full(net::max_datagram_size, 0xa5);
    require(sender.send_to(source, full) && net::wait_readable(receiver, 1000) &&
            receiver.receive_from(source, bytes) && bytes == full && source == sender_endpoint,
            "Maximum-size UDP return datagram failed");
    full.push_back(0);
    require(!sender.send_to(receiver_endpoint, full), "Sent oversized UDP datagram");
}
}

int main() {
    try {
        exchange("127.0.0.1", net::listen_tcp("127.0.0.1", 0));
        net::Socket ipv6;
        try {
            ipv6 = net::listen_tcp("::1", 0);
        } catch (const std::runtime_error& error) {
            // Some runners have IPv6 disabled. Probe only the TCP loopback;
            // once available, all UDP assertions for that family must pass.
            std::cerr << "IPv6 loopback unavailable: " << error.what() << '\n';
        }
        if (ipv6) exchange("::1", std::move(ipv6));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
