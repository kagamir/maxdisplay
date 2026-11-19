#include "app.hpp"
#include "encoder.hpp"
#include "net.hpp"
#include "udp.hpp"
#include "wire.hpp"
#include "virtual_display.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

volatile std::sig_atomic_t interrupted = 0;
void signal_stop(int) { interrupted = 1; }

class StopSignals {
public:
    StopSignals() {
        interrupted = 0;
        previous_int_ = std::signal(SIGINT, signal_stop);
        previous_term_ = std::signal(SIGTERM, signal_stop);
    }
    ~StopSignals() {
        std::signal(SIGINT, previous_int_);
        std::signal(SIGTERM, previous_term_);
    }
private:
    using Handler = void (*)(int);
    Handler previous_int_, previous_term_;
};

void pump(double seconds) {
    @autoreleasepool {
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false) == kCFRunLoopRunFinished)
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }
}

void wait_for(dispatch_semaphore_t done, Clock::time_point deadline, const char *operation,
              bool cancelable = true) {
    while (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) != 0) {
        if (cancelable && interrupted) throw std::runtime_error("Interrupted");
        if (Clock::now() >= deadline) throw std::runtime_error(std::string(operation) + " timed out");
        pump(0.025);
    }
}

std::runtime_error capture_error(const char *operation, NSError *error) {
    return std::runtime_error(std::string(operation) + ": " +
                              (error.localizedDescription.UTF8String ?: "unknown ScreenCaptureKit error"));
}

SCDisplay *find_display(CGDirectDisplayID display_id) {
    // WindowServer and ScreenCaptureKit observe new displays asynchronously.
    auto deadline = Clock::now() + 10s;
    while (Clock::now() < deadline && !interrupted) {
        __block SCDisplay *found = nil;
        __block NSError *failure = nil;
        auto done = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
            completionHandler:^(SCShareableContent *content, NSError *error) {
                failure = error;
                for (SCDisplay *screen in content.displays) {
                    if (screen.displayID == display_id) { found = screen; break; }
                }
                dispatch_semaphore_signal(done);
            }];
        wait_for(done, deadline, "Enumerating displays");
        if (failure) throw capture_error("Enumerating displays", failure);
        if (found) return found;
        pump(0.1);
    }
    throw std::runtime_error("MaxDisplay did not appear as an active extended display. Check System Settings > Displays.");
}

// TCP writes can block. UDP has its own bounded queue and service thread.
// Both paths preserve inter-frame dependencies.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    explicit Connection(const SenderOptions& options) {
        if (options.transport == Transport::udp)
            udp_ = std::make_unique<udp::Sender>(options.host, options.port);
        else
            tcp_ = net::connect_tcp(options.host, options.port);
    }

    void send_header(const SenderOptions& options) {
        if (udp_) {
            udp_->send(wire::encode_header({options.width, options.height, options.fps, options.codec}));
        } else {
            wire::send_header(tcp_, options.width, options.height, options.fps, options.codec);
        }
    }

    void send(std::vector<uint8_t> frame) {
        if (closing_.load()) return;
        if (udp_) {
            udp_->send(frame);
            return;
        }
        if (pending_.fetch_add(1) >= 12) {
            --pending_;
            throw std::runtime_error("Receiver is too slow. Try a lower bitrate or frame rate.");
        }
        auto self = shared_from_this();
        auto payload = std::make_shared<std::vector<uint8_t>>(std::move(frame));
        dispatch_async(writes_, ^{
            if (!self->closing_) {
                try { wire::send_frame(self->tcp_, *payload); }
                catch (...) { self->fail(std::current_exception()); }
            }
            --self->pending_;
        });
    }

    void fail(std::exception_ptr error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_) error_ = std::move(error);
        closing_ = true;
    }

    void check() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_) std::rethrow_exception(error_);
        }
        if (udp_ ? udp_->peer_closed() : tcp_.peer_closed())
            throw std::runtime_error("Windows receiver disconnected");
    }

    void close() {
        closing_ = true;
        if (udp_) udp_->shutdown();
        else tcp_.shutdown();
        dispatch_sync(writes_, ^{});
    }
private:
    net::Socket tcp_;
    std::unique_ptr<udp::Sender> udp_;
    dispatch_queue_t writes_ = dispatch_queue_create("maxdisplay.network", DISPATCH_QUEUE_SERIAL);
    std::atomic<unsigned> pending_{0};
    std::atomic<bool> closing_{false};
    std::mutex mutex_;
    std::exception_ptr error_;
};
} // namespace

@interface MDStreamSink : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<Connection> connection;
    std::shared_ptr<Encoder> encoder; // accessed only on the serial sample queue after registration
}
@end

@implementation MDStreamSink
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    connection->fail(std::make_exception_ptr(capture_error("Screen capture stopped", error)));
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample
        ofType:(SCStreamOutputType)type {
    @autoreleasepool {
        if (type != SCStreamOutputTypeScreen || !encoder || !CMSampleBufferIsValid(sample)) return;
        NSArray *attachments = (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(sample, false);
        NSNumber *status = attachments.firstObject[SCStreamFrameInfoStatus];
        if (!status || (status.integerValue != SCFrameStatusComplete && status.integerValue != SCFrameStatusStarted))
            return;
        CVImageBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
        if (!pixels) return;
        try { encoder->encode(pixels, CMSampleBufferGetPresentationTimeStamp(sample)); }
        catch (...) { connection->fail(std::current_exception()); }
    }
}
@end

namespace {
void change_capture(SCStream *stream, bool start) {
    __block NSError *failure = nil;
    auto done = dispatch_semaphore_create(0);
    void (^complete)(NSError *) = ^(NSError *error) {
        failure = error;
        dispatch_semaphore_signal(done);
    };
    if (start) [stream startCaptureWithCompletionHandler:complete];
    else [stream stopCaptureWithCompletionHandler:complete];
    const char *operation = start ? "Starting screen capture" : "Stopping screen capture";
    wait_for(done, Clock::now() + 10s, operation, start);
    if (failure) throw capture_error(operation, failure);
}

int stream_desktop(const SenderOptions& options) {
    if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess())
        throw std::runtime_error("Allow Screen Recording for MaxDisplay or your terminal in System Settings "
                                 "> Privacy & Security, then quit, reopen and retry.");

    auto connection = std::make_shared<Connection>(options);
    VirtualDisplay display(options.width, options.height, options.fps, options.scale == 2.0);
    CGDirectDisplayID display_id = display.id();
    SCDisplay *source = find_display(display_id);
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:source excludingWindows:@[]];
    SCStreamConfiguration *config = [SCStreamConfiguration new];
    config.width = options.width;
    config.height = options.height;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.colorSpaceName = kCGColorSpaceITUR_709;
    config.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
    config.minimumFrameInterval = CMTimeMake(1, static_cast<int32_t>(options.fps));
    config.queueDepth = 3;
    config.showsCursor = YES;
    config.capturesAudio = NO;

    connection->send_header(options);
    auto encoder = std::make_shared<Encoder>(options.width, options.height, options.fps, options.bitrate,
        [connection](std::vector<uint8_t> frame) { connection->send(std::move(frame)); }, options.codec);
    MDStreamSink *sink = [MDStreamSink new];
    sink->encoder = encoder;
    sink->connection = connection;
    SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:sink];
    auto samples = dispatch_queue_create("maxdisplay.capture", DISPATCH_QUEUE_SERIAL);
    NSError *error = nil;
    if (![stream addStreamOutput:sink type:SCStreamOutputTypeScreen sampleHandlerQueue:samples error:&error])
        throw capture_error("Adding capture output", error);

    std::exception_ptr failure;
    try {
        change_capture(stream, true);
        std::cout << "Streaming MaxDisplay " << options.width << 'x' << options.height << " to "
                  << options.host << ':' << options.port << " using "
                  << (options.codec == Codec::av1 ? "AV1" : "H.264") << " over "
                  << (options.transport == Transport::udp ? "UDP" : "TCP")
                  << ". Press Ctrl+C to stop." << std::endl;
        while (!interrupted) {
            connection->check();
            encoder->check_error();
            if (!CGDisplayIsOnline(display_id) || CGDisplayIsInMirrorSet(display_id))
                throw std::runtime_error("MaxDisplay is no longer an independent display");
            pump(0.2);
        }
    } catch (...) { failure = std::current_exception(); }

    connection->close();
    try { change_capture(stream, false); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    [stream removeStreamOutput:sink type:SCStreamOutputTypeScreen error:nullptr];
    // A late delivery after a stop timeout must not race encoder destruction.
    dispatch_sync(samples, ^{ sink->encoder.reset(); });
    try { encoder->finish(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure && !interrupted) std::rethrow_exception(failure);
    return 0;
}
} // namespace

int run_sender(const SenderOptions& options) {
    @autoreleasepool {
        StopSignals signals;
        try { return stream_desktop(options); }
        catch (...) {
            if (interrupted) return 0;
            throw;
        }
    }
}
