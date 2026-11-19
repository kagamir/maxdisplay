#include "app.hpp"
#include "display_scale.hpp"
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
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
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
        : legacy_(options.legacy) {
        if (options.transport == Transport::udp)
            udp_ = std::make_unique<udp::Sender>(options.host, options.port);
        else
            tcp_ = net::connect_tcp(options.host, options.port);
    }

    ~Connection() { close(); }

    wire::DisplaySize request_display_size() {
        return udp_ ? udp_->request_display_size() : wire::request_display_size(tcp_);
    }

    void send_header(const SenderOptions& options, const DisplayGeometry& mode) {
        control_.emplace(options.bitrate, options.fps, options.timeout_ms,
                         options.adaptive, options.adaptive_fps, !legacy_);
        const auto timeout = legacy_ ? 0u : options.timeout_ms;
        if (udp_)
            udp_->send(wire::encode_header({mode.stream_width, mode.stream_height, options.fps, options.codec, timeout}));
        else
            wire::send_header(tcp_, mode.stream_width, mode.stream_height, options.fps, options.codec, timeout);
        writer_ = std::thread([this] {
            try { run(); }
            catch (...) { fail(std::current_exception()); }
        });
    }

    struct EncodeRequest {
        std::uint32_t bitrate;
        std::uint32_t fps;
        bool force_key;
    };

    std::optional<EncodeRequest> prepare(std::uint64_t capture, bool refresh_only, bool& refining) {
        refining = false;
        std::lock_guard lock(mutex_);
        if (closing_) return std::nullopt;
        const auto now = now_us();
        expire(now);
        if (!refresh_only && !legacy_ && control_->expired(now, capture)) {
            // This sample was already old when ScreenCaptureKit delivered it.
            // The timer can resample its image if no newer sample arrives.
            pending_.clear();
            control_->loss(now, StreamControl::Cause::stale_capture);
            return std::nullopt;
        }
        const auto action = control_->next_capture(now, !refresh_only);
        refining = control_->refining(now);
        if (action == StreamControl::Capture::skip) return std::nullopt;
        // On a timer tick the retained image is re-sampled. The capture queue
        // owns that image and replaces it on every subsequent screen change.
        encode_start_us_ = now;
        // encode() finishes the submitted frame before the capture queue submits
        // another. The callback itself may be delivered on a VideoToolbox thread.
        capture_us_ = std::max(capture, capture_us_ + 1);
        return EncodeRequest{control_->bitrate(), control_->fps(), action == StreamControl::Capture::force_key};
    }

    void encoded(std::vector<uint8_t> bytes, bool keyframe) {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        const auto now = now_us();
        expire(now);
        control_->observe_encode_time(now - encode_start_us_, keyframe);
        const bool stale = !legacy_ && control_->expired(now, capture_us_, keyframe);
        if (stale || pending_.size() >= 4) {
            pending_.clear();
            control_->loss(now, stale ? StreamControl::Cause::slow_encode : StreamControl::Cause::backlog);
            if (stale) return;
        }
        const auto sequence = next_sequence_++;
        if (!control_->accept_encoded(sequence, keyframe)) return;
        pending_.push_back({sequence, capture_us_, keyframe, std::move(bytes)});
        wake_.notify_one();
    }

    void encode_skipped(bool requested_key) {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        control_->observe_encode_time(now_us() - encode_start_us_, requested_key);
        control_->encode_skipped();
    }

    void observe_quality(std::optional<double> psnr, std::optional<int> quantizer) {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        last_psnr_ = psnr;
        last_quantizer_ = quantizer;
        control_->observe_quality(now_us(), psnr, quantizer);
    }

    struct Snapshot {
        StreamControl::Stats stats;
        std::uint32_t bitrate, fps;
        std::optional<double> psnr;
        std::optional<int> quantizer;
    };

    Snapshot snapshot() {
        std::lock_guard lock(mutex_);
        return {control_->stats(), control_->bitrate(), control_->fps(), last_psnr_, last_quantizer_};
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
        if (control_->poll(now)) pending_.clear();
        if (!legacy_ && !pending_.empty() &&
            control_->expired(now, pending_.front().timestamp_us, pending_.front().keyframe)) {
            pending_.clear();
            control_->loss(now, StreamControl::Cause::queue_expired);
        }
    }

    void feedback(const std::vector<std::uint8_t>& bytes) {
        const auto ack = wire::unpack_feedback(bytes);
        std::lock_guard lock(mutex_);
        if (control_->acknowledge(ack.sequence, ack.request_keyframe, now_us())) pending_.clear();
    }

    void receive_feedback() {
        if (udp_) {
            if (udp_->peer_closed()) throw std::runtime_error("Windows receiver disconnected");
            if (!legacy_) {
                if (auto message = udp_->take_feedback()) feedback(*message);
            }
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
                // A blocking write can overlap a recovery on the capture queue.
                // Register before the write so recovery can retire its old GOP.
                control_->start_send(frame.sequence, now, frame.data.size(), frame.keyframe);
            }
            bool sent = true;
            if (legacy_) {
                if (udp_) udp_->send(frame.data);
                else wire::send_frame(tcp_, frame.data);
            } else {
                const auto message = wire::pack_frame(frame);
                const auto age_limit = std::chrono::microseconds(control_->deadline(frame.timestamp_us, frame.keyframe));
                const auto deadline = Clock::time_point(std::chrono::duration_cast<Clock::duration>(age_limit));
                sent = udp_ ? (Clock::now() < deadline && udp_->send_frame(message, frame.keyframe))
                            : wire::send_frame_before(tcp_, message, deadline);
            }
            std::lock_guard lock(mutex_);
            if (sent) {
                control_->confirm_send(frame.data.size(), frame.keyframe);
            } else {
                pending_.clear();
                control_->loss(now_us(), StreamControl::Cause::send_blocked);
            }
        }
    }

    bool legacy_;
    std::optional<StreamControl> control_;
    net::Socket tcp_;
    std::unique_ptr<udp::Sender> udp_;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<wire::Frame> pending_;
    bool closing_ = false;
    std::exception_ptr error_;
    std::uint32_t next_sequence_ = 1;
    std::uint64_t capture_us_ = 0, encode_start_us_ = 0;
    std::vector<std::uint8_t> ack_ = std::vector<std::uint8_t>(wire::feedback_size);
    std::size_t ack_used_ = 0;
    std::optional<double> last_psnr_;
    std::optional<int> last_quantizer_;
};

constexpr std::uint64_t idle_tick_us = 25'000;
void schedule_refresh(dispatch_source_t timer, std::uint64_t us) {
    const auto ns = us * NSEC_PER_USEC;
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, ns), ns, ns / 40);
}

void print_stats(const Connection::Snapshot& current, const StreamControl::Stats& previous, double seconds) {
    const auto& s = current.stats;
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << "sender target_kbps=" << current.bitrate / 1000.0
        << " sent_kbps=" << (s.bytes - previous.bytes) * 0.008 / seconds
        << " target_fps=" << current.fps << " sent_fps=" << (s.frames - previous.frames) / seconds
        << " delivery_kbps=" << s.delivery_bps / 1000.0
        << " rtt_ms=" << s.rtt_us / 1000.0 << " queue_ms=" << s.queue_us / 1000.0
        << " encode_ms=" << s.encode_us / 1000.0 << " psnr_db=";
    if (current.psnr) out << *current.psnr;
    else out << "n/a";
    if (current.quantizer) out << " av1_q=" << *current.quantizer;
    out << " keys_sent=" << s.keyframes - previous.keyframes
        << " last_sent_key_bytes=" << s.key_bytes << " last_acked_key_rtt_ms=" << s.key_rtt_us / 1000.0;
    constexpr const char* names[] = {"stale_capture", "slow_encode", "backlog", "queue_expired",
                                     "send_blocked", "unacknowledged", "receiver_request", "ack_delay"};
    static_assert(std::size(names) == static_cast<std::size_t>(StreamControl::Cause::count));
    out << " cuts/key_requests";
    for (std::size_t i = 0; i < std::size(names); ++i)
        out << ' ' << names[i] << '=' << s.cuts[i] - previous.cuts[i] << '/'
            << s.key_requests[i] - previous.key_requests[i];
    std::clog << out.str() << '\n';
}
} // namespace

@interface MDStreamSink : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<Connection> connection;
    std::shared_ptr<Encoder> encoder;
    // Accessed only on the capture queue once capture starts.
    CVPixelBufferRef latest;
    dispatch_source_t refresh_timer;
    std::uint64_t refresh_tick_us;
    bool timer_follows_capture;
    std::uint32_t bitrate, fps;
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
        bool refining;
        const auto request = connection->prepare(capture, refresh, refining);
        // Anchor to each actual capture. A free-running timer can miss a
        // Windows decoder's two-period idle drain before its first refinement.
        if (!refresh) {
            schedule_refresh(refresh_timer, refresh_tick_us);
            timer_follows_capture = true;
        } else if (timer_follows_capture && !refining) {
            schedule_refresh(refresh_timer, idle_tick_us);
            timer_follows_capture = false;
        }
        if (!request) return;
        if (bitrate != request->bitrate) {
            encoder->set_bitrate(request->bitrate);
            bitrate = request->bitrate;
            std::clog << "Adaptive encoder target: " << bitrate / 1000 << " kbps" << std::endl;
        }
        if (fps != request->fps) {
            fps = request->fps;
            std::clog << "Adaptive frame rate: " << fps << " fps" << std::endl;
        }
        if (encoder->encode(latest, CMTimeMake(now_us(), 1'000'000), request->force_key, fps))
            connection->observe_quality(encoder->last_psnr(), encoder->last_quantizer());
        else
            connection->encode_skipped(request->force_key);
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

int stream_desktop(SenderOptions options) {
    if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess())
        throw std::runtime_error("Allow Screen Recording for MaxDisplay or your terminal in System Settings "
                                 "> Privacy & Security, then quit, reopen and retry.");

    auto connection = std::make_shared<Connection>(options);
    wire::DisplaySize monitor{};
    if (!options.width && !options.height) monitor = connection->request_display_size();
    const auto mode = resolve_display_geometry(options, monitor.width, monitor.height);
    if (mode.fps < options.fps)
        std::clog << codec_name(options.codec) << " video at " << mode.stream_width << 'x'
                  << mode.stream_height << " limits the frame rate to " << mode.fps << " fps\n";
    options.fps = mode.fps;
    VirtualDisplay display(mode.logical_width, mode.logical_height, mode.fps, options.scale > 1.0);
    CGDirectDisplayID display_id = display.id();
    SCDisplay *source = find_display(display_id);
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:source excludingWindows:@[]];
    SCStreamConfiguration *config = [SCStreamConfiguration new];
    config.width = mode.stream_width;
    config.height = mode.stream_height;
    config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    config.colorSpaceName = kCGColorSpaceITUR_709;
    config.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
    config.minimumFrameInterval = CMTimeMake(1, static_cast<int32_t>(options.fps));
    config.queueDepth = 3;
    config.showsCursor = YES;
    config.capturesAudio = NO;

    connection->send_header(options, mode);
    auto encoder = std::make_shared<Encoder>(mode.stream_width, mode.stream_height, mode.fps, options.bitrate,
        [connection](std::vector<uint8_t> frame, bool keyframe) { connection->encoded(std::move(frame), keyframe); },
        options.codec, options.require_hardware, options.legacy);
    MDStreamSink *sink = [MDStreamSink new];
    sink->encoder = encoder;
    sink->bitrate = options.bitrate;
    sink->fps = options.fps;
    sink->connection = connection;
    SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:sink];
    auto samples = dispatch_queue_create("maxdisplay.capture", DISPATCH_QUEUE_SERIAL);
    NSError *error = nil;
    if (![stream addStreamOutput:sink type:SCStreamOutputTypeScreen sampleHandlerQueue:samples error:&error])
        throw capture_error("Adding capture output", error);

    // Dispatch timers coalesce on the serial capture queue. Refreshes reuse
    // the latest image for both keyframe recovery and static quality updates.
    auto refresh = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, samples);
    sink->refresh_timer = refresh;
    sink->refresh_tick_us = StreamControl::refresh_tick_us(options.fps);
    schedule_refresh(refresh, idle_tick_us);
    __weak MDStreamSink *refresh_sink = sink;
    dispatch_source_set_event_handler(refresh, ^{ [refresh_sink encodeLatestAt:now_us() refreshOnly:true]; });
    dispatch_resume(refresh);
    std::exception_ptr failure;
    StreamControl::Stats previous_stats{};
    auto last_report = Clock::now();
    try {
        change_capture(stream, true);
        std::cout << "Streaming MaxDisplay " << mode.logical_width << 'x' << mode.logical_height
                  << " desktop points as " << mode.stream_width << 'x' << mode.stream_height << " pixels to "
                  << options.host << ':' << options.port << " using "
                  << codec_name(options.codec) << (encoder->hardware_accelerated() ? " (hardware)" : " (software)")
                  << " over "
                  << (options.transport == Transport::udp ? "UDP" : "TCP") << ", "
                  << (options.adaptive && !options.legacy ? "adaptive" : "fixed")
                  << " bitrate " << options.bitrate / 1000 << " kbps, "
                  << (options.adaptive_fps && !options.legacy ? "adaptive" : "fixed")
                  << " frame rate up to " << options.fps << " fps";
        if (options.legacy) std::cout << ", legacy stream";
        else std::cout << ", frame budget " << options.timeout_ms << " ms";
        std::cout << ". Press Ctrl+C to stop." << std::endl;
        while (!interrupted) {
            connection->check();
            const auto now = Clock::now();
            if (options.stats && now - last_report >= 1s) {
                const auto snapshot = connection->snapshot();
                print_stats(snapshot, previous_stats, std::chrono::duration<double>(now - last_report).count());
                previous_stats = snapshot.stats;
                last_report = now;
            }
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
