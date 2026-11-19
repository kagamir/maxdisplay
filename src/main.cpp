#include "app.hpp"
#include "wire.hpp"

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void print_help() {
    std::cout << "MaxDisplay - use a Windows PC as a Mac extended display\n\n"
                 "Windows: maxdisplay [receive] [--bind IP] [--port PORT] [--windowed]\n"
                 "macOS:   maxdisplay send WINDOWS_HOST [--port PORT] [--size WIDTHxHEIGHT]\n"
                 "                                      [--fps FPS] [--bitrate-mbps MBPS]\n\n"
                 "Start Windows first. Default: TCP 49300, 1920x1080, 60 fps, 12 Mbps.\n"
                 "Windows: F11 toggles full screen, Esc exits. Mac: Ctrl+C stops sharing.\n";
}

std::uint32_t number(std::string_view value, std::uint32_t min, std::uint32_t max,
                     std::string_view option) {
    std::uint32_t result = 0;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result < min || result > max)
        throw std::invalid_argument(std::string(option) + " expects an integer between " +
                                    std::to_string(min) + " and " + std::to_string(max));
    return result;
}

std::string_view option_value(int& index, int argc, char** argv) {
    if (index + 1 == argc || std::string_view(argv[index + 1]).empty() || argv[index + 1][0] == '-')
        throw std::invalid_argument(std::string("Missing value for ") + argv[index]);
    return argv[++index];
}

int run(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            print_help();
            return 0;
        }
    }
#if defined(__APPLE__)
    if (argc < 3 || std::string_view(argv[1]) != "send" || !argv[2][0] || argv[2][0] == '-') {
        print_help();
        return 2;
    }
    SenderOptions options;
    options.host = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--size") {
            const auto value = option_value(i, argc, argv);
            auto separator = value.find('x');
            if (separator == value.npos) throw std::invalid_argument("Use --size WIDTHxHEIGHT");
            options.width = number(value.substr(0, separator), 48, 4096, key);
            options.height = number(value.substr(separator + 1), 48, 2304, key);
        } else if (key == "--fps") {
            options.fps = number(option_value(i, argc, argv), 1, 120, key);
        } else if (key == "--bitrate-mbps") {
            options.bitrate = number(option_value(i, argc, argv), 1, 100, key) * 1'000'000;
        } else if (key == "--port") {
            options.port = static_cast<std::uint16_t>(number(option_value(i, argc, argv), 1, 65535, key));
        } else {
            throw std::invalid_argument("Unknown sender option: " + std::string(key));
        }
    }
    if (!wire::valid_mode(options.width, options.height, options.fps))
        throw std::invalid_argument("Use even dimensions and a frame rate within H.264 Level 5.1 (e.g. 3840x2160 at 30 fps)");
    return run_sender(options);
#elif defined(_WIN32)
    ReceiverOptions options;
    const bool explicit_receive = argc > 1 && std::string_view(argv[1]) == "receive";
    for (int i = explicit_receive ? 2 : 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--bind") {
            options.bind = option_value(i, argc, argv);
        } else if (key == "--port") {
            options.port = static_cast<std::uint16_t>(number(option_value(i, argc, argv), 1, 65535, key));
        } else if (key == "--windowed") {
            options.windowed = true;
        } else {
            throw std::invalid_argument("Unknown receiver option: " + std::string(key));
        }
    }
    return run_receiver(options);
#endif
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "MaxDisplay: " << error.what() << '\n';
        return 1;
    }
}
