#include "mac/virtual_display.hpp"
#import <CoreGraphics/CGSession.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::vector<CGDirectDisplayID> online_displays() {
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess)
        throw std::runtime_error("Cannot enumerate online displays");
    std::vector<CGDirectDisplayID> displays(count);
    if (count && CGGetOnlineDisplayList(count, displays.data(), &count) != kCGErrorSuccess)
        throw std::runtime_error("Cannot read online display IDs");
    displays.resize(count);
    return displays;
}

bool contains(const std::vector<CGDirectDisplayID>& displays, CGDirectDisplayID id) {
    return std::find(displays.begin(), displays.end(), id) != displays.end();
}

void check_lifecycle(bool hi_dpi) {
    using namespace std::chrono_literals;
    const auto original = online_displays();
    CGDirectDisplayID id = kCGNullDirectDisplay;
    @autoreleasepool {
        VirtualDisplay display(1280, 720, 60, hi_dpi);
        id = display.id();
        if (contains(original, id) || !contains(online_displays(), id) ||
            !CGDisplayIsActive(id) || CGDisplayIsInMirrorSet(id))
            throw std::runtime_error("MaxDisplay is not a newly registered independent display");
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
        if (!mode) throw std::runtime_error("Cannot read the virtual display mode");
        const bool matches = CGDisplayModeGetPixelWidth(mode) == 1280 && CGDisplayModeGetPixelHeight(mode) == 720 &&
                             CGDisplayModeGetWidth(mode) == (hi_dpi ? 640u : 1280u) &&
                             CGDisplayModeGetHeight(mode) == (hi_dpi ? 360u : 720u);
        CGDisplayModeRelease(mode);
        if (!matches) throw std::runtime_error("Virtual display did not use the requested 1280x720 pixel mode");
    }

    auto deadline = std::chrono::steady_clock::now() + 8s;
    while (contains(online_displays(), id)) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Virtual display stayed online after release");
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, false) == kCFRunLoopRunFinished)
            std::this_thread::sleep_for(25ms);
    }
    std::cout << "Created and removed independent 1280x720 display " << id << '\n';
}
} // namespace

int main() {
    @autoreleasepool {
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session) {
            std::cout << "Skipped: this process has no Quartz window server session\n";
            return 77;
        }
        CFRelease(session);
        try {
            check_lifecycle(false);
            check_lifecycle(true);
        }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
