#include "app.hpp"
#include "encoder.hpp"
#include "net.hpp"
#include "rate_control.hpp"
#include "udp.hpp"
#include "wire.hpp"
#include "virtual_display.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <condition_variable>
#include <deque>
#include <optional>
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

std::uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

// The capture queue never waits for I/O. A single writer owns the TCP byte
// stream so a frame already partially written is completed or the socket closed.
class Connection {
public:
    explicit Connection(const SenderOptions& options)
        : legacy_(options.legacy), control_(options.bitrate, options.fps, options.timeout_ms,
                                           options.adaptive, !options.legacy) {
        if (options.transport == Transport::udp)
            udp_ = std::make_unique<udp::Sender>(options.host, options.port);
        else
            tcp_ = net::connect_tcp(options.host, options.port);
    }

    ~Connection() { close(); }

    void send_header(const SenderOptions& options) {
        const auto timeout = legacy_ ? 0u : options.timeout_ms;
        if (udp_)
            udp_->send(wire::encode_header({options.width, options.height, options.fps, options.codec, timeout}));
        else
            wire::send_header(tcp_, options.width, options.height, options.fps, options.codec, timeout);
        writer_ = std::thread([this] {
            try { run(); }
            catch (...) { fail(std::current_exception()); }
        });
    }

    struct EncodeRequest { std::uint32_t bitrate; bool force_key; };

    std::optional<EncodeRequest> prepare(std::uint64_t capture, bool refresh_only) {
        std::lock_guard lock(mutex_);
        if (closing_) return std::nullopt;
        const auto now = now_us();
        expire(now);
        if (control_.expired(now, capture)) {
            // This sample was already old when ScreenCaptureKit delivered it.
            // The timer can resample its image if no newer sample arrives.
            pending_.clear();
            control_.loss(now);
            return std::nullopt;
        }
        const bool force = control_.take_key_request(now);
        if (!force && (refresh_only || control_.needs_key())) return std::nullopt;
        // encode() finishes the submitted frame before the capture queue submits
        // another. The callback itself may be delivered on a VideoToolbox thread.
        capture_us_ = std::max(capture, capture_us_ + 1);
        return EncodeRequest{control_.bitrate(), force};
    }

    void encoded(std::vector<uint8_t> bytes, bool keyframe) {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        const auto now = now_us();
        expire(now);
        if (control_.expired(now, capture_us_) || pending_.size() >= 4) {
            pending_.clear();
            control_.loss(now);
            if (control_.expired(now, capture_us_)) return;
        }
        const auto sequence = next_sequence_++;
        if (!control_.accept_encoded(sequence, keyframe)) return;
        pending_.push_back({sequence, capture_us_, keyframe, std::move(bytes)});
        wake_.notify_one();
    }

    void fail(std::exception_ptr error) {
        std::lock_guard lock(mutex_);
        if (!closing_) error_ = std::move(error);
        closing_ = true;
        wake_.notify_one();
    }

    void check() {
        std::lock_guard lock(mutex_);
        if (error_) std::rethrow_exception(error_);
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            wake_.notify_one();
        }
        if (udp_) udp_->shutdown();
        else tcp_.shutdown();
        if (writer_.joinable()) writer_.join();
    }

private:
    void expire(std::uint64_t now) {
        if (control_.poll(now)) pending_.clear();
        if (!pending_.empty() && control_.expired(now, pending_.front().timestamp_us)) {
            pending_.clear();
            control_.loss(now);
        }
    }

    void feedback(const std::vector<std::uint8_t>& bytes) {
        const auto ack = wire::unpack_feedback(bytes);
        std::lock_guard lock(mutex_);
        if (control_.acknowledge(ack.sequence, ack.request_keyframe, now_us())) pending_.clear();
    }

    void receive_feedback() {
        if (udp_) {
            if (udp_->peer_closed()) throw std::runtime_error("Windows receiver disconnected");
            if (!legacy_)
                if (auto message = udp_->take_feedback()) feedback(*message);
        } else if (legacy_) {
            if (tcp_.peer_closed()) throw std::runtime_error("Windows receiver disconnected");
        } else {
            // Fixed-size records may straddle TCP reads. Limit each drain to
            // leave time to transmit when feedback arrives in a burst.
            for (unsigned i = 0; i < 64; ++i) {
                auto count = tcp_.try_read(ack_.data() + ack_used_, ack_.size() - ack_used_);
                if (!count) break;
                ack_used_ += count;
                if (ack_used_ == ack_.size()) {
                    feedback(ack_);
                    ack_used_ = 0;
                }
            }
        }
    }

    void run() {
        for (;;) {
            receive_feedback();
            wire::Frame frame;
            {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, 5ms, [this] { return closing_ || !pending_.empty(); });
                if (closing_) return;
                const auto now = now_us();
                expire(now);
                if (pending_.empty()) continue;
                frame = std::move(pending_.front());
                pending_.pop_front();
                control_.sent(frame.sequence, now, frame.data.size());
            }
            const auto message = legacy_ ? std::move(frame.data) : wire::pack_frame(frame);
            if (!udp_) {
                wire::send_frame(tcp_, message);
            } else if (legacy_) {
                udp_->send(message);
            } else if (!udp_->send_frame(message)) {
                std::lock_guard lock(mutex_);
                pending_.clear();
                control_.loss(now_us());
            }
        }
    }

    bool legacy_;
    StreamControl control_;
    net::Socket tcp_;
    std::unique_ptr<udp::Sender> udp_;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<wire::Frame> pending_;
    bool closing_ = false;
    std::exception_ptr error_;
    std::uint32_t next_sequence_ = 1;
    std::uint64_t capture_us_ = 0;
    std::vector<std::uint8_t> ack_ = std::vector<std::uint8_t>(wire::feedback_size);
    std::size_t ack_used_ = 0;
};
} // namespace

@interface MDStreamSink : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<Connection> connection;
    std::shared_ptr<Encoder> encoder;
    // Accessed only on the capture queue once capture starts.
    CVPixelBufferRef latest;
    std::uint32_t bitrate;
}
- (void)encodeLatestAt:(std::uint64_t)capture refreshOnly:(bool)refresh;
@end

@implementation MDStreamSink
- (void)dealloc {
    if (latest) CVPixelBufferRelease(latest);
}

- (void)encodeLatestAt:(std::uint64_t)capture refreshOnly:(bool)refresh {
    if (!encoder || !latest) return;
    try {
        const auto request = connection->prepare(capture, refresh);
        if (!request) return;
        if (bitrate != request->bitrate) {
            encoder->set_bitrate(request->bitrate);
            bitrate = request->bitrate;
        }
        encoder->encode(latest, CMTimeMake(now_us(), 1'000'000), request->force_key);
    } catch (...) { connection->fail(std::current_exception()); }
}

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
        CVPixelBufferRetain(pixels);
        if (latest) CVPixelBufferRelease(latest);
        latest = pixels;
        auto capture = now_us();
        // ScreenCaptureKit's display time and the CoreMedia host clock share
        // the mach_absolute_time clock. Account for waiting on the sample queue.
        NSNumber *displayTime = attachments.firstObject[SCStreamFrameInfoDisplayTime];
        if (displayTime.unsignedLongLongValue) {
            auto age = CMTimeSubtract(CMClockGetTime(CMClockGetHostTimeClock()),
                                      CMClockMakeHostTimeFromSystemUnits(displayTime.unsignedLongLongValue));
            age = CMTimeConvertScale(age, 1'000'000, kCMTimeRoundingMethod_Default);
            if (CMTIME_IS_NUMERIC(age) && age.value > 0)
                capture -= std::min(capture, static_cast<std::uint64_t>(age.value));
        }
        [self encodeLatestAt:capture refreshOnly:false];
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
        [connection](std::vector<uint8_t> frame, bool keyframe) { connection->encoded(std::move(frame), keyframe); },
        options.codec, options.require_hardware);
    MDStreamSink *sink = [MDStreamSink new];
    sink->encoder = encoder;
    sink->bitrate = options.bitrate;
    sink->connection = connection;
    SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:sink];
    auto samples = dispatch_queue_create("maxdisplay.capture", DISPATCH_QUEUE_SERIAL);
    NSError *error = nil;
    if (![stream addStreamOutput:sink type:SCStreamOutputTypeScreen sampleHandlerQueue:samples error:&error])
        throw capture_error("Adding capture output", error);

    // Dispatch timers coalesce when encoding is busy. One retained image is
    // sufficient to regenerate an IDR if a stationary desktop loses a packet.
    auto refresh = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, samples);
    dispatch_source_set_timer(refresh, DISPATCH_TIME_NOW, 25 * NSEC_PER_MSEC, 3 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(refresh, ^{ [sink encodeLatestAt:now_us() refreshOnly:true]; });
    dispatch_resume(refresh);
    std::exception_ptr failure;
    try {
        change_capture(stream, true);
        std::cout << "Streaming MaxDisplay " << options.width << 'x' << options.height << " to "
                  << options.host << ':' << options.port << " using "
                  << codec_name(options.codec) << (encoder->hardware_accelerated() ? " (hardware)" : " (software)")
                  << " over "
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

    dispatch_source_cancel(refresh);
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
