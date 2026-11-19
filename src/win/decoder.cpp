#include "decoder.hpp"
#include "av1.hpp"
#include "wire.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
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

YuvMatrix color_matrix(IMFMediaType* type) {
    const auto matrix = MFGetAttributeUINT32(type, MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    const bool full = MFGetAttributeUINT32(type, MF_MT_VIDEO_NOMINAL_RANGE,
                                           MFNominalRange_16_235) == MFNominalRange_0_255;
    if (matrix == MFVideoTransferMatrix_BT601) {
        return full ? YuvMatrix{0, 256, 359, -88, -183, 454}
                    : YuvMatrix{16, 298, 409, -100, -208, 516};
    }
    return full ? YuvMatrix{0, 256, 403, -48, -120, 475}
                : bt709_video_range;
}

struct Activations {
    IMFActivate** entries = nullptr;
    UINT32 count = 0;
    ~Activations() {
        for (UINT32 i = 0; i < count; ++i) entries[i]->Release();
        CoTaskMemFree(entries);
    }
};

ComPtr<IMFTransform> create_decoder(Codec codec, bool prefer_hardware) {
    ComPtr<IMFTransform> transform;
    if (codec == Codec::h264) {
        check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(transform.GetAddressOf())),
              "Load Windows H.264 decoder (Windows N requires the Media Feature Pack)");
        return transform;
    }
    // The Windows HEVC Video Extensions decoder, like the H.264 decoder, has
    // a synchronous IMFTransform API and can use DXVA internally.
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_HEVC};
    Activations matches;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                    MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                    &input, nullptr, &matches.entries, &matches.count), "Find Windows HEVC decoder");
    if (!matches.count) {
        throw HevcDecoderUnavailable("Windows HEVC decoder is unavailable. Install HEVC Video "
                                     "Extensions (and the Media Feature Pack on Windows N).");
    }
    HRESULT hr = E_FAIL;
    ComPtr<IMFTransform> fallback;
    for (UINT32 i = 0; i < matches.count; ++i) {
        hr = matches.entries[i]->ActivateObject(IID_PPV_ARGS(transform.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) continue;
        ComPtr<IMFAttributes> attributes;
        if (!prefer_hardware || (SUCCEEDED(transform->GetAttributes(&attributes)) &&
                                 MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE)))
            return transform;
        if (!fallback) fallback = transform;
    }
    if (fallback) return fallback;
    check(hr, "Activate Windows HEVC decoder");
    return transform;
}

struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    D3D11_MAPPED_SUBRESOURCE planes{};
    MappedTexture(ID3D11DeviceContext* c, ID3D11Texture2D* t) : context(c), texture(t) {
        check(context->Map(texture, 0, D3D11_MAP_READ, 0, &planes), "Map decoded NV12 texture");
    }
    ~MappedTexture() { context->Unmap(texture, 0); }
};

struct GpuDecoderDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<ID3D11Texture2D> staging;
    UINT staging_width = 0, staging_height = 0;

    void attach(IMFTransform* transform, IMFAttributes* attributes, Codec codec, UINT w, UINT h) {
        if (!MFGetAttributeUINT32(attributes, MF_SA_D3D11_AWARE, FALSE))
            throw std::runtime_error("Windows decoder does not support D3D11");
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())), "Find D3D11 graphics adapters");
        const GUID profile = codec == Codec::h265 ? D3D11_DECODER_PROFILE_HEVC_VLD_MAIN
                                                  : D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
        const D3D11_VIDEO_DECODER_DESC decoder_desc{profile, w, h, DXGI_FORMAT_NV12};
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            auto hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                throw std::runtime_error("No graphics adapter supports hardware decoding for this codec and size");
            check(hr, "Enumerate D3D11 graphics adapters");
            DXGI_ADAPTER_DESC1 info{};
            check(adapter->GetDesc1(&info), "Read D3D11 graphics adapter");
            if (info.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                   device.ReleaseAndGetAddressOf(), nullptr, context.ReleaseAndGetAddressOf());
            ComPtr<ID3D11VideoDevice> video;
            UINT configs = 0;
            if (SUCCEEDED(hr) && SUCCEEDED(device.As(&video)) &&
                SUCCEEDED(video->GetVideoDecoderConfigCount(&decoder_desc, &configs)) && configs)
                break;
        }
        ComPtr<ID3D10Multithread> threads;
        check(device.As(&threads), "Get D3D11 multithread protection");
        threads->SetMultithreadProtected(TRUE);
        UINT token = 0;
        check(MFCreateDXGIDeviceManager(&token, &manager), "Create DXGI device manager");
        check(manager->ResetDevice(device.Get(), token), "Set hardware decoding device");
        check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                        reinterpret_cast<ULONG_PTR>(manager.Get())),
              "Attach D3D11 device to Windows decoder");
    }

    ID3D11Texture2D* copy_surface(IMFDXGIBuffer* output, UINT coded_width, UINT coded_height,
                                  bool& decoder_surface) {
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        check(output->GetResource(IID_PPV_ARGS(texture.GetAddressOf())), "Get decoder texture");
        check(output->GetSubresourceIndex(&subresource), "Get decoded picture array index");
        D3D11_TEXTURE2D_DESC src{};
        texture->GetDesc(&src);
        ComPtr<ID3D11Device> owner;
        texture->GetDevice(&owner);
        if (owner.Get() != device.Get() || src.Format != DXGI_FORMAT_NV12 ||
            src.MipLevels != 1 || subresource >= src.ArraySize || src.SampleDesc.Count != 1 ||
            src.Width < coded_width || src.Height < coded_height ||
            src.Width > 4096 + 255 || src.Height > 2304 + 255) {
            throw std::runtime_error("Windows decoder returned an incompatible NV12 texture");
        }
        decoder_surface = (src.BindFlags & D3D11_BIND_DECODER) != 0;
        if (!staging || staging_width != src.Width || staging_height != src.Height) {
            D3D11_TEXTURE2D_DESC dst{};
            dst.Width = src.Width;
            dst.Height = src.Height;
            dst.MipLevels = dst.ArraySize = 1;
            dst.Format = DXGI_FORMAT_NV12;
            dst.SampleDesc.Count = 1;
            dst.Usage = D3D11_USAGE_STAGING;
            dst.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateTexture2D(&dst, nullptr, staging.ReleaseAndGetAddressOf()),
                  "Create staging NV12 texture");
            staging_width = src.Width;
            staging_height = src.Height;
        }
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), subresource, nullptr);
        return staging.Get();
    }
};

} // namespace

struct Decoder::Impl {
    MediaRuntime runtime; // Released after the transform and all of its buffers.
    std::unique_ptr<GpuDecoderDevice> gpu;
    ComPtr<IMFTransform> transform;
    MFT_INPUT_STREAM_INFO input_info{};
    MFT_OUTPUT_STREAM_INFO output_info{};
    ComPtr<IMFMediaType> nv12;
    UINT32 width, height;
    Codec codec;
    bool require_hardware, hardware = false;
    bool reported_backend = false;
    bool discontinuity = true;
    LONGLONG time = 0;
    LONGLONG duration;

    Impl(UINT32 w, UINT32 h, UINT32 fps, Codec format, bool require)
        : width(w), height(h), codec(format), require_hardware(require), duration(0) {
        if (!wire::valid_mode(width, height, fps, codec)) {
            throw std::runtime_error("Unsupported video display dimensions or frame rate");
        }
        duration = 10000000LL / fps;
        transform = create_decoder(codec, true);
        try {
            gpu = std::make_unique<GpuDecoderDevice>();
            ComPtr<IMFAttributes> attributes;
            check(transform->GetAttributes(&attributes), "Get decoder attributes");
            gpu->attach(transform.Get(), attributes.Get(), codec, w, h);
            configure(fps);
        } catch (const std::runtime_error& error) {
            if (require_hardware) throw HardwareDecodeUnavailable(error.what());
            std::cerr << "D3D11 video decoding unavailable (" << error.what()
                      << "). Trying software decoding.\n";
            transform.Reset();
            gpu.reset();
            transform = create_decoder(codec, false);
            configure(fps);
        }
    }

    void configure(UINT32 fps) {
        ComPtr<IMFAttributes> attributes;
        check(transform->GetAttributes(&attributes), "Get decoder attributes");
        check(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "Enable low-latency decoding");

        ComPtr<IMFMediaType> input;
        check(MFCreateMediaType(&input), "MFCreateMediaType");
        check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video type");
        check(input->SetGUID(MF_MT_SUBTYPE, codec == Codec::h265 ? MFVideoFormat_HEVC : MFVideoFormat_H264),
              "Set compressed video subtype");
        if (codec == Codec::h265)
            check(input->SetUINT32(MF_MT_VIDEO_PROFILE, eAVEncH265VProfile_Main_420_8), "Set HEVC Main profile");
        // The sender uses video-range BT.709 even at SD resolutions. Supply the
        // same metadata here for the initial negotiation before the first SPS.
        check(input->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "Set input color matrix");
        check(input->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "Set input color range");
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
                throw std::runtime_error("Windows decoder did not offer NV12 output");
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

    void record_backend(bool accelerated) {
        if (require_hardware && !accelerated)
            throw HardwareDecodeUnavailable("Windows decoder did not output a hardware decoder surface");
        if (!reported_backend || hardware != accelerated) {
            std::clog << codec_name(codec) << " decoder: "
                      << (accelerated ? "D3D11 hardware" : "software") << '\n';
            reported_backend = true;
        }
        hardware = accelerated;
    }

    VideoFrame convert(IMFSample* sample) {
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
        } else if (coded_width - width > (codec == Codec::h265 ? 63u : 15u) ||
                   coded_height - height > (codec == Codec::h265 ? 63u : 15u)) {
            throw std::runtime_error("Unexpected coded frame padding");
        }
        if (left > coded_width - width || top > coded_height - height || (left | top) % 2) {
            throw std::runtime_error("Aperture outside the NV12 frame");
        }

        auto pixels = [&](const BYTE* bytes, UINT stride, UINT surface_height) {
            const BYTE* luma = bytes + static_cast<size_t>(top) * stride + left;
            const BYTE* chroma = bytes + static_cast<size_t>(surface_height + top / 2) * stride + left;
            return convert_yuv420(width, height, color_matrix(nv12.Get()), luma, stride,
                                  chroma, stride, chroma + 1, stride, 2);
        };

        ComPtr<IMFMediaBuffer> buffer;
        check(sample->GetBufferByIndex(0, &buffer), "Get decoded picture buffer");
        ComPtr<IMFDXGIBuffer> dxgi;
        if (gpu && SUCCEEDED(buffer.As(&dxgi))) {
            bool accelerated = false;
            auto* texture = gpu->copy_surface(dxgi.Get(), coded_width, coded_height, accelerated);
            MappedTexture mapped(gpu->context.Get(), texture);
            if (!mapped.planes.pData || mapped.planes.RowPitch < gpu->staging_width)
                throw std::runtime_error("Invalid mapped NV12 row pitch");
            record_backend(accelerated);
            // UV starts after the physical texture height, including padding.
            return pixels(static_cast<const BYTE*>(mapped.planes.pData), mapped.planes.RowPitch,
                          gpu->staging_height);
        }

        record_backend(false);
        const UINT32 stride = MFGetAttributeUINT32(nv12.Get(), MF_MT_DEFAULT_STRIDE, coded_width);
        const auto bytes_needed = static_cast<uint64_t>(stride) * (coded_height + coded_height / 2);
        check(sample->ConvertToContiguousBuffer(buffer.ReleaseAndGetAddressOf()), "Get contiguous NV12 output");
        BufferLock lock(buffer.Get());
        if (stride < coded_width || bytes_needed > lock.length)
            throw std::runtime_error("Short NV12 output buffer or invalid stride");
        return pixels(lock.bytes, stride, coded_height);
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
            check(hr, "Decode video output");
            if (sample && (result.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) != MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) {
                output(convert(sample.Get()));
            }
        }
    }
};

Decoder::Decoder(uint32_t width, uint32_t height, uint32_t fps, Codec codec, bool require_hardware) {
    if (codec == Codec::av1) {
        if (require_hardware) throw HardwareDecodeUnavailable("AV1 decoding uses the software decoder");
        av1_ = std::make_unique<Av1Decoder>(width, height);
    } else if (codec == Codec::h264 || codec == Codec::h265)
        impl_ = std::make_unique<Impl>(width, height, fps, codec, require_hardware);
    else
        throw std::invalid_argument("Unsupported video codec");
}

Decoder::~Decoder() = default;

bool Decoder::hardware_accelerated() const { return impl_ && impl_->hardware; }

void Decoder::decode(const std::vector<uint8_t>& access_unit, const Output& output) {
    if (av1_) return av1_->decode(access_unit, output);
    if (access_unit.empty() || access_unit.size() > wire::max_frame_bytes) {
        throw std::runtime_error("Invalid video access unit length");
    }
    const DWORD length = static_cast<DWORD>(access_unit.size());
    const DWORD buffer_size = (std::max)(length, impl_->input_info.cbSize);
    if (buffer_size > wire::max_frame_bytes) {
        throw std::runtime_error("Invalid decoder input buffer size");
    }
    auto sample = make_sample(buffer_size, impl_->input_info.cbAlignment);
    ComPtr<IMFMediaBuffer> buffer;
    check(sample->GetBufferByIndex(0, &buffer), "Get video input buffer");
    {
        BufferLock lock(buffer.Get());
        std::memcpy(lock.bytes, access_unit.data(), length);
    }
    check(buffer->SetCurrentLength(length), "Set video input length");
    if (impl_->discontinuity) {
        check(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE), "Mark decoder discontinuity");
    }
    check(sample->SetSampleTime(impl_->time), "Set input time");
    check(sample->SetSampleDuration(impl_->duration), "Set input duration");

    HRESULT hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        impl_->read_output(output);
        hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
    }
    check(hr, "Submit video input");
    impl_->discontinuity = false;
    impl_->time += impl_->duration;
    impl_->read_output(output);
}

void Decoder::reset() {
    if (av1_) return av1_->reset();
    check(impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "Flush video decoder");
    impl_->discontinuity = true;
}

void Decoder::finish(const Output& output) {
    if (av1_) return av1_->finish(output);
    check(impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "End stream");
    check(impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Drain video decoder");
    impl_->read_output(output);
    check(impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0), "End streaming");
}
