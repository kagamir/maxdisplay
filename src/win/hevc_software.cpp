#include "hevc_software.hpp"
#include "wire.hpp"

#include <libde265/de265.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(de265_error status) {
    if (status != DE265_OK)
        throw std::runtime_error(std::string("HEVC software decoder: ") + de265_get_error_text(status));
}
}

struct HevcSoftwareDecoder::Impl {
    std::unique_ptr<de265_decoder_context, decltype(&de265_free_decoder)> decoder{
        de265_new_decoder(), &de265_free_decoder};
    const uint32_t width, height;

    Impl(uint32_t w, uint32_t h) : width(w), height(h) {
        if (!decoder) throw std::runtime_error("Could not create HEVC software decoder");
        auto limits = *de265_get_security_limits(decoder.get());
        // HEVC may pad both coded dimensions to the next CTU and signal a crop.
        limits.max_image_size_pixels = ((w + 63) & ~63u) * ((h + 63) & ~63u);
        limits.max_NAL_size_bytes = wire::max_frame_bytes;
        de265_set_security_limits(decoder.get(), &limits);
        const auto workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        check(de265_start_worker_threads(decoder.get(), static_cast<int>(workers)));
    }

    VideoFrame pixels(const de265_image* image) const {
        if (de265_get_chroma_format(image) != de265_chroma_420 ||
            de265_get_image_full_range_flag(image) || de265_get_image_matrix_coefficients(image) != 1)
            throw std::runtime_error("HEVC decoder expected video-range BT.709 4:2:0");
        std::array<int, 3> strides{};
        std::array<const uint8_t*, 3> planes{};
        for (int channel = 0; channel < 3; ++channel) {
            planes[channel] = de265_get_image_plane(image, channel, &strides[channel]);
            const int w = static_cast<int>(channel ? width / 2 : width);
            const int h = static_cast<int>(channel ? height / 2 : height);
            if (!planes[channel] || strides[channel] < w || de265_get_bits_per_pixel(image, channel) != 8 ||
                de265_get_image_width(image, channel) != w || de265_get_image_height(image, channel) != h)
                throw std::runtime_error("HEVC decoder output conflicts with negotiated geometry or bit depth");
        }
        return convert_yuv420(width, height, bt709_video_range, planes[0], strides[0],
                              planes[1], strides[1], planes[2], strides[2], 1);
    }

    void drain(const Output& output) {
        int more = 1;
        while (more) {
            const auto status = de265_decode(decoder.get(), &more);
            bool released = false;
            while (const auto* image = de265_peek_next_picture(decoder.get())) {
                auto frame = pixels(image);
                de265_release_next_picture(decoder.get());
                released = true;
                output(std::move(frame));
            }
            if (status == DE265_ERROR_WAITING_FOR_INPUT_DATA) return;
            if (status == DE265_ERROR_IMAGE_BUFFER_FULL && released) continue;
            check(status);
        }
    }
};

HevcSoftwareDecoder::HevcSoftwareDecoder(uint32_t w, uint32_t h)
    : impl_(std::make_unique<Impl>(w, h)) {}
HevcSoftwareDecoder::~HevcSoftwareDecoder() = default;

void HevcSoftwareDecoder::decode(const std::vector<uint8_t>& unit, const Output& output) {
    check(de265_push_data(impl_->decoder.get(), unit.data(), static_cast<int>(unit.size()), 0, nullptr));
    de265_push_end_of_frame(impl_->decoder.get());
    impl_->drain(output);
}

void HevcSoftwareDecoder::finish(const Output& output) {
    check(de265_flush_data(impl_->decoder.get()));
    impl_->drain(output);
}

void HevcSoftwareDecoder::reset() { de265_reset(impl_->decoder.get()); }
