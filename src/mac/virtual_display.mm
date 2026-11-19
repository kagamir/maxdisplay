#include "virtual_display.hpp"

#import <Foundation/Foundation.h>

#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>

// CoreGraphics does not publish headers for virtual displays. These local
// types describe the message ABI; private classes are resolved only at runtime.
@interface MDDisplayDescriptor : NSObject
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy) NSString *name;
@property(nonatomic) unsigned int maxPixelsWide;
@property(nonatomic) unsigned int maxPixelsHigh;
@property(nonatomic) unsigned int vendorID;
@property(nonatomic) unsigned int productID;
@property(nonatomic) unsigned int serialNum;
@property(nonatomic) unsigned int serialNumber;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) CGPoint redPrimary;
@property(nonatomic) CGPoint greenPrimary;
@property(nonatomic) CGPoint bluePrimary;
@property(nonatomic) CGPoint whitePoint;
@end

@interface MDDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)rate;
@end

@interface MDDisplaySettings : NSObject
@property(nonatomic) unsigned int hiDPI;
@property(nonatomic, copy) NSArray *modes;
@end

@interface MDDisplay : NSObject
- (instancetype)initWithDescriptor:(MDDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(MDDisplaySettings *)settings;
@property(nonatomic, readonly) CGDirectDisplayID displayID;
@end

namespace {
Class private_class(NSString *name, std::initializer_list<SEL> required) {
    Class cls = NSClassFromString(name);
    for (SEL selector : required) {
        if (![cls instancesRespondToSelector:selector])
            throw std::runtime_error(std::string("Unsupported macOS virtual display API: ") +
                                     name.UTF8String + " " + NSStringFromSelector(selector).UTF8String);
    }
    return cls;
}

MDDisplay *make_display(uint32_t width, uint32_t height, uint32_t fps, bool hi_dpi) {
    Class descriptor_class = private_class(@"CGVirtualDisplayDescriptor",
        {@selector(setQueue:), @selector(setName:), @selector(setMaxPixelsWide:),
         @selector(setMaxPixelsHigh:), @selector(setSizeInMillimeters:), @selector(setVendorID:),
         @selector(setProductID:), @selector(setSerialNum:), @selector(setWhitePoint:),
         @selector(setRedPrimary:), @selector(setGreenPrimary:), @selector(setBluePrimary:)});
    Class mode_class = private_class(@"CGVirtualDisplayMode", {@selector(initWithWidth:height:refreshRate:)});
    Class settings_class = private_class(@"CGVirtualDisplaySettings", {@selector(setModes:), @selector(setHiDPI:)});
    Class display_class = private_class(@"CGVirtualDisplay",
        {@selector(initWithDescriptor:), @selector(applySettings:), @selector(displayID)});

    MDDisplayDescriptor *descriptor = [descriptor_class new];
    descriptor.name = @"MaxDisplay";
    descriptor.queue = dispatch_get_main_queue();
    descriptor.vendorID = 0xF0F0;
    descriptor.productID = 1;
    // A stable identity lets macOS remember the user's display arrangement.
    descriptor.serialNum = 1;
    if ([descriptor respondsToSelector:@selector(setSerialNumber:)]) descriptor.serialNumber = 1;
    descriptor.maxPixelsWide = width;
    descriptor.maxPixelsHigh = height;
    const double pixels_per_inch = hi_dpi ? 220 : 110;
    descriptor.sizeInMillimeters = CGSizeMake(width * 25.4 / pixels_per_inch, height * 25.4 / pixels_per_inch);
    descriptor.redPrimary = CGPointMake(0.640, 0.330);
    descriptor.greenPrimary = CGPointMake(0.300, 0.600);
    descriptor.bluePrimary = CGPointMake(0.150, 0.060);
    descriptor.whitePoint = CGPointMake(0.3127, 0.3290);

    // For HiDPI the descriptor advertises the backing pixels, while the mode
    // advertises points. CoreGraphics renders each point at 2× on both axes.
    const auto mode_width = hi_dpi ? width / 2 : width;
    const auto mode_height = hi_dpi ? height / 2 : height;
    MDDisplayMode *mode = [(MDDisplayMode *)[mode_class alloc] initWithWidth:mode_width
                                                                      height:mode_height
                                                                 refreshRate:fps];
    if (!mode) throw std::runtime_error("macOS rejected the virtual display mode");
    MDDisplaySettings *settings = [settings_class new];
    settings.hiDPI = hi_dpi ? 1 : 0;
    settings.modes = @[mode];
    MDDisplay *display = [(MDDisplay *)[display_class alloc] initWithDescriptor:descriptor];
    if (!display || ![display applySettings:settings] || display.displayID == kCGNullDirectDisplay)
        throw std::runtime_error("macOS could not register MaxDisplay with the requested display mode");
    return display;
}

void extend_display(CGDirectDisplayID display_id) {
    if (!CGDisplayIsInMirrorSet(display_id)) return;
    CGDisplayConfigRef change = nullptr;
    if (CGBeginDisplayConfiguration(&change) != kCGErrorSuccess)
        throw std::runtime_error("Could not begin display configuration");
    if (CGConfigureDisplayMirrorOfDisplay(change, display_id, kCGNullDirectDisplay) != kCGErrorSuccess) {
        CGCancelDisplayConfiguration(change);
        throw std::runtime_error("Could not disable mirroring for MaxDisplay");
    }
    if (CGCompleteDisplayConfiguration(change, kCGConfigureForAppOnly) != kCGErrorSuccess)
        throw std::runtime_error("Could not apply extended display configuration");
}

} // namespace

VirtualDisplay::VirtualDisplay(uint32_t width, uint32_t height, uint32_t fps, bool hi_dpi)
    : display_(make_display(width, height, fps, hi_dpi)) {
    using namespace std::chrono_literals;
    auto deadline = std::chrono::steady_clock::now() + 8s;
    bool requested_extension = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto display_id = id();
        if (CGDisplayIsOnline(display_id)) {
            if (CGDisplayIsInMirrorSet(display_id)) {
                if (!requested_extension) {
                    extend_display(display_id);
                    requested_extension = true;
                }
            } else if (CGDisplayIsActive(display_id)) {
                return;
            }
        }
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.025, false) == kCFRunLoopRunFinished)
            std::this_thread::sleep_for(25ms);
    }
    throw std::runtime_error("MaxDisplay did not become an active extended display. Check System Settings > Displays.");
}

CGDirectDisplayID VirtualDisplay::id() const { return display_.displayID; }
