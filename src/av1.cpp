#include "av1.hpp"
#include "wire.hpp"

#include <aom/aom_encoder.h>
#include <aom/aom_decoder.h>
#include <aom/aomcx.h>
#include <aom/aomdx.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
unsigned thread_count() {
    return std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
}

struct Context {
    aom_codec_ctx_t codec{};
    bool initialized = false;

    ~Context() { if (initialized) aom_codec_destroy(&codec); }

    void check(aom_codec_err_t status, const char* operation) {
        if (status == AOM_CODEC_OK) return;
        const auto* detail = aom_codec_error_detail(&codec);
        throw std::runtime_error(std::string("AV1 ") + operation + ": " +
                                 aom_codec_err_to_string(status) + (detail ? " (" + std::string(detail) + ")" : ""));
    }
};
} // namespace

struct Av1Encoder::Impl : Context {
    uint32_t width, height;
    aom_codec_enc_cfg_t cfg{};

    Impl(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate) : width(w), height(h) {
        if (!wire::valid_mode(w, h, fps, Codec::av1) || !bitrate)
            throw std::invalid_argument("Invalid AV1 encoder dimensions, frame rate or bitrate");
        check(aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg, AOM_USAGE_REALTIME), "encoder defaults");
        cfg.g_w = w;
        cfg.g_h = h;
        cfg.g_timebase = {1, static_cast<int>(fps)};
        cfg.g_threads = thread_count();
        cfg.g_lag_in_frames = 0;
        cfg.rc_end_usage = AOM_CBR;
        cfg.rc_target_bitrate = std::max(1u, bitrate / 1000);
        cfg.rc_dropframe_thresh = 0;
        cfg.kf_max_dist = fps * 2;
        check(aom_codec_enc_init(&codec, aom_codec_av1_cx(), &cfg, 0), "encoder init");
        initialized = true;
        check(aom_codec_control(&codec, AOME_SET_CPUUSED, 10), "realtime speed");
        check(aom_codec_control(&codec, AV1E_SET_ROW_MT, 1u), "row threading");
        if (width >= 1280)
            check(aom_codec_control(&codec, AV1E_SET_TILE_COLUMNS, 1u), "tile columns");
        check(aom_codec_control(&codec, AV1E_SET_TUNE_CONTENT, AOM_CONTENT_SCREEN), "screen-content tuning");
        check(aom_codec_control(&codec, AV1E_SET_COLOR_PRIMARIES, AOM_CICP_CP_BT_709), "color primaries");
        check(aom_codec_control(&codec, AV1E_SET_TRANSFER_CHARACTERISTICS, AOM_CICP_TC_BT_709), "transfer function");
        check(aom_codec_control(&codec, AV1E_SET_MATRIX_COEFFICIENTS, AOM_CICP_MC_BT_709), "color matrix");
        check(aom_codec_control(&codec, AV1E_SET_COLOR_RANGE, AOM_CR_STUDIO_RANGE), "color range");
    }

    void submit(aom_image_t* image, int64_t pts, const Output& output, bool force_keyframe = false) {
        check(aom_codec_encode(&codec, image, pts, image ? 1 : 0, force_keyframe ? AOM_EFLAG_FORCE_KF : 0),
              "encode frame");
        aom_codec_iter_t iter = nullptr;
        while (const auto* packet = aom_codec_get_cx_data(&codec, &iter)) {
            if (packet->kind != AOM_CODEC_CX_FRAME_PKT) continue;
            const auto& frame = packet->data.frame;
            if (!frame.sz || frame.sz > wire::max_frame_bytes) throw std::runtime_error("Invalid AV1 packet size");
            const auto* data = static_cast<const uint8_t*>(frame.buf);
            output(std::vector<uint8_t>(data, data + frame.sz), (frame.flags & AOM_FRAME_IS_KEY) != 0);
        }
    }
};

Av1Encoder::Av1Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate)
    : impl_(std::make_unique<Impl>(width, height, fps, bitrate)) {}
Av1Encoder::~Av1Encoder() = default;

void Av1Encoder::encode_nv12(uint8_t* y, size_t y_stride, uint8_t* uv, size_t uv_stride,
                             int64_t pts, const Output& output, bool force_keyframe) {
    if (!y || !uv || y_stride < impl_->width || uv_stride < impl_->width ||
        y_stride > std::numeric_limits<int>::max() || uv_stride > std::numeric_limits<int>::max())
        throw std::runtime_error("Invalid NV12 planes for AV1");
    // CoreVideo planes need not be contiguous. Point libaom directly at the
    // captured surfaces during its synchronous, zero-lookahead encode.
    aom_image_t image{};
    if (!aom_img_wrap(&image, AOM_IMG_FMT_NV12, impl_->width, impl_->height, 1, y))
        throw std::runtime_error("Cannot wrap AV1 encoder input");
    image.planes[AOM_PLANE_Y] = y;
    image.planes[AOM_PLANE_U] = uv;
    image.planes[AOM_PLANE_V] = nullptr;
    image.stride[AOM_PLANE_Y] = static_cast<int>(y_stride);
    image.stride[AOM_PLANE_U] = static_cast<int>(uv_stride);
    image.stride[AOM_PLANE_V] = 0;
    impl_->submit(&image, pts, output, force_keyframe);
}

void Av1Encoder::set_bitrate(uint32_t bps) {
    if (!bps) throw std::invalid_argument("AV1 bitrate must be positive");
    auto next = impl_->cfg;
    next.rc_target_bitrate = std::max(1u, bps / 1000);
    if (next.rc_target_bitrate == impl_->cfg.rc_target_bitrate) return;
    impl_->check(aom_codec_enc_config_set(&impl_->codec, &next), "update bitrate");
    impl_->cfg = next;
}

void Av1Encoder::finish(const Output& output) { impl_->submit(nullptr, 0, output); }

struct Av1Decoder::Impl : Context {
    uint32_t width, height;
    bool first = true;

    Impl(uint32_t w, uint32_t h) : width(w), height(h) {
        if (!wire::valid_mode(w, h, 1, Codec::av1))
            throw std::invalid_argument("Invalid AV1 decoder dimensions");
        aom_codec_dec_cfg_t cfg{};
        cfg.threads = thread_count();
        cfg.w = w;
        cfg.h = h;
        cfg.allow_lowbitdepth = 1;
        check(aom_codec_dec_init(&codec, aom_codec_av1_dx(), &cfg, 0), "decoder init");
        initialized = true;
    }

    void submit(const uint8_t* data, size_t size, const Output& output) {
        check(aom_codec_decode(&codec, data, size, nullptr), "decode frame");
        aom_codec_iter_t iter = nullptr;
        while (const auto* image = aom_codec_get_frame(&codec, &iter)) {
            if (image->d_w != width || image->d_h != height || image->fmt != AOM_IMG_FMT_I420 ||
                image->range != AOM_CR_STUDIO_RANGE || image->mc != AOM_CICP_MC_BT_709 ||
                image->monochrome || image->bit_depth != 8)
                throw std::runtime_error("AV1 frame conflicts with negotiated size or BT.709 I420 format");
            output(convert_yuv420(width, height, bt709_video_range, image->planes[0], image->stride[0],
                                  image->planes[1], image->stride[1], image->planes[2], image->stride[2], 1));
        }
    }
};

Av1Decoder::Av1Decoder(uint32_t width, uint32_t height) : impl_(std::make_unique<Impl>(width, height)) {}
Av1Decoder::~Av1Decoder() = default;

void Av1Decoder::decode(const std::vector<uint8_t>& access_unit, const Output& output) {
    if (access_unit.empty() || access_unit.size() > wire::max_frame_bytes)
        throw std::runtime_error("Invalid AV1 access unit length");
    if (impl_->first) {
        aom_codec_stream_info_t info{};
        impl_->check(aom_codec_peek_stream_info(aom_codec_av1_dx(), access_unit.data(), access_unit.size(), &info),
                     "read initial sequence header");
        if (!info.is_kf || info.w != impl_->width || info.h != impl_->height)
            throw std::runtime_error("AV1 sequence header conflicts with the negotiated dimensions");
        impl_->first = false;
    }
    impl_->submit(access_unit.data(), access_unit.size(), output);
}

void Av1Decoder::finish(const Output& output) { impl_->submit(nullptr, 0, output); }

void Av1Decoder::reset() { impl_ = std::make_unique<Impl>(impl_->width, impl_->height); }
