#include "mac/virtual_display.hpp"
#import <CoreGraphics/CGSession.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
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

// Run this query in a fresh process. WindowServer's creator-side mode cache
// can return a null CGDisplayMode for an otherwise active virtual screen.
int verify_mode(CGDirectDisplayID id, uint32_t width, uint32_t height, bool hi_dpi) {
    auto mode = CGDisplayCopyDisplayMode(id);
    if (!mode) throw std::runtime_error("Observer could not read the active display mode");
    const auto actual_width = CGDisplayModeGetPixelWidth(mode);
    const auto actual_height = CGDisplayModeGetPixelHeight(mode);
    const auto logical_width = CGDisplayModeGetWidth(mode);
    const auto logical_height = CGDisplayModeGetHeight(mode);
    std::cout << "Observed " << actual_width << 'x' << actual_height << " pixels, "
              << logical_width << 'x' << logical_height << " points\n";
    CGDisplayModeRelease(mode);
    if (logical_width != width || logical_height != height)
        throw std::runtime_error("WindowServer changed the requested Mac desktop resolution");
    const bool standard = actual_width == width && actual_height == height;
    const bool retina = actual_width == width * 2 && actual_height == height * 2;
    if (!standard && !(hi_dpi && retina))
        throw std::runtime_error("WindowServer selected an unexpected backing density");
    return 0;
}

void check_lifecycle(bool hi_dpi) {
    using namespace std::chrono_literals;
    const uint32_t width = 1280;
    const uint32_t height = 720;
    const auto original = online_displays();
    CGDirectDisplayID id = kCGNullDirectDisplay;
    @autoreleasepool {
        VirtualDisplay display(width, height, 60, hi_dpi);
        id = display.id();
        if (contains(original, id) || !contains(online_displays(), id) ||
            !CGDisplayIsActive(id) || CGDisplayIsInMirrorSet(id))
            throw std::runtime_error("MaxDisplay is not a newly registered independent display");
        NSTask *observer = [NSTask new];
        observer.executableURL = [NSURL fileURLWithPath:NSProcessInfo.processInfo.arguments.firstObject];
        observer.arguments = @[@"--verify", @(id).stringValue, @(width).stringValue, @(height).stringValue,
                               @(hi_dpi ? 1 : 0).stringValue];
        NSError *error = nil;
        if (![observer launchAndReturnError:&error])
            throw std::runtime_error("Could not launch display observer: " + std::string(error.localizedDescription.UTF8String));
        [observer waitUntilExit];
        if (observer.terminationStatus) throw std::runtime_error("Independent display mode check failed");
    }

    auto deadline = std::chrono::steady_clock::now() + 8s;
    while (contains(online_displays(), id)) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Virtual display stayed online after release");
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, false) == kCFRunLoopRunFinished)
            std::this_thread::sleep_for(25ms);
    }
    std::cout << "Created and removed independent " << width << 'x' << height << " display " << id << '\n';
}
} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session) {
            std::cout << "Skipped: this process has no Quartz window server session\n";
            return 77;
        }
        CFRelease(session);
        try {
            if (argc == 6 && std::string(argv[1]) == "--verify")
                return verify_mode(static_cast<CGDirectDisplayID>(std::stoul(argv[2])),
                                   static_cast<uint32_t>(std::stoul(argv[3])),
                                   static_cast<uint32_t>(std::stoul(argv[4])), std::string(argv[5]) == "1");
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
