#include "secure.hpp"
#include "wire.hpp"

#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/platform_util.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>
#include <limits>
#include <stdexcept>

namespace secure {
namespace {

constexpr std::size_t header_size = 9, tag_size = 16;
static_assert(record_overhead == header_size + tag_size);
constexpr unsigned kdf_iterations = 600'000;
// Even at the maximum frame size, each directional key encrypts at most 64 GiB.
constexpr std::uint64_t records_per_key = 4096;

std::array<std::uint8_t, 8> hello_prefix(Role role) {
    return {'M', 'X', 'E', 'C', 1, static_cast<std::uint8_t>(role), 0, 0};
}

Role opposite(Role role) { return role == Role::sender ? Role::receiver : Role::sender; }

void store_u64(std::uint8_t* out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

std::uint64_t load_u64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | in[i];
    return value;
}

std::array<std::uint8_t, 12> record_iv(std::uint64_t sequence) {
    std::array<std::uint8_t, 12> iv{};
    store_u64(iv.data() + 4, sequence);
    return iv;
}

struct Secret {
    std::array<std::uint8_t, 32> bytes{};
    ~Secret() { mbedtls_platform_zeroize(bytes.data(), bytes.size()); }
};

struct Direction {
    mbedtls_gcm_context gcm;
    std::uint64_t sequence = 0, epoch = std::numeric_limits<std::uint64_t>::max();

    Direction() { mbedtls_gcm_init(&gcm); }
    ~Direction() { mbedtls_gcm_free(&gcm); }

    void select_key(const Secret& master, Role source, std::uint64_t next_sequence, std::uint8_t channel) {
        const auto next_epoch = (next_sequence - 1) / records_per_key;
        if (epoch == next_epoch) return;
        // HKDF-Expand with one SHA-256 block. The public label binds each
        // AES-128 subkey to its sending endpoint, channel and key epoch.
        std::array<std::uint8_t, 17> info{'M', 'X', 'D', 'K', 1, static_cast<std::uint8_t>(source), channel};
        store_u64(info.data() + 8, next_epoch);
        info.back() = 1;
        Secret expanded;
        const auto* digest = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        if (!digest || mbedtls_md_hmac(digest, master.bytes.data(), master.bytes.size(), info.data(), info.size(),
                                      expanded.bytes.data()) != 0 ||
            mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, expanded.bytes.data(), 128) != 0)
            throw std::runtime_error("Could not initialize the AES-128-GCM record key");
        epoch = next_epoch;
    }
};

void validate_payload(Kind kind, std::size_t size) {
    if (size > wire::max_frame_bytes || (kind == Kind::confirm ? size != 0 : size == 0))
        throw std::runtime_error("Invalid encrypted record length");
}

void random_bytes(std::uint8_t* bytes, std::size_t size) {
    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);
    const auto status = mbedtls_entropy_func(&entropy, bytes, size);
    mbedtls_entropy_free(&entropy);
    if (status != 0) throw std::runtime_error("Could not generate secure random bytes");
}

} // namespace

std::uint64_t random_token() {
    std::array<std::uint8_t, 8> bytes;
    std::uint64_t token;
    do {
        random_bytes(bytes.data(), bytes.size());
        token = load_u64(bytes.data());
    } while (!token);
    return token;
}

Bytes make_hello(Role role) {
    Bytes hello(hello_size);
    const auto prefix = hello_prefix(role);
    std::copy(prefix.begin(), prefix.end(), hello.begin());
    random_bytes(hello.data() + prefix.size(), hello.size() - prefix.size());
    return hello;
}

void validate_hello(const Bytes& hello, Role expected) {
    const auto prefix = hello_prefix(expected);
    if (hello.size() != hello_size || !std::equal(prefix.begin(), prefix.end(), hello.begin()))
        throw std::runtime_error("Expected an encrypted MaxDisplay handshake; check --password on both endpoints");
}

struct Session::Impl {
    explicit Impl(Role role) : local(role) {}
    Role local;
    Secret master;
    Direction tx, rx;
    std::atomic<bool> cursor_issued{false};
};

Session::Session(const std::string& password, const Bytes& sender_hello, const Bytes& receiver_hello, Role local)
    : impl_(std::make_unique<Impl>(local)) {
    if (password.empty()) throw std::invalid_argument("An encrypted session requires a nonempty password");
    validate_hello(sender_hello, Role::sender);
    validate_hello(receiver_hello, Role::receiver);
    std::array<std::uint8_t, 2 * hello_size> salt;
    std::copy(sender_hello.begin(), sender_hello.end(), salt.begin());
    std::copy(receiver_hello.begin(), receiver_hello.end(), salt.begin() + hello_size);
    if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.data()),
                                      password.size(), salt.data(), salt.size(), kdf_iterations,
                                      impl_->master.bytes.size(), impl_->master.bytes.data()) != 0)
        throw std::runtime_error("Could not derive the MaxDisplay session key");
}

Session::~Session() = default;

Bytes Session::seal(Kind kind, const Bytes& plaintext) {
    validate_payload(kind, plaintext.size());
    auto& tx = impl_->tx;
    if (tx.sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Encrypted session record limit reached; reconnect");
    Bytes record(plaintext.size() + record_overhead);
    // Reserve the sequence before encryption. A discarded or failed write must
    // never cause the next record to reuse the nonce.
    const auto sequence = ++tx.sequence;
    record[0] = static_cast<std::uint8_t>(kind);
    store_u64(record.data() + 1, sequence);
    const auto iv = record_iv(sequence);
    tx.select_key(impl_->master, impl_->local, sequence, 0);
    if (mbedtls_gcm_crypt_and_tag(&tx.gcm, MBEDTLS_GCM_ENCRYPT, plaintext.size(), iv.data(), iv.size(),
                                  record.data(), header_size, plaintext.data(), record.data() + header_size,
                                  tag_size, record.data() + header_size + plaintext.size()) != 0)
        throw std::runtime_error("Could not encrypt MaxDisplay record");
    return record;
}

Bytes Session::open(Kind expected, const Bytes& record) {
    if (record.size() < record_overhead || record[0] != static_cast<std::uint8_t>(expected))
        throw std::runtime_error("Unexpected encrypted record type or length");
    const auto length = record.size() - record_overhead;
    validate_payload(expected, length);
    auto& rx = impl_->rx;
    const auto sequence = load_u64(record.data() + 1);
    if (sequence <= rx.sequence) throw std::runtime_error("Repeated or out-of-order encrypted record");
    rx.select_key(impl_->master, opposite(impl_->local), sequence, 0);
    Bytes plaintext(length);
    const auto iv = record_iv(sequence);
    if (mbedtls_gcm_auth_decrypt(&rx.gcm, length, iv.data(), iv.size(), record.data(), header_size,
                                 record.data() + header_size + length, tag_size, record.data() + header_size,
                                 plaintext.data()) != 0)
        throw std::runtime_error("Encrypted record authentication failed; check password and connection");
    rx.sequence = sequence;
    return plaintext;
}

struct Datagrams::Impl {
    Impl(const Secret& key, Role source) : master(key), local(source) {}
    Secret master;
    Role local;
    Direction tx, rx;
    // Bit n records receipt of (rx.sequence - n). Only authenticated packets
    // advance this window, including when a key epoch changes.
    std::uint64_t received = 0;
};

std::unique_ptr<Datagrams> Session::datagrams() {
    if (impl_->cursor_issued.exchange(true))
        throw std::logic_error("Cursor encryption channel already issued for this session");
    return std::unique_ptr<Datagrams>(new Datagrams(std::make_unique<Datagrams::Impl>(impl_->master, impl_->local)));
}

Datagrams::Datagrams(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Datagrams::~Datagrams() = default;

namespace {
constexpr std::size_t token_size = 8, datagram_prefix_size = 16;
static_assert(datagram_overhead == datagram_prefix_size - token_size + tag_size);
constexpr std::uint8_t cursor_channel = 1;
}

Bytes Datagrams::seal(const Bytes& packet) {
    if (packet.size() <= token_size || packet.size() > net::max_datagram_size - datagram_overhead)
        throw std::invalid_argument("Invalid cursor datagram length");
    auto& tx = impl_->tx;
    if (tx.sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Cursor datagram counter exhausted; reconnect");
    const auto sequence = ++tx.sequence;
    Bytes record(packet.size() + datagram_overhead);
    std::copy_n(packet.begin(), token_size, record.begin());
    store_u64(record.data() + token_size, sequence);
    tx.select_key(impl_->master, impl_->local, sequence, cursor_channel);
    const auto iv = record_iv(sequence);
    const auto length = packet.size() - token_size;
    if (mbedtls_gcm_crypt_and_tag(&tx.gcm, MBEDTLS_GCM_ENCRYPT, length, iv.data(), iv.size(),
                                  record.data(), datagram_prefix_size, packet.data() + token_size,
                                  record.data() + datagram_prefix_size, tag_size,
                                  record.data() + datagram_prefix_size + length) != 0)
        throw std::runtime_error("Could not encrypt cursor datagram");
    return record;
}

std::optional<Bytes> Datagrams::open(const Bytes& record) {
    if (record.size() <= datagram_prefix_size + tag_size || record.size() > net::max_datagram_size)
        return std::nullopt;
    const auto sequence = load_u64(record.data() + token_size);
    auto& rx = impl_->rx;
    if (!sequence) return std::nullopt;
    if (sequence <= rx.sequence) {
        const auto age = rx.sequence - sequence;
        if (age >= 64 || ((impl_->received >> age) & 1)) return std::nullopt;
    }
    rx.select_key(impl_->master, opposite(impl_->local), sequence, cursor_channel);
    const auto iv = record_iv(sequence);
    const auto length = record.size() - datagram_prefix_size - tag_size;
    Bytes packet(token_size + length);
    if (mbedtls_gcm_auth_decrypt(&rx.gcm, length, iv.data(), iv.size(), record.data(), datagram_prefix_size,
                                 record.data() + datagram_prefix_size + length, tag_size,
                                 record.data() + datagram_prefix_size, packet.data() + token_size) != 0)
        return std::nullopt;
    std::copy_n(record.begin(), token_size, packet.begin());
    if (sequence > rx.sequence) {
        const auto advance = sequence - rx.sequence;
        impl_->received = advance >= 64 ? 1 : (impl_->received << advance) | 1;
        rx.sequence = sequence;
    } else {
        impl_->received |= std::uint64_t{1} << (rx.sequence - sequence);
    }
    return packet;
}

} // namespace secure
