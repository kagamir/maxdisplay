#include "encoder.hpp"
#include "av1.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <exception>
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
    uint32_t width, height;
    bool hardware = false;
    int64_t last_av1_pts = -1;
    CMTime last_vt_pts = kCMTimeInvalid;
    std::mutex error_mutex;
    std::exception_ptr error;
    CMTime duration;

    Impl(Output cb, uint32_t w, uint32_t h, uint32_t fps)
        : output(std::move(cb)), width(w), height(h), duration(CMTimeMake(1, static_cast<int32_t>(fps))) {}

    OSStatus close() noexcept {
        if (!session)
            return noErr;
        OSStatus status = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        session = nullptr;
        return status;
    }

    ~Impl() { close(); }

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
            self.output(annex_b(sample, keyframe), keyframe);
        } catch (...) {
            std::lock_guard<std::mutex> lock(self.error_mutex);
            if (!self.error)
                self.error = std::current_exception();
        }
    }
};

Encoder::Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
                 std::function<void(std::vector<uint8_t>)> output, Codec codec, bool require_hardware)
    : Encoder(width, height, fps, bitrate, [cb = std::move(output)](std::vector<uint8_t> data, bool) {
          cb(std::move(data));
      }, codec, require_hardware) {}

Encoder::Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, Output output,
                 Codec codec, bool require_hardware)
    : impl_(std::make_unique<Impl>(std::move(output), width, height, fps)) {
    if (codec == Codec::av1) {
        if (require_hardware) throw std::invalid_argument("AV1 encoding uses software libaom");
        impl_->av1 = std::make_unique<Av1Encoder>(width, height, fps, bitrate);
        return;
    }
    const bool hevc = codec == Codec::h265;
    if (!hevc && codec != Codec::h264) throw std::invalid_argument("Unsupported video codec");
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
        (__bridge NSString*)kVTCompressionPropertyKey_MaxKeyFrameInterval: @(fps * 2)
    };
    auto open = [&](bool hardware) {
        NSDictionary* spec = hardware ?
            @{(__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES} :
            @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO};
        check_status(VTCompressionSessionCreate(kCFAllocatorDefault, width, height,
                                                hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
                                                (__bridge CFDictionaryRef)spec, nullptr, nullptr,
                                                &Impl::encoded, impl_.get(), &impl_->session),
                     hevc ? "Create VideoToolbox HEVC encoder" : "Create VideoToolbox H.264 encoder");
        check_status(VTSessionSetProperties(impl_->session, (__bridge CFDictionaryRef)properties),
                     "Configure VideoToolbox encoder");
        // Older encoders may omit this optional property. Closed GOPs make recovery independent.
        VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
        check_status(VTCompressionSessionPrepareToEncodeFrames(impl_->session), "Prepare VideoToolbox encoder");
        CFTypeRef acceleration = nullptr;
        OSStatus status = VTSessionCopyProperty(impl_->session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                                kCFAllocatorDefault, &acceleration);
        impl_->hardware = status == noErr && acceleration && CFEqual(acceleration, kCFBooleanTrue);
        if (acceleration) CFRelease(acceleration);
        if (hardware && !impl_->hardware)
            throw std::runtime_error("VideoToolbox did not report a hardware encoder for the requested format");
    };
    try {
        open(true);
    } catch (const std::runtime_error& hw_error) {
        if (require_hardware)
            throw std::runtime_error(std::string("Hardware encoding required: ") + hw_error.what());
        impl_->close();
        try {
            open(false);
        } catch (const std::runtime_error& sw_error) {
            throw std::runtime_error(std::string("Hardware encoding: ") + hw_error.what() +
                                     ". Software encoding: " + sw_error.what());
        }
    }
}

Encoder::~Encoder() = default;

void Encoder::encode(CVPixelBufferRef pixels, CMTime pts, bool force_keyframe) {
    check_error();
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
        impl_->av1->encode_nv12(static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0)),
                                CVPixelBufferGetBytesPerRowOfPlane(pixels, 0),
                                static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1)),
                                CVPixelBufferGetBytesPerRowOfPlane(pixels, 1), impl_->last_av1_pts, impl_->output,
                                force_keyframe);
        return;
    }
    if (!impl_->session) throw std::logic_error("The compression session is closed");
    if (CMTIME_IS_VALID(impl_->last_vt_pts) && CMTimeCompare(pts, impl_->last_vt_pts) <= 0)
        pts = CMTimeAdd(impl_->last_vt_pts, impl_->duration);
    impl_->last_vt_pts = pts;
    NSDictionary* properties = force_keyframe ? @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
    check_status(VTCompressionSessionEncodeFrame(impl_->session, pixels, pts, impl_->duration,
                                                 (__bridge CFDictionaryRef)properties, nullptr, nullptr),
                 "Submit VideoToolbox frame");
    // A static desktop might never produce another frame to flush this one.
    check_status(VTCompressionSessionCompleteFrames(impl_->session, pts), "Complete VideoToolbox frame");
    check_error();
}

bool Encoder::hardware_accelerated() const { return impl_->hardware; }

void Encoder::set_bitrate(uint32_t bps) {
    check_error();
    if (!bps) throw std::invalid_argument("Encoder bitrate must be positive");
    if (impl_->av1) return impl_->av1->set_bitrate(bps);
    if (!impl_->session) throw std::logic_error("The compression session is closed");
    check_status(VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_AverageBitRate,
                                      (__bridge CFNumberRef)@(bps)), "Update VideoToolbox bitrate");
}

void Encoder::check_error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    if (impl_->error)
        std::rethrow_exception(impl_->error);
}

void Encoder::finish() {
    if (impl_->av1) {
        impl_->av1->finish(impl_->output);
        impl_->av1.reset();
        return;
    }
    OSStatus status = impl_->close();
    check_error();
    check_status(status, "Finish VideoToolbox encoder");
}
