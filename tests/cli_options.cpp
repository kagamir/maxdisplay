#include "app.hpp"

#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
SenderOptions sender;
ReceiverOptions receiver;

int invoke(std::initializer_list<const char*> arguments) {
    std::vector<std::string> args{"maxdisplay"};
    args.insert(args.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return run_cli(static_cast<int>(argv.size()), argv.data());
}

void expect(bool test, const char* message) {
    if (!test) throw std::runtime_error(message);
}

void invalid(std::initializer_list<const char*> arguments) {
    try {
        invoke(arguments);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected invalid arguments to be rejected");
}
} // namespace

// Capture the final options at the normal dispatch boundary without opening a connection.
int run_sender(const SenderOptions& options) { sender = options; return 0; }
int run_receiver(const ReceiverOptions& options) { receiver = options; return 0; }

int main() {
    try {
#ifdef __APPLE__
        expect(invoke({"send", "example"}) == 0 && sender.width == 1920 && sender.height == 1080 &&
               sender.scale == 1 && sender.codec == Codec::h264 && sender.transport == Transport::tcp,
               "Unexpected Mac defaults");
        expect(invoke({"send", "example", "--codec", "av1", "--transport", "udp", "--size", "640x360",
                       "--scale", "1.5"}) == 0 && sender.width == 960 && sender.height == 540 &&
               sender.scale == 1.5 && sender.codec == Codec::av1 && sender.transport == Transport::udp,
               "Fractional scale or media selections lost");
        expect(invoke({"send", "example", "--scale", "2", "--size", "641x361"}) == 0 &&
               sender.width == 1282 && sender.height == 722 && sender.scale == 2,
               "2x mode or option order mismatch");
        expect(invoke({"send", "example", "--size", "7680x4320", "--scale", ".25"}) == 0 &&
               sender.width == 1920 && sender.height == 1080, "Downscaling used pre-scale limits");
        expect(invoke({"send", "example", "--size", "100x100", "--scale", "0.58"}) == 0 &&
               sender.width == 58 && sender.height == 58, "Binary rounding rejected a valid pixel count");
        for (auto bad : {"0", "-1", "NaN", "inf", "2oops", "1e999", "0.00001", "0.333", "1e100"})
            invalid({"send", "example", "--scale", bad});
        invalid({"send", "example", "--size", "640x360", "--scale", "100"});
        invalid({"send", "example", "--size", "641x361"});
        invalid({"send", "example", "--codec", "unknown"});
        invalid({"send", "example", "--transport", "sctp"});
#elif defined(_WIN32)
        expect(invoke({}) == 0 && receiver.transport == Transport::tcp, "Unexpected Windows defaults");
        expect(invoke({"receive", "--transport", "udp", "--port", "12345", "--windowed"}) == 0 &&
               receiver.transport == Transport::udp && receiver.port == 12345 && receiver.windowed,
               "Windows UDP options lost");
        invalid({"receive", "--transport", "invalid"});
#endif
        std::cout << "CLI option checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
