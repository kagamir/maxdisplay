#include "display_scale.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void expect_mode(DisplayGeometry mode, unsigned desktop_w, unsigned desktop_h,
                 unsigned video_w, unsigned video_h, unsigned fps = 60) {
    if (mode.logical_width != desktop_w || mode.logical_height != desktop_h ||
        mode.stream_width != video_w || mode.stream_height != video_h || mode.fps != fps)
        throw std::runtime_error("Unexpected desktop, video resolution, or frame rate");
}

void rejected(const SenderOptions& options, unsigned monitor_w = 0, unsigned monitor_h = 0) {
    try {
        resolve_display_geometry(options, monitor_w, monitor_h);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Unsupported display geometry was accepted");
}
}

int main() {
    try {
        SenderOptions options;
        rejected(options); // Both the explicit request and the monitor are absent.
        expect_mode(resolve_display_geometry(options, 1920, 1080), 1920, 1080, 1920, 1080);
        options.scale = 0.75;
        expect_mode(resolve_display_geometry(options, 1920, 1080), 2560, 1440, 1920, 1080);
        options.scale = 1.25;
        expect_mode(resolve_display_geometry(options, 1366, 768), 1093, 614, 1366, 768);
        options.scale = 2;
        expect_mode(resolve_display_geometry(options, 1921, 1081), 961, 541, 1920, 1080);
        expect_mode(resolve_display_geometry(options, 3840, 2160), 1920, 1080, 3840, 2160, 30);
        options.adaptive_fps = false;
        rejected(options, 3840, 2160);
        options.codec = Codec::h265;
        expect_mode(resolve_display_geometry(options, 3840, 2160), 1920, 1080, 3840, 2160);
        rejected(options, 7680, 4320);
        options.scale = 0.0000001;
        rejected(options, 1920, 1080);
        options.width = 1920;
        options.height = 1080;
        options.scale = 0.75;
        expect_mode(resolve_display_geometry(options, 3840, 2160), 1920, 1080, 1440, 810);
        options.scale = 1.5;
        expect_mode(resolve_display_geometry(options), 1920, 1080, 2880, 1620);
        options.width = 641;
        options.height = 361;
        options.scale = 2;
        expect_mode(resolve_display_geometry(options), 641, 361, 1282, 722);
        options.scale = 1;
        rejected(options); // Odd video dimensions cannot be encoded as NV12.
        options.height = 0;
        rejected(options, 1920, 1080);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
