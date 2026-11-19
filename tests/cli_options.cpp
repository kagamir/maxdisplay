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
        expect(invoke({"send", "example"}) == 0 && sender.width == 0 && sender.height == 0 &&
               sender.scale == 1 && sender.codec == Codec::h264 && sender.transport == Transport::tcp &&
               sender.timeout_ms == 200 && sender.adaptive && sender.adaptive_fps && !sender.require_hardware &&
               !sender.stats && !sender.video_cursor && sender.password.empty(),
               "Unexpected Mac defaults");
        expect(invoke({"send", "example", "--codec", "av1", "--transport", "udp", "--size", "640x360",
                       "--scale", "1.5"}) == 0 && sender.width == 640 && sender.height == 360 &&
               sender.scale == 1.5 && sender.codec == Codec::av1 && sender.transport == Transport::udp,
               "Fractional scale or media selections lost");
        expect(invoke({"send", "example", "--codec", "h265", "--require-hardware", "--frame-timeout-ms", "150",
                       "--bitrate-mbps", "16", "--fixed-bitrate", "--stats"}) == 0 && sender.codec == Codec::h265 &&
               sender.require_hardware && sender.timeout_ms == 150 && sender.bitrate == 16'000'000 &&
               !sender.adaptive && sender.adaptive_fps && sender.stats, "HEVC streaming options lost");
        expect(invoke({"send", "example", "--fps", "48", "--fixed-fps", "--scale", "0.75"}) == 0 &&
               sender.fps == 48 && !sender.adaptive_fps && sender.adaptive && sender.width == 0,
               "Fixed frame rate interfered with adaptive bitrate or monitor sizing");
        expect(invoke({"send", "example", "--video-cursor"}) == 0 && sender.video_cursor,
               "Could not select the video cursor");
        expect(invoke({"send", "example", "--password", u8"共享 密碼"}) == 0 &&
               sender.password == u8"共享 密碼", "Sender lost its UTF-8 password");
        expect(invoke({"send", "example", "--password=--help"}) == 0 && sender.password == "--help",
               "Option-like password triggered help");
        invalid({"send", "example", "--password"});
        invalid({"send", "example", "--password", ""});
        invalid({"send", "example", "--password="});
        invalid({"send", "example", "--password", "--help"});
        invalid({"send", "example", "--legacy"});
        invalid({"send", "example", "--require-hardware", "--codec", "av1"});
        invalid({"send", "example", "--frame-timeout-ms", "0"});
        invalid({"send", "example", "--frame-timeout-ms", "60001"});
        expect(invoke({"send", "example", "--scale", "2", "--size", "641x361"}) == 0 &&
               sender.width == 641 && sender.height == 361 && sender.scale == 2,
               "2x mode or option order mismatch");
        expect(invoke({"send", "example", "--size", "7680x4320", "--scale", ".25"}) == 0 &&
               sender.width == 7680 && sender.height == 4320, "Downscaling changed the desktop dimensions");
        expect(invoke({"send", "example", "--size", "100x100", "--scale", "0.58"}) == 0 &&
               sender.width == 100 && sender.height == 100, "Binary rounding rejected a valid pixel count");
        for (auto bad : {"0", "-1", "NaN", "inf", "2oops", "1e999", "0.00001", "0.333", "1e100"})
            invalid({"send", "example", "--size", "1920x1080", "--scale", bad});
        invalid({"send", "example", "--size", "640x360", "--scale", "100"});
        invalid({"send", "example", "--size", "641x361"});
        invalid({"send", "example", "--codec", "unknown"});
        invalid({"send", "example", "--transport", "sctp"});
#elif defined(_WIN32)
        expect(invoke({}) == 0 && receiver.transport == Transport::tcp && !receiver.require_hardware &&
               !receiver.stats && receiver.password.empty(),
               "Unexpected Windows defaults");
        expect(invoke({"receive", "--transport", "udp", "--port", "12345", "--windowed", "--require-hardware", "--stats"}) == 0 &&
               receiver.transport == Transport::udp && receiver.port == 12345 && receiver.windowed && receiver.require_hardware && receiver.stats,
               "Windows UDP options lost");
        expect(invoke({"receive", "--password", u8"共享 密碼", "--transport", "udp"}) == 0 &&
               receiver.password == u8"共享 密碼", "Receiver lost its UTF-8 password");
        expect(invoke({"--password=-leading-dash"}) == 0 && receiver.password == "-leading-dash",
               "Default receive mode lost its password");
        invalid({"receive", "--password"});
        invalid({"receive", "--password", ""});
        invalid({"receive", "--password="});
        invalid({"receive", "--password", "--licenses"});
        invalid({"receive", "--transport", "invalid"});
#endif
        std::cout << "CLI option checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
