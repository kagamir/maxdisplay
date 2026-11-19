#include "decoder.hpp"
#include "wire.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << std::setw(8) << std::setfill('0') << static_cast<uint32_t>(hr) << ')';
        throw std::runtime_error(message.str());
    }
}

struct MediaRuntime {
    MediaRuntime() {
        check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
        const HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            CoUninitialize();
            check(hr, "MFStartup (Windows N requires the Media Feature Pack)");
        }
    }

    ~MediaRuntime() {
        MFShutdown();
        CoUninitialize();
    }
};

struct BufferLock {
    explicit BufferLock(IMFMediaBuffer* buffer) : buffer(buffer) {
        check(buffer->Lock(&bytes, nullptr, &length), "IMFMediaBuffer::Lock");
    }
    ~BufferLock() { buffer->Unlock(); }
    IMFMediaBuffer* buffer;
    BYTE* bytes = nullptr;
    DWORD length = 0;
};

ComPtr<IMFSample> make_sample(DWORD size, DWORD alignment) {
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    check(MFCreateSample(&sample), "MFCreateSample");
    // Stream info gives byte alignment. The allocator expects a bit mask.
    check(MFCreateAlignedMemoryBuffer(size, alignment ? alignment - 1 : 0, &buffer),
          "MFCreateAlignedMemoryBuffer");
    check(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer");
    return sample;
}

struct YuvMatrix {
    int y_offset, y_scale, r_v, g_u, g_v, b_u;
};

YuvMatrix color_matrix(IMFMediaType* type, UINT32 height) {
    const auto matrix = MFGetAttributeUINT32(type, MF_MT_YUV_MATRIX,
                                             height >= 720 ? MFVideoTransferMatrix_BT709
                                                           : MFVideoTransferMatrix_BT601);
    const bool full = MFGetAttributeUINT32(type, MF_MT_VIDEO_NOMINAL_RANGE,
                                           MFNominalRange_16_235) == MFNominalRange_0_255;
    if (matrix == MFVideoTransferMatrix_BT601) {
        return full ? YuvMatrix{0, 256, 359, -88, -183, 454}
                    : YuvMatrix{16, 298, 409, -100, -208, 516};
    }
    return full ? YuvMatrix{0, 256, 403, -48, -120, 475}
                : YuvMatrix{16, 298, 459, -55, -136, 541};
}

uint8_t channel(int value) {
    return static_cast<uint8_t>(std::clamp((value + 128) >> 8, 0, 255));
}

} // namespace

struct Decoder::Impl {
    MediaRuntime runtime; // Released after the transform and all of its buffers.
    ComPtr<IMFTransform> transform;
    MFT_INPUT_STREAM_INFO input_info{};
    MFT_OUTPUT_STREAM_INFO output_info{};
    ComPtr<IMFMediaType> nv12;
    UINT32 width, height;
    LONGLONG time = 0;
    LONGLONG duration;

    Impl(UINT32 w, UINT32 h, UINT32 fps) : width(w), height(h), duration(0) {
        if (width < 48 || width > 4096 || height < 48 || height > 2304 ||
            (width | height) % 2 || fps == 0 || fps > 120) {
            throw std::runtime_error("Unsupported H.264 display dimensions or frame rate");
        }
        duration = 10000000LL / fps;
        check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(transform.GetAddressOf())),
              "Load Windows H.264 decoder (Windows N requires the Media Feature Pack)");
        ComPtr<IMFAttributes> attributes;
        check(transform->GetAttributes(&attributes), "Get decoder attributes");
        check(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Enable low-latency decoding");

        ComPtr<IMFMediaType> input;
        check(MFCreateMediaType(&input), "MFCreateMediaType");
        check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video type");
        check(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Set H.264 subtype");
        check(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height), "Set frame size");
        check(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1), "Set frame rate");
        check(MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set pixel aspect");
        // VideoToolbox disables frame reordering. Let the decoder present an IDR
        // without waiting for later pictures in its display-order queue.
        check(input->SetUINT32(MF_MT_VIDEO_NO_FRAME_ORDERING, TRUE), "Set decode order");
        check(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set interlace mode");
        check(transform->SetInputType(0, input.Get(), 0), "Set decoder input type");
        choose_output();
        check(transform->GetInputStreamInfo(0, &input_info), "Get input stream info");
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin streaming");
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start stream");
    }

    void choose_output() {
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> type;
            const HRESULT hr = transform->GetOutputAvailableType(0, i, &type);
            if (hr == MF_E_NO_MORE_TYPES) {
                throw std::runtime_error("Windows H.264 decoder did not offer NV12 output");
            }
            check(hr, "Get decoder output type");
            GUID subtype{};
            check(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Get output subtype");
            if (subtype == MFVideoFormat_NV12) {
                check(transform->SetOutputType(0, type.Get(), 0), "Set NV12 output");
                nv12.Reset();
                check(transform->GetOutputCurrentType(0, &nv12), "Get current NV12 type");
                check(transform->GetOutputStreamInfo(0, &output_info), "Get output stream info");
                return;
            }
        }
    }

    VideoFrame convert(IMFSample* sample) const {
        UINT32 coded_width = 0, coded_height = 0;
        check(MFGetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, &coded_width, &coded_height),
              "Read decoded dimensions");
        if (coded_width < width || coded_width > 4096 || coded_height < height ||
            coded_height > 2304 || (coded_width | coded_height) % 2) {
            throw std::runtime_error("Decoded frame size conflicts with stream header");
        }
        UINT32 left = 0, top = 0;
        MFVideoArea crop{};
        const HRESULT aperture = nv12->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                               reinterpret_cast<UINT8*>(&crop), sizeof(crop), nullptr);
        if (SUCCEEDED(aperture)) {
            if (crop.Area.cx != static_cast<LONG>(width) || crop.Area.cy != static_cast<LONG>(height) ||
                crop.OffsetX.value < 0 || crop.OffsetY.value < 0 || crop.OffsetX.fract || crop.OffsetY.fract) {
                throw std::runtime_error("Invalid decoded display aperture");
            }
            left = static_cast<UINT32>(crop.OffsetX.value);
            top = static_cast<UINT32>(crop.OffsetY.value);
        } else if (aperture != MF_E_ATTRIBUTENOTFOUND) {
            check(aperture, "Read display aperture");
        } else if (coded_width - width > 15 || coded_height - height > 15) {
            throw std::runtime_error("Unexpected coded frame padding");
        }
        if (left > coded_width - width || top > coded_height - height || (left | top) % 2) {
            throw std::runtime_error("Aperture outside the NV12 frame");
        }

        const UINT32 stride = MFGetAttributeUINT32(nv12.Get(), MF_MT_DEFAULT_STRIDE, coded_width);
        const auto bytes_needed = static_cast<uint64_t>(stride) * (coded_height + coded_height / 2);
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "Get contiguous NV12 output");
        BufferLock lock(buffer.Get());
        if (stride < coded_width || bytes_needed > lock.length) {
            throw std::runtime_error("Short NV12 output buffer or invalid stride");
        }

        const auto matrix = color_matrix(nv12.Get(), height);
        VideoFrame frame{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
        const BYTE* uv_plane = lock.bytes + static_cast<size_t>(stride) * coded_height;
        for (UINT32 y = 0; y < height; ++y) {
            const BYTE* luma = lock.bytes + static_cast<size_t>(top + y) * stride + left;
            const BYTE* uv = uv_plane + static_cast<size_t>((top + y) / 2) * stride + left;
            BYTE* pixel = frame.bgra.data() + static_cast<size_t>(y) * width * 4;
            for (UINT32 x = 0; x < width; x += 2) {
                const int u = uv[x] - 128, v = uv[x + 1] - 128;
                const int blue = matrix.b_u * u, green = matrix.g_u * u + matrix.g_v * v;
                const int red = matrix.r_v * v;
                for (UINT32 i = 0; i < 2; ++i, pixel += 4) {
                    const int luminance = matrix.y_scale * (luma[x + i] - matrix.y_offset);
                    pixel[0] = channel(luminance + blue);
                    pixel[1] = channel(luminance + green);
                    pixel[2] = channel(luminance + red);
                    pixel[3] = 255;
                }
            }
        }
        return frame;
    }

    void read_output(const Output& output) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER result{};
            ComPtr<IMFSample> sample;
            if (!(output_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                if (output_info.cbSize == 0 || output_info.cbSize > 64u * 1024 * 1024) {
                    throw std::runtime_error("Invalid decoder output buffer size");
                }
                sample = make_sample(output_info.cbSize, output_info.cbAlignment);
                result.pSample = sample.Get();
            }
            DWORD status = 0;
            const HRESULT hr = transform->ProcessOutput(0, 1, &result, &status);
            if (!sample && result.pSample) sample.Attach(result.pSample);
            if (result.pEvents) result.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                choose_output();
                continue;
            }
            check(hr, "Decode H.264 output");
            if (sample && (result.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) != MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) {
                output(convert(sample.Get()));
            }
        }
    }
};

Decoder::Decoder(uint32_t width, uint32_t height, uint32_t fps)
    : impl_(std::make_unique<Impl>(width, height, fps)) {}

Decoder::~Decoder() = default;

void Decoder::decode(const std::vector<uint8_t>& access_unit, const Output& output) {
    if (access_unit.empty() || access_unit.size() > wire::max_frame_bytes) {
        throw std::runtime_error("Invalid H.264 access unit length");
    }
    const DWORD length = static_cast<DWORD>(access_unit.size());
    const DWORD buffer_size = (std::max)(length, impl_->input_info.cbSize);
    if (buffer_size > wire::max_frame_bytes) {
        throw std::runtime_error("Invalid decoder input buffer size");
    }
    auto sample = make_sample(buffer_size, impl_->input_info.cbAlignment);
    ComPtr<IMFMediaBuffer> buffer;
    check(sample->GetBufferByIndex(0, &buffer), "Get H.264 input buffer");
    {
        BufferLock lock(buffer.Get());
        std::memcpy(lock.bytes, access_unit.data(), length);
    }
    check(buffer->SetCurrentLength(length), "Set H.264 input length");
    check(sample->SetSampleTime(impl_->time), "Set input time");
    check(sample->SetSampleDuration(impl_->duration), "Set input duration");

    HRESULT hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        impl_->read_output(output);
        hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
    }
    check(hr, "Submit H.264 input");
    impl_->time += impl_->duration;
    impl_->read_output(output);
}

void Decoder::finish(const Output& output) {
    check(impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "End stream");
    check(impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Drain H.264 decoder");
    impl_->read_output(output);
    check(impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0), "End streaming");
}
