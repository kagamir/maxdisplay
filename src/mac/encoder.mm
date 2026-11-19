#include "encoder.hpp"
#include "av1.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void check_status(OSStatus status, const char* operation) {
    if (status != noErr)
        throw std::runtime_error(std::string(operation) + " failed (OSStatus " +
                                 std::to_string(status) + ")");
}

bool is_sync_sample(CMSampleBufferRef sample) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0)
        return true;
    auto first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    return CFDictionaryGetValue(first, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
}

std::optional<double> luma_psnr(CMSampleBufferRef sample) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140400
    if (@available(macOS 14.4, *)) {
        NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample, false);
        NSDictionary* frame = attachments.firstObject;
        NSDictionary* metrics = frame[(__bridge NSString*)kVTSampleAttachmentKey_QualityMetrics];
        NSNumber* value = metrics[(__bridge NSString*)kVTSampleAttachmentQualityMetricsKey_LumaMeanSquaredError];
        if ([value isKindOfClass:NSNumber.class]) {
            const double mse = value.doubleValue;
            if (mse == 0) return std::numeric_limits<double>::infinity();
            if (mse > 0 && std::isfinite(mse)) return 10 * std::log10(255.0 * 255.0 / mse);
        }
    }
#endif
    return std::nullopt;
}

void start_nal(std::vector<uint8_t>& frame) {
    frame.insert(frame.end(), {0, 0, 0, 1});
}

std::vector<uint8_t> annex_b(CMSampleBufferRef sample, bool keyframe) {
    const auto format = CMSampleBufferGetFormatDescription(sample);
    const auto block = CMSampleBufferGetDataBuffer(sample);
    if (!format || !block || CMSampleBufferGetNumSamples(sample) != 1)
        throw std::runtime_error("VideoToolbox returned an invalid video sample");

    const bool hevc = CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_HEVC;
    const auto get_parameter_set = hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex
                                        : CMVideoFormatDescriptionGetH264ParameterSetAtIndex;
    size_t parameter_count = 0;
    int length_size = 0;
    check_status(get_parameter_set(format, 0, nullptr, nullptr, &parameter_count, &length_size),
                 "Read video format");
    if (length_size != 1 && length_size != 2 && length_size != 4)
        throw std::runtime_error("Invalid NAL length field");

    const size_t total = CMBlockBufferGetDataLength(block);
    if (!total)
        throw std::runtime_error("VideoToolbox returned an empty video sample");
    std::vector<uint8_t> frame;
    frame.reserve(total + 128);
    if (keyframe) {
        // Include the session's current VPS (HEVC), SPS and PPS at every random access point.
        for (size_t i = 0; i < parameter_count; ++i) {
            const uint8_t* data = nullptr;
            size_t size = 0;
            check_status(get_parameter_set(format, i, &data, &size, nullptr, nullptr),
                         "Read video parameter set");
            if (!data || !size)
                throw std::runtime_error("VideoToolbox returned an empty video parameter set");
            // SPS starts with the NAL header, profile_idc, constraints, and level_idc.
            if (!hevc && (data[0] & 0x1f) == 7 && (size < 4 || data[3] > 51))
                throw std::runtime_error("H.264 stream exceeds Windows decoder level 5.1");
            start_nal(frame);
            frame.insert(frame.end(), data, data + size);
        }
    }

    // A CMBlockBuffer can span multiple noncontiguous allocations.
    const auto prefix_size = static_cast<size_t>(length_size);
    size_t offset = 0;
    while (offset < total) {
        if (total - offset < prefix_size)
            throw std::runtime_error("Truncated NAL length");
        uint8_t prefix[4]{};
        check_status(CMBlockBufferCopyDataBytes(block, offset, prefix_size, prefix),
                     "Read NAL length");
        offset += prefix_size;
        size_t payload_size = 0;
        for (size_t i = 0; i < prefix_size; ++i)
            payload_size = (payload_size << 8) | prefix[i];
        if (!payload_size || payload_size > total - offset)
            throw std::runtime_error("Invalid NAL payload size");
        start_nal(frame);
        const size_t dest = frame.size();
        frame.resize(dest + payload_size);
        check_status(CMBlockBufferCopyDataBytes(block, offset, payload_size, frame.data() + dest),
                     "Copy NAL payload");
        offset += payload_size;
    }
    return frame;
}

} // namespace

struct Encoder::Impl {
    VTCompressionSessionRef session = nullptr;
    Output output;
    std::unique_ptr<Av1Encoder> av1;
    uint32_t width, height, fps, bitrate;
    Codec codec;
    bool require_hardware;
    std::atomic<bool> hardware{false};
    bool need_sync = true;
    int64_t last_av1_pts = -1;
    CMTime last_vt_pts = kCMTimeInvalid;
    CMTime duration;
    std::optional<double> psnr;

    // Only one sample is submitted at a time. Publish output to the transport
    // after CompleteFrames succeeds so a failed frame can be retried exactly once.
    struct Frame {
        std::vector<uint8_t> bytes;
        bool keyframe = false;
        std::optional<double> psnr;
    };
    std::mutex result_mutex;
    Frame result;
    std::exception_ptr callback_error;

    Impl(Output cb, uint32_t w, uint32_t h, uint32_t fps, uint32_t bps, Codec codec, bool strict)
        : output(std::move(cb)), width(w), height(h), fps(fps), bitrate(bps), codec(codec),
          require_hardware(strict), duration(CMTimeMake(1, static_cast<int32_t>(fps))) {}

    void close() noexcept {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
    }

    ~Impl() { close(); }

    void open(bool use_hardware);
    void fall_back(const std::runtime_error& cause);
    Frame submit(CVPixelBufferRef pixels, CMTime pts, bool force_keyframe);

    static void encoded(void* context, void*, OSStatus status, VTEncodeInfoFlags flags,
                        CMSampleBufferRef sample) noexcept {
        auto& self = *static_cast<Impl*>(context);
        try {
            check_status(status, "VideoToolbox encoder output");
            if (flags & kVTEncodeInfo_FrameDropped)
                return;
            if (!sample || !CMSampleBufferDataIsReady(sample))
                throw std::runtime_error("VideoToolbox returned no encoded sample");
            const bool keyframe = is_sync_sample(sample);
            auto bytes = annex_b(sample, keyframe);
            std::lock_guard<std::mutex> lock(self.result_mutex);
            self.result = {std::move(bytes), keyframe, luma_psnr(sample)};
        } catch (...) {
            std::lock_guard<std::mutex> lock(self.result_mutex);
            self.callback_error = std::current_exception();
        }
    }
};

void Encoder::Impl::open(bool use_hardware) {
    const bool hevc = codec == Codec::h265;
    const CFStringRef profile = hevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_Baseline_AutoLevel;
    NSDictionary* properties = @{
        (__bridge NSString*)kVTCompressionPropertyKey_RealTime: @YES,
        (__bridge NSString*)kVTCompressionPropertyKey_AllowFrameReordering: @NO,
        (__bridge NSString*)kVTCompressionPropertyKey_ProfileLevel: (__bridge NSString*)profile,
        (__bridge NSString*)kVTCompressionPropertyKey_ColorPrimaries: (__bridge NSString*)kCVImageBufferColorPrimaries_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_TransferFunction: (__bridge NSString*)kCVImageBufferTransferFunction_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_YCbCrMatrix: (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_AverageBitRate: @(bitrate),
        (__bridge NSString*)kVTCompressionPropertyKey_ExpectedFrameRate: @(fps),
        (__bridge NSString*)kVTCompressionPropertyKey_MaxKeyFrameInterval: @(INT32_MAX)
    };
    NSDictionary* spec = use_hardware ?
        @{(__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES} :
        @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO};
    check_status(VTCompressionSessionCreate(kCFAllocatorDefault, width, height,
                                            hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
                                            (__bridge CFDictionaryRef)spec, nullptr, nullptr,
                                            &Impl::encoded, this, &session),
                 hevc ? "Create VideoToolbox HEVC encoder" : "Create VideoToolbox H.264 encoder");
    check_status(VTSessionSetProperties(session, (__bridge CFDictionaryRef)properties),
                 "Configure VideoToolbox encoder");
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, (__bridge CFNumberRef)@0);
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140400
    if (@available(macOS 14.4, *)) {
        // Quality metrics are optional even on systems that export this key.
        VTSessionSetProperty(session, kVTCompressionPropertyKey_CalculateMeanSquaredError, kCFBooleanTrue);
    }
#endif
    // Older encoders may omit this optional property. Closed GOPs make recovery independent.
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
    check_status(VTCompressionSessionPrepareToEncodeFrames(session), "Prepare VideoToolbox encoder");
    CFTypeRef acceleration = nullptr;
    OSStatus status = VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                            kCFAllocatorDefault, &acceleration);
    hardware = status == noErr && acceleration && CFEqual(acceleration, kCFBooleanTrue);
    if (acceleration) CFRelease(acceleration);
    if (use_hardware && !hardware)
        throw std::runtime_error("VideoToolbox did not report a hardware encoder for the requested format");
    if (!use_hardware && hardware)
        throw std::runtime_error("VideoToolbox selected a hardware encoder for a software-only session");
    need_sync = true;
}

void Encoder::Impl::fall_back(const std::runtime_error& cause) {
    if (require_hardware)
        throw std::runtime_error(std::string("Hardware encoding required: ") + cause.what());
    std::cerr << "Warning: " << codec_name(codec) << " hardware encoding failed (" << cause.what()
              << "). Switching to software encoding.\n";
    close();
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        result = {};
        callback_error = nullptr;
    }
    try {
        open(false);
    } catch (const std::runtime_error& software_error) {
        throw std::runtime_error(std::string("Hardware encoding: ") + cause.what() +
                                 ". Software encoding: " + software_error.what());
    }
}

Encoder::Impl::Frame Encoder::Impl::submit(CVPixelBufferRef pixels, CMTime pts, bool force_keyframe) {
    NSDictionary* properties = (need_sync || force_keyframe) ?
        @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
    check_status(VTCompressionSessionEncodeFrame(session, pixels, pts, duration,
                                                 (__bridge CFDictionaryRef)properties, nullptr, nullptr),
                 "Submit VideoToolbox frame");
    // A static desktop might never produce another frame to flush this one.
    check_status(VTCompressionSessionCompleteFrames(session, pts), "Complete VideoToolbox frame");
    std::lock_guard<std::mutex> lock(result_mutex);
    if (callback_error) std::rethrow_exception(callback_error);
    auto frame = std::exchange(result, Frame{});
    if (!frame.bytes.empty()) {
        if (need_sync && !frame.keyframe)
            throw std::runtime_error("New VideoToolbox session did not produce a random access picture");
        need_sync = false;
    }
    return frame;
}

Encoder::Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, Output output,
                 Codec codec, bool require_hardware)
    : impl_(std::make_unique<Impl>(std::move(output), width, height, fps, bitrate, codec, require_hardware)) {
    if (codec == Codec::av1) {
        if (require_hardware) throw std::invalid_argument("AV1 encoding uses software libaom");
        std::cerr << "Warning: AV1 hardware encoding is unavailable in this build. Using software libaom.\n";
        impl_->av1 = std::make_unique<Av1Encoder>(width, height, fps, bitrate);
        return;
    }
    if (codec != Codec::h264 && codec != Codec::h265)
        throw std::invalid_argument("Unsupported video codec");
    try {
        impl_->open(true);
    } catch (const std::runtime_error& error) {
        impl_->fall_back(error);
    }
}

Encoder::~Encoder() = default;

bool Encoder::encode(CVPixelBufferRef pixels, CMTime pts, bool force_keyframe, uint32_t current_fps) {
    if (!pixels || !CMTIME_IS_NUMERIC(pts))
        throw std::runtime_error("Screen capture returned an invalid frame or timestamp");
    if (impl_->av1) {
        if (CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
            CVPixelBufferGetPlaneCount(pixels) != 2 || CVPixelBufferGetWidth(pixels) != impl_->width ||
            CVPixelBufferGetHeight(pixels) != impl_->height)
            throw std::runtime_error("AV1 capture requires video-range NV12 at the configured size");
        check_status(CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly), "Lock AV1 capture buffer");
        struct Unlock {
            CVPixelBufferRef pixels;
            ~Unlock() { CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly); }
        } unlock{pixels};
        const auto timestamp = CMTimeConvertScale(pts, impl_->duration.timescale, kCMTimeRoundingMethod_Default);
        impl_->last_av1_pts = std::max<int64_t>(impl_->last_av1_pts + 1, timestamp.value);
        return impl_->av1->encode_nv12(static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0)),
                                       CVPixelBufferGetBytesPerRowOfPlane(pixels, 0),
                                       static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1)),
                                       CVPixelBufferGetBytesPerRowOfPlane(pixels, 1), impl_->last_av1_pts, impl_->output,
                                       force_keyframe, current_fps);
    }
    if (!impl_->session) throw std::logic_error("The compression session is closed");
    if (CMTIME_IS_VALID(impl_->last_vt_pts) && CMTimeCompare(pts, impl_->last_vt_pts) <= 0)
        pts = CMTimeAdd(impl_->last_vt_pts, impl_->duration);
    impl_->last_vt_pts = pts;
    Impl::Frame frame;
    try {
        frame = impl_->submit(pixels, pts, force_keyframe);
    } catch (const std::runtime_error& error) {
        if (!impl_->hardware) throw;
        impl_->fall_back(error);
        frame = impl_->submit(pixels, pts, true);
    }
    impl_->psnr = frame.psnr;
    if (frame.bytes.empty()) return false;
    impl_->output(std::move(frame.bytes), frame.keyframe);
    return true;
}

std::optional<double> Encoder::last_psnr() const { return impl_->psnr; }

std::optional<int> Encoder::last_quantizer() const {
    if (impl_->av1) return impl_->av1->last_quantizer();
    return std::nullopt;
}

bool Encoder::hardware_accelerated() const { return impl_->hardware; }

void Encoder::set_bitrate(uint32_t bps) {
    if (!bps) throw std::invalid_argument("Encoder bitrate must be positive");
    if (impl_->av1) return impl_->av1->set_bitrate(bps);
    if (!impl_->session) throw std::logic_error("The compression session is closed");
    impl_->bitrate = bps;
    try {
        check_status(VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_AverageBitRate,
                                          (__bridge CFNumberRef)@(bps)), "Update VideoToolbox bitrate");
    } catch (const std::runtime_error& error) {
        if (!impl_->hardware) throw;
        impl_->fall_back(error);
    }
}

void Encoder::finish() {
    if (impl_->av1) {
        impl_->av1->finish(impl_->output);
        impl_->av1.reset();
        return;
    }
    // submit() completed and delivered each input sample before returning.
    impl_->close();
}
