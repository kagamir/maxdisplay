#include "app.hpp"
#include "build_version.hpp"
#include "display_scale.hpp"
#include "third_party_notices.hpp"

#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
void print_help() {
    std::cout << "MaxDisplay - use a Windows PC as a Mac extended display\n\n"
                 "Windows: maxdisplay [receive] [--bind IP] [--port PORT]\n"
                 "                              [--require-hardware] [--windowed] [--stats] [--password PASSWORD]\n"
                 "macOS:   maxdisplay send WINDOWS_HOST [--port PORT] [--size WIDTHxHEIGHT]\n"
                 "                                      [--scale FACTOR] [--fps FPS] [--bitrate-mbps MBPS]\n"
                 "                                      [--codec h264|h265|av1] [--transport tcp|udp]\n"
                 "                                      [--frame-timeout-ms MS] [--fixed-bitrate] [--fixed-fps]\n"
                 "                                      [--require-hardware] [--video-cursor] [--stats] [--password PASSWORD]\n\n"
                 "Windows listens on TCP and UDP at --port (default 49300). Start it first.\n"
                 "Use --password PASSWORD on both devices for AES-128-GCM encryption. Omit it on both for plaintext.\n"
                 "Use --password=VALUE for a password beginning with a dash.\n"
                 "--stats prints per-second streaming statistics on this device.\n"
                 "--size sets the Mac virtual desktop. The stream size is --size times --scale.\n"
                 "Omit --size to use the Windows monitor's pixel dimensions divided by --scale for the desktop.\n"
                 "H.264/H.265 prefer hardware and warn on software fallback. --require-hardware disables fallback.\n"
                 "--fps and --bitrate-mbps are adaptive ceilings. Each can be fixed independently.\n"
                 "The Mac sends its pointer over UDP to the same receiver port. Use --video-cursor to capture it in video.\n"
                 "The default frame deadline is 200 ms.\n"
                 "Windows: F11 toggles full screen, Esc exits. Mac: Ctrl+C stops sharing.\n"
                 "Use --version to print the build version.\n"
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

constexpr std::string_view password_prefix = "--password=";

std::string_view password_value(std::string_view key, int& index, int argc, char** argv) {
    if (key == "--password") return option_value(index, argc, argv);
    const auto value = key.substr(password_prefix.size());
    if (value.empty()) throw std::invalid_argument("--password requires a nonempty value");
    return value;
}

#ifdef __APPLE__
Transport parse_transport(std::string_view value) {
    if (value == "tcp") return Transport::tcp;
    if (value == "udp") return Transport::udp;
    throw std::invalid_argument("--transport expects tcp or udp");
}

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
        if (std::string_view(argv[i]) == "--password") {
            option_value(i, argc, argv);
            continue;
        }
        if (std::string_view(argv[i]) == "--licenses") {
            for (const auto& notice : third_party_notices) std::cout << notice;
            std::cout << '\n';
            return 0;
        }
        if (std::string_view(argv[i]) == "--version") {
            std::cout << "MaxDisplay " << (build_version::release_tag[0] ? build_version::release_tag : "dev") << '\n';
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
    for (int i = 3; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--size") {
            const auto value = option_value(i, argc, argv);
            auto separator = value.find('x');
            if (separator == value.npos) throw std::invalid_argument("Use --size WIDTHxHEIGHT");
            options.width = number(value.substr(0, separator), 1, std::numeric_limits<std::uint32_t>::max(), key);
            options.height = number(value.substr(separator + 1), 1, std::numeric_limits<std::uint32_t>::max(), key);
        } else if (key == "--scale") {
            options.scale = parse_scale(option_value(i, argc, argv));
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
        } else if (key == "--video-cursor") {
            options.video_cursor = true;
        } else if (key == "--fixed-bitrate") {
            options.adaptive = false;
        } else if (key == "--fixed-fps") {
            options.adaptive_fps = false;
        } else if (key == "--password" || key.compare(0, password_prefix.size(), password_prefix) == 0) {
            options.password = password_value(key, i, argc, argv);
        } else if (key == "--stats") {
            options.stats = true;
        } else if (key == "--require-hardware") {
            options.require_hardware = true;
        } else if (key == "--port") {
            options.port = static_cast<std::uint16_t>(number(option_value(i, argc, argv), 1, 65535, key));
        } else {
            throw std::invalid_argument("Unknown sender option: " + std::string(key));
        }
    }
    if (options.require_hardware && options.codec == Codec::av1)
        throw std::invalid_argument("--require-hardware requires --codec h264 or --codec h265");
    if (options.width) resolve_display_geometry(options);
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
        } else if (key == "--password" || key.compare(0, password_prefix.size(), password_prefix) == 0) {
            options.password = password_value(key, i, argc, argv);
        } else if (key == "--stats") {
            options.stats = true;
        } else if (key == "--require-hardware") {
            options.require_hardware = true;
        } else {
            throw std::invalid_argument("Unknown receiver option: " + std::string(key));
        }
    }
    return run_receiver(options);
#endif
}
