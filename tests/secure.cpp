#include "secure.hpp"
#include "wire.hpp"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
using namespace secure;

void require(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function&& function, const char* message) {
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

Bytes hex(const std::string& text) {
    Bytes result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return result;
}

void test_session() {
    const std::string password = "MaxDisplay test password";
    auto client = make_hello(Role::sender);
    auto server = make_hello(Role::receiver);
    require(client != make_hello(Role::sender) && server != make_hello(Role::receiver), "Repeated random challenge");
    validate_hello(client, Role::sender);
    rejects([&] { validate_hello(client, Role::receiver); }, "Accepted a reflected hello");
    rejects([&] { validate_hello({}, Role::sender); }, "Accepted a short hello");
    auto malformed = client;
    malformed[4] ^= 1;
    rejects([&] { validate_hello(malformed, Role::sender); }, "Accepted unknown crypto version");

    // Reference vectors derived independently with Python hashlib.pbkdf2_hmac,
    // hmac.digest and cryptography AESGCM. Challenge octets are 00..1f and
    // 20..3f. The frame input includes both a NUL and a non-ASCII octet.
    std::iota(client.begin() + 8, client.end(), 0);
    std::iota(server.begin() + 8, server.end(), 32);
    rejects([&] { Session session("", client, server, Role::sender); }, "Accepted empty encryption password");
    Session tx(password, client, server, Role::sender), rx(password, client, server, Role::receiver);
    const auto server_proof = rx.seal(Kind::confirm, {});
    const auto client_proof = tx.seal(Kind::confirm, {});
    require(server_proof == hex("01000000000000000147bfba64960fb489209dda88bfebd752"), "Incorrect receiver proof");
    require(client_proof == hex("010000000000000001289ff1b4080c9e7e6ee86e71f1a7a846"), "Incorrect sender proof");
    rejects([&] { tx.open(Kind::confirm, client_proof); }, "Accepted a reflected sender proof");
    require(tx.open(Kind::confirm, server_proof).empty() && rx.open(Kind::confirm, client_proof).empty(),
            "Mutual confirmation failed");

    const Bytes pixels{'P','i','x','e','l','s',0,255};
    const auto frame = tx.seal(Kind::frame, pixels);
    require(frame == hex("0500000000000000028769f74d2abe2284c5cf67e56d7dafe03216ce4f667bbeb6"),
            "Incorrect sender AES-GCM test vector");
    rejects([&] { rx.open(Kind::header, frame); }, "Accepted frame as stream header");
    for (const std::size_t offset : {std::size_t(0), std::size_t(8), std::size_t(9), frame.size() - 1}) {
        auto damaged = frame;
        damaged[offset] ^= 0x01;
        const auto kind = static_cast<Kind>(damaged[0]);
        rejects([&] { rx.open(kind, damaged); }, "Accepted changed type, counter, ciphertext or tag");
    }
    require(rx.open(Kind::frame, frame) == pixels, "Unauthenticated record advanced receiver state");
    rejects([&] { rx.open(Kind::frame, frame); }, "Accepted a replayed frame");
    const auto feedback = rx.seal(Kind::feedback, {'a','c','k'});
    require(feedback == hex("060000000000000002fbe143dec080e1141e85175ed5af0c4b4c5db6"), "Incorrect feedback vector");
    require(tx.open(Kind::feedback, feedback) == Bytes({'a','c','k'}), "Failed to decrypt feedback");

    for (int i = 3; i <= 4096; ++i) tx.seal(Kind::frame, pixels);
    const auto new_epoch = tx.seal(Kind::frame, pixels);
    require(new_epoch == hex("050000000000001001e80c3cf43c30ce10a489c8cd1fbc8b442649478167f0e99e"),
            "Incorrect rotated AES-GCM key");
    require(rx.open(Kind::frame, new_epoch) == pixels, "Could not skip lost records across a key rotation");

    Session wrong_password("other password", client, server, Role::sender);
    rejects([&] { wrong_password.open(Kind::confirm, server_proof); }, "A different password accepted the proof");
    server.back() ^= 0xff;
    Session fresh(password, client, server, Role::receiver);
    rejects([&] { fresh.open(Kind::frame, frame); }, "Accepted an old session's frame after reconnect");
    client.back() ^= 0xff;
    server.back() ^= 0xff;
    Session other_sender(password, client, server, Role::receiver);
    rejects([&] { other_sender.open(Kind::confirm, client_proof); }, "Client challenge was not bound to session");

    const Bytes largest(wire::max_frame_bytes, 0x56);
    require(rx.open(Kind::frame, tx.seal(Kind::frame, largest)) == largest, "Rejected maximum frame");
    const Bytes oversized(wire::max_frame_bytes + 1, 0x42);
    rejects([&] { tx.seal(Kind::frame, oversized); }, "Accepted oversized frame");
    rejects([&] { tx.seal(Kind::confirm, pixels); }, "Accepted nonempty proof");
    rejects([&] { rx.open(Kind::frame, {}); }, "Accepted a truncated record");
}
} // namespace

int main() {
    try {
        test_session();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
