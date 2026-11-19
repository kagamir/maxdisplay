#include "app.hpp"
#include "display_scale.hpp"
#include "third_party_notices.hpp"
#include "wire.hpp"

#include <charconv>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
void print_help() {
    std::cout << "MaxDisplay - use a Windows PC as a Mac extended display\n\n"
                 "Windows: maxdisplay [receive] [--bind IP] [--port PORT] [--transport tcp|udp]\n"
                 "                              [--require-hardware] [--windowed]\n"
                 "macOS:   maxdisplay send WINDOWS_HOST [--port PORT] [--size WIDTHxHEIGHT]\n"
                 "                                      [--scale FACTOR] [--fps FPS] [--bitrate-mbps MBPS]\n"
                 "                                      [--codec h264|h265|av1] [--transport tcp|udp]\n"
                 "                                      [--frame-timeout-ms MS] [--fixed-bitrate]\n"
                 "                                      [--require-hardware] [--legacy]\n\n"
                 "Start Windows first. Defaults: H.264 over TCP 49300, 1920x1080, scale 1, 60 fps, 12 Mbps.\n"
                 "The pixel resolution is --size times --scale. Scale 2 requests macOS HiDPI.\n"
                 "H.264/H.265 prefer hardware. AV1 uses the bundled software codec.\n"
                 "Adaptive bitrate uses --bitrate-mbps as its ceiling; --fixed-bitrate disables adaptation.\n"
                 "The default frame deadline is 200 ms. Use --legacy for older H.264/AV1 receivers.\n"
                 "Windows: F11 toggles full screen, Esc exits. Mac: Ctrl+C stops sharing.\n"
                 "Use --licenses to view bundled third-party notices.\n";
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

Transport parse_transport(std::string_view value) {
    if (value == "tcp") return Transport::tcp;
    if (value == "udp") return Transport::udp;
    throw std::invalid_argument("--transport expects tcp or udp");
}

#ifdef __APPLE__
double parse_scale(std::string_view value) {
    std::istringstream input{std::string(value)};
    input.imbue(std::locale::classic());
    double scale = 0;
    if (!(input >> std::noskipws >> scale) || !input.eof() || !std::isfinite(scale) || scale <= 0)
        throw std::invalid_argument("--scale expects a positive finite number (e.g. 1.5 or 2)");
    return scale;
}

Codec parse_codec(std::string_view value) {
    if (value == "h264") return Codec::h264;
    if (value == "h265" || value == "hevc") return Codec::h265;
    if (value == "av1") return Codec::av1;
    throw std::invalid_argument("--codec expects h264, h265 or av1");
}
#endif
} // namespace

int run_cli(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--licenses") {
            std::cout << third_party_notices << '\n';
            return 0;
        }
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
    auto base_width = options.width;
    auto base_height = options.height;
    auto scale = options.scale;
    bool explicit_timeout = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--size") {
            const auto value = option_value(i, argc, argv);
            auto separator = value.find('x');
            if (separator == value.npos) throw std::invalid_argument("Use --size WIDTHxHEIGHT");
            base_width = number(value.substr(0, separator), 1, std::numeric_limits<std::uint32_t>::max(), key);
            base_height = number(value.substr(separator + 1), 1, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "--scale") {
            scale = parse_scale(option_value(i, argc, argv));
        } else if (key == "--codec") {
            options.codec = parse_codec(option_value(i, argc, argv));
        } else if (key == "--transport") {
            options.transport = parse_transport(option_value(i, argc, argv));
        } else if (key == "--fps") {
            options.fps = number(option_value(i, argc, argv), 1, 120, key);
        } else if (key == "--bitrate-mbps") {
            options.bitrate = number(option_value(i, argc, argv), 1, 100, key) * 1'000'000;
        } else if (key == "--frame-timeout-ms") {
            options.timeout_ms = number(option_value(i, argc, argv), 1, 60'000, key);
            explicit_timeout = true;
        } else if (key == "--fixed-bitrate") {
            options.adaptive = false;
        } else if (key == "--require-hardware") {
            options.require_hardware = true;
        } else if (key == "--legacy") {
            options.legacy = true;
        } else if (key == "--port") {
            options.port = static_cast<std::uint16_t>(number(option_value(i, argc, argv), 1, 65535, key));
        } else {
            throw std::invalid_argument("Unknown sender option: " + std::string(key));
        }
    }
    if (options.require_hardware && options.codec == Codec::av1)
        throw std::invalid_argument("--require-hardware requires --codec h264 or --codec h265");
    if (options.legacy) {
        if (options.codec == Codec::h265 || explicit_timeout)
            throw std::invalid_argument("--legacy supports H.264 or AV1 and the old protocol has no frame deadline");
        options.adaptive = false;
    }
    set_display_size(options, base_width, base_height, scale);
    if (!wire::valid_mode(options.width, options.height, options.fps, options.codec)) {
        throw std::invalid_argument("Unsupported " + std::string(codec_name(options.codec)) +
            " mode after scaling: " + std::to_string(options.width) + "x" + std::to_string(options.height) +
            " at " + std::to_string(options.fps) + " fps");
    }
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
        } else if (key == "--transport") {
            options.transport = parse_transport(option_value(i, argc, argv));
        } else if (key == "--windowed") {
            options.windowed = true;
        } else if (key == "--require-hardware") {
            options.require_hardware = true;
        } else {
            throw std::invalid_argument("Unknown receiver option: " + std::string(key));
        }
    }
    return run_receiver(options);
#endif
}
