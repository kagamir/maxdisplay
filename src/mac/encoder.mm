#include "encoder.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

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

std::vector<uint8_t> annex_b(CMSampleBufferRef sample) {
    const auto format = CMSampleBufferGetFormatDescription(sample);
    const auto block = CMSampleBufferGetDataBuffer(sample);
    if (!format || !block || CMSampleBufferGetNumSamples(sample) != 1)
        throw std::runtime_error("VideoToolbox returned an invalid H.264 sample");

    size_t parameter_count = 0;
    int length_size = 0;
    check_status(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                     format, 0, nullptr, nullptr, &parameter_count, &length_size),
                 "Read H.264 format");
    if (length_size != 1 && length_size != 2 && length_size != 4)
        throw std::runtime_error("Invalid H.264 NAL length field");

    const size_t total = CMBlockBufferGetDataLength(block);
    if (!total)
        throw std::runtime_error("VideoToolbox returned an empty H.264 sample");
    std::vector<uint8_t> frame;
    frame.reserve(total + 128);
    if (is_sync_sample(sample)) {
        for (size_t i = 0; i < parameter_count; ++i) {
            const uint8_t* data = nullptr;
            size_t size = 0;
            check_status(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                             format, i, &data, &size, nullptr, nullptr),
                         "Read H.264 parameter set");
            if (!data || !size)
                throw std::runtime_error("VideoToolbox returned an empty H.264 parameter set");
            // SPS starts with the NAL header, profile_idc, constraints, and level_idc.
            if ((data[0] & 0x1f) == 7 && (size < 4 || data[3] > 51))
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
            throw std::runtime_error("Truncated H.264 NAL length");
        uint8_t prefix[4]{};
        check_status(CMBlockBufferCopyDataBytes(block, offset, prefix_size, prefix),
                     "Read H.264 NAL length");
        offset += prefix_size;
        size_t payload_size = 0;
        for (size_t i = 0; i < prefix_size; ++i)
            payload_size = (payload_size << 8) | prefix[i];
        if (!payload_size || payload_size > total - offset)
            throw std::runtime_error("Invalid H.264 NAL payload size");
        start_nal(frame);
        const size_t dest = frame.size();
        frame.resize(dest + payload_size);
        check_status(CMBlockBufferCopyDataBytes(block, offset, payload_size, frame.data() + dest),
                     "Copy H.264 NAL payload");
        offset += payload_size;
    }
    return frame;
}

} // namespace

struct Encoder::Impl {
    VTCompressionSessionRef session = nullptr;
    Output output;
    std::mutex error_mutex;
    std::exception_ptr error;
    CMTime duration;

    Impl(Output cb, uint32_t fps)
        : output(std::move(cb)), duration(CMTimeMake(1, static_cast<int32_t>(fps))) {}

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
            check_status(status, "H.264 encoder output");
            if (flags & kVTEncodeInfo_FrameDropped)
                return;
            if (!sample || !CMSampleBufferDataIsReady(sample))
                throw std::runtime_error("VideoToolbox returned no encoded sample");
            self.output(annex_b(sample));
        } catch (...) {
            std::lock_guard<std::mutex> lock(self.error_mutex);
            if (!self.error)
                self.error = std::current_exception();
        }
    }
};

Encoder::Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate, Output output)
    : impl_(std::make_unique<Impl>(std::move(output), fps)) {
    NSDictionary* spec = @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES};
    check_status(VTCompressionSessionCreate(kCFAllocatorDefault, width, height, kCMVideoCodecType_H264,
                                            (__bridge CFDictionaryRef)spec, nullptr, nullptr,
                                            &Impl::encoded, impl_.get(), &impl_->session),
                 "Create VideoToolbox H.264 encoder");
    NSDictionary* properties = @{
        (__bridge NSString*)kVTCompressionPropertyKey_RealTime: @YES,
        (__bridge NSString*)kVTCompressionPropertyKey_AllowFrameReordering: @NO,
        (__bridge NSString*)kVTCompressionPropertyKey_ProfileLevel: (__bridge NSString*)kVTProfileLevel_H264_Baseline_AutoLevel,
        (__bridge NSString*)kVTCompressionPropertyKey_ColorPrimaries: (__bridge NSString*)kCVImageBufferColorPrimaries_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_TransferFunction: (__bridge NSString*)kCVImageBufferTransferFunction_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_YCbCrMatrix: (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_AverageBitRate: @(bitrate),
        (__bridge NSString*)kVTCompressionPropertyKey_ExpectedFrameRate: @(fps),
        (__bridge NSString*)kVTCompressionPropertyKey_MaxKeyFrameInterval: @(fps * 2)
    };
    check_status(VTSessionSetProperties(impl_->session, (__bridge CFDictionaryRef)properties),
                 "Configure VideoToolbox H.264 encoder");
    check_status(VTCompressionSessionPrepareToEncodeFrames(impl_->session), "Prepare H.264 encoder");
}

Encoder::~Encoder() = default;

void Encoder::encode(CVPixelBufferRef pixels, CMTime pts) {
    check_error();
    if (!impl_->session)
        throw std::logic_error("The H.264 compression session is closed");
    if (!pixels || !CMTIME_IS_NUMERIC(pts))
        throw std::runtime_error("Screen capture returned an invalid frame or timestamp");
    check_status(VTCompressionSessionEncodeFrame(impl_->session, pixels, pts, impl_->duration,
                                                 nullptr, nullptr, nullptr),
                 "Submit H.264 frame");
    // A static desktop might never produce another frame to flush this one.
    check_status(VTCompressionSessionCompleteFrames(impl_->session, pts), "Complete H.264 frame");
    check_error();
}

void Encoder::check_error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    if (impl_->error)
        std::rethrow_exception(impl_->error);
}

void Encoder::finish() {
    OSStatus status = impl_->close();
    check_error();
    check_status(status, "Finish H.264 encoder");
}
