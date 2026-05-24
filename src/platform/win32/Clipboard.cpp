#include "Clipboard.h"
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <thread>
#include <future>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include "ClipboardHashGuard.h"
#include "Logger.h"
#include "ScopedTimer.h"

static std::thread g_clipboardThread;
static HWND g_hwnd = nullptr;
static ClipboardHashGuard g_clipboardHashGuard;

#define CLIPBOARD_DEBOUNCE_TIMER_ID 1
#define CLIPBOARD_DEBOUNCE_INTERVAL_MS 250

static ClipboardCallback g_clipboardCallback = nullptr;

struct DelayedClipboardRenderState {
    std::mutex mutex;
    std::shared_ptr<const ClipboardPayload> payload;
};

static DelayedClipboardRenderState g_delayedClipboardRenderState;

#ifndef BI_ALPHABITFIELDS
#define BI_ALPHABITFIELDS 6L
#endif

static bool IsPngStream(const std::vector<unsigned char>& data) {
    static constexpr unsigned char signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return data.size() >= sizeof(signature) && std::memcmp(data.data(), signature, sizeof(signature)) == 0;
}

static bool CheckedMulSize(size_t a, size_t b, size_t& result) {
    if (a != 0 && b > (std::numeric_limits<size_t>::max)() / a) return false;
    result = a * b;
    return true;
}

static void LogLastError(const char* function, const wchar_t* message) {
    g_logger.log(function, Logger::Level::Warning, L"%ls (GetLastError=%lu)", message, GetLastError());
}

static void LogHResult(const char* function, const wchar_t* message, HRESULT hr) {
    g_logger.log(function, Logger::Level::Warning, L"%ls (HRESULT=0x%08lx)", message, static_cast<unsigned long>(hr));
}

class ScopedComInitializer {
public:
    ScopedComInitializer()
        : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        if (hr_ == RPC_E_CHANGED_MODE) {
            ok_ = true;
            return;
        }

        ok_ = SUCCEEDED(hr_);
        uninitialize_ = ok_;
    }

    ~ScopedComInitializer() {
        if (uninitialize_) {
            CoUninitialize();
        }
    }

    bool Ok() const {
        return ok_;
    }

    HRESULT Result() const {
        return hr_;
    }

private:
    HRESULT hr_;
    bool ok_{ false };
    bool uninitialize_{ false };
};

static UINT ClippOriginClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"ClippOriginMarker");
    return format;
}

static bool SetClippOriginClipboardMarker() {
    const UINT format = ClippOriginClipboardFormat();
    if (format == 0) {
        LogLastError(__FUNCTION__, L"Failed to register Clipp clipboard origin format");
        return false;
    }

    static constexpr uint32_t marker = 0x50504c43u; // "CLPP" little-endian
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(marker));
    if (!hMem) {
        LogLastError(__FUNCTION__, L"Failed to allocate Clipp clipboard origin marker");
        return false;
    }

    void* dst = GlobalLock(hMem);
    if (!dst) {
        LogLastError(__FUNCTION__, L"Failed to lock Clipp clipboard origin marker");
        GlobalFree(hMem);
        return false;
    }

    std::memcpy(dst, &marker, sizeof(marker));
    GlobalUnlock(hMem);

    if (!::SetClipboardData(format, hMem)) {
        LogLastError(__FUNCTION__, L"Failed to set Clipp clipboard origin marker");
        GlobalFree(hMem);
        return false;
    }

    return true;
}

static bool ClipboardHasClippOriginMarker() {
    const UINT format = ClippOriginClipboardFormat();
    if (format == 0) {
        return false;
    }

    return IsClipboardFormatAvailable(format) != FALSE;
}

static unsigned char ScaleMaskComponent(uint32_t pixel, uint32_t mask) {
    if (mask == 0) return 0;

    unsigned int shift = 0;
    while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;

    unsigned int bits = 0;
    uint32_t shiftedMask = mask >> shift;
    while (bits < 32u && (shiftedMask & (uint32_t{ 1 } << bits)) != 0u) ++bits;

    const uint32_t value = (pixel & mask) >> shift;
    const uint32_t maxValue = (bits >= 32u) ? 0xffffffffu : ((uint32_t{ 1 } << bits) - 1u);
    if (maxValue == 0) return 0;
    return static_cast<unsigned char>((value * 255u + (maxValue / 2u)) / maxValue);
}

static uint32_t ReadLe16(const unsigned char* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8);
}

static uint32_t ReadLe32(const unsigned char* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

static bool IsStandard32BppBgra(uint32_t redMask, uint32_t greenMask, uint32_t blueMask, uint32_t alphaMask) {
    return redMask == 0x00ff0000u
        && greenMask == 0x0000ff00u
        && blueMask == 0x000000ffu
        && (alphaMask == 0u || alphaMask == 0xff000000u);
}

static void ConvertBgraToRgbaRow(const unsigned char* src, unsigned char* dst, size_t width, bool preserveAlpha) {
    if (preserveAlpha) {
        for (size_t x = 0; x < width; ++x) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
            src += 4;
            dst += 4;
        }
    }
    else {
        for (size_t x = 0; x < width; ++x) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
            src += 4;
            dst += 4;
        }
    }
}

static size_t DIBColorTableEntries(const BITMAPINFOHEADER& header) {
    if (header.biClrUsed != 0) return header.biClrUsed;
    if (header.biBitCount <= 8) return size_t{ 1 } << header.biBitCount;
    return 0;
}

static bool WicEncodePngFromRgba(
    const std::vector<unsigned char>& rgba,
    size_t width,
    size_t height,
    std::vector<unsigned char>& pngData) {
    pngData.clear();
    if (width == 0 || height == 0 ||
        width > (std::numeric_limits<UINT>::max)() ||
        height > (std::numeric_limits<UINT>::max)()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard image with WIC: invalid dimensions (%zu x %zu)", width, height);
        return false;
    }

    size_t rowStride = 0;
    size_t bufferBytes = 0;
    if (!CheckedMulSize(width, 4u, rowStride) ||
        !CheckedMulSize(rowStride, height, bufferBytes) ||
        rowStride > (std::numeric_limits<UINT>::max)() ||
        bufferBytes > (std::numeric_limits<UINT>::max)()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard image with WIC: RGBA buffer size is too large (%zu x %zu)", width, height);
        return false;
    }
    if (rgba.size() != bufferBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard image with WIC: RGBA size mismatch (expected=%zu, actual=%zu)", bufferBytes, rgba.size());
        return false;
    }

    ScopedComInitializer com;
    if (!com.Ok()) {
        LogHResult(__FUNCTION__, L"Cannot initialize COM for WIC PNG encoding", com.Result());
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC imaging factory for PNG encoding", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        GUID_WICPixelFormat32bppRGBA,
        static_cast<UINT>(rowStride),
        static_cast<UINT>(bufferBytes),
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(rgba.data())),
        bitmap.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to wrap RGBA clipboard image for WIC PNG encoding", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC PNG output stream", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC PNG encoder", hr);
        return false;
    }

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to initialize WIC PNG encoder", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC PNG frame", hr);
        return false;
    }

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to initialize WIC PNG frame", hr);
        return false;
    }

    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to set WIC PNG frame size", hr);
        return false;
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppRGBA;
    hr = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to set WIC PNG pixel format", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> source;
    if (IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppRGBA)) {
        hr = bitmap.As(&source);
        if (FAILED(hr)) {
            LogHResult(__FUNCTION__, L"Failed to use RGBA bitmap as WIC PNG source", hr);
            return false;
        }
    }
    else {
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr)) {
            LogHResult(__FUNCTION__, L"Failed to create WIC PNG format converter", hr);
            return false;
        }

        hr = converter->Initialize(
            bitmap.Get(),
            pixelFormat,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            LogHResult(__FUNCTION__, L"Failed to initialize WIC PNG format converter", hr);
            return false;
        }

        hr = converter.As(&source);
        if (FAILED(hr)) {
            LogHResult(__FUNCTION__, L"Failed to use converted bitmap as WIC PNG source", hr);
            return false;
        }
    }

    hr = frame->WriteSource(source.Get(), nullptr);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to write WIC PNG frame pixels", hr);
        return false;
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to commit WIC PNG frame", hr);
        return false;
    }

    hr = encoder->Commit();
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to commit WIC PNG encoder", hr);
        return false;
    }

    HGLOBAL encodedGlobal = nullptr;
    hr = GetHGlobalFromStream(stream.Get(), &encodedGlobal);
    if (FAILED(hr) || encodedGlobal == nullptr) {
        LogHResult(__FUNCTION__, L"Failed to read WIC PNG output stream", FAILED(hr) ? hr : E_POINTER);
        return false;
    }

    const SIZE_T encodedBytes = GlobalSize(encodedGlobal);
    if (encodedBytes == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"WIC PNG encoder produced an empty stream");
        return false;
    }

    const void* encodedData = GlobalLock(encodedGlobal);
    if (!encodedData) {
        LogLastError(__FUNCTION__, L"Failed to lock WIC PNG output stream");
        return false;
    }

    const auto* bytes = static_cast<const unsigned char*>(encodedData);
    pngData.assign(bytes, bytes + encodedBytes);
    GlobalUnlock(encodedGlobal);

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"WIC encoded clipboard image as PNG (%zu x %zu, PNG: %zu bytes)", width, height, pngData.size());
    return true;
}

static bool WicDecodePngToBgra(
    const std::vector<unsigned char>& pngData,
    std::vector<unsigned char>& bgra,
    UINT& width,
    UINT& height) {
    bgra.clear();
    width = 0;
    height = 0;

    if (pngData.empty() || pngData.size() > (std::numeric_limits<DWORD>::max)()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard PNG with WIC: invalid payload size (%zu bytes)", pngData.size());
        return false;
    }

    ScopedComInitializer com;
    if (!com.Ok()) {
        LogHResult(__FUNCTION__, L"Cannot initialize COM for WIC PNG decoding", com.Result());
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC imaging factory for PNG decoding", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC PNG input stream", hr);
        return false;
    }

    hr = stream->InitializeFromMemory(
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(pngData.data())),
        static_cast<DWORD>(pngData.size()));
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to initialize WIC PNG input stream", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(
        stream.Get(),
        nullptr,
        WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC PNG decoder", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to read WIC PNG frame", hr);
        return false;
    }

    hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to read WIC PNG dimensions", hr);
        return false;
    }
    if (width == 0 || height == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard PNG with WIC: decoded image has invalid dimensions (%u x %u)", width, height);
        return false;
    }

    size_t rowStride = 0;
    size_t pixelBytes = 0;
    if (!CheckedMulSize(width, 4u, rowStride) ||
        !CheckedMulSize(rowStride, height, pixelBytes) ||
        rowStride > (std::numeric_limits<UINT>::max)() ||
        pixelBytes > (std::numeric_limits<UINT>::max)()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard PNG with WIC: BGRA buffer size is too large (%u x %u)", width, height);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC PNG decode format converter", hr);
        return false;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to initialize WIC PNG decode format converter", hr);
        return false;
    }

    bgra.assign(pixelBytes, 0);
    hr = converter->CopyPixels(
        nullptr,
        static_cast<UINT>(rowStride),
        static_cast<UINT>(pixelBytes),
        bgra.data());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to copy WIC PNG pixels as BGRA", hr);
        bgra.clear();
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"WIC decoded clipboard PNG as BGRA (%u x %u, PNG: %zu bytes, pixels: %zu bytes)", width, height, pngData.size(), bgra.size());
    return true;
}

static bool DIBToPNG(const unsigned char* dibData, size_t dibSize, std::vector<unsigned char>& pngData) {
    pngData.clear();
    if (dibSize < sizeof(BITMAPINFOHEADER)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: DIB is too small (%zu bytes)", dibSize);
        return false;
    }

    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(dibData);
    if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biSize > dibSize) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: invalid header size %lu for %zu byte DIB", header->biSize, dibSize);
        return false;
    }
    if (header->biPlanes != 1 || header->biWidth <= 0 || header->biHeight == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: unsupported geometry/planes (width=%ld, height=%ld, planes=%u)", header->biWidth, header->biHeight, header->biPlanes);
        return false;
    }

    const uint16_t bitCount = header->biBitCount;
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: unsupported bit depth %u", bitCount);
        return false;
    }

    const bool bitfields = header->biCompression == BI_BITFIELDS || header->biCompression == BI_ALPHABITFIELDS;
    if (header->biCompression != BI_RGB && !bitfields) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: unsupported compression %lu", header->biCompression);
        return false;
    }

    const size_t width = static_cast<size_t>(header->biWidth);
    const size_t height = header->biHeight < 0
        ? static_cast<size_t>(-static_cast<int64_t>(header->biHeight))
        : static_cast<size_t>(header->biHeight);
    const bool bottomUp = header->biHeight > 0;

    size_t rowBits = 0;
    if (!CheckedMulSize(width, bitCount, rowBits)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: row bit count overflow (width=%zu, bit depth=%u)", width, bitCount);
        return false;
    }
    size_t rowStride = ((rowBits + 31u) / 32u) * 4u;
    size_t pixelBytes = 0;
    if (!CheckedMulSize(rowStride, height, pixelBytes)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: pixel data size overflow (width=%zu, height=%zu, bit depth=%u)", width, height, bitCount);
        return false;
    }

    size_t masksOffset = header->biSize;
    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    uint32_t alphaMask = 0;

    if (bitfields) {
        if (header->biSize >= sizeof(BITMAPV4HEADER)) {
            const auto* v4 = reinterpret_cast<const BITMAPV4HEADER*>(dibData);
            redMask = v4->bV4RedMask;
            greenMask = v4->bV4GreenMask;
            blueMask = v4->bV4BlueMask;
            alphaMask = v4->bV4AlphaMask;
        }
        else {
            const size_t maskBytes = header->biCompression == BI_ALPHABITFIELDS ? 16u : 12u;
            if (masksOffset > dibSize || dibSize - masksOffset < maskBytes) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: missing bitfield masks");
                return false;
            }
            redMask = ReadLe32(dibData + masksOffset);
            greenMask = ReadLe32(dibData + masksOffset + 4);
            blueMask = ReadLe32(dibData + masksOffset + 8);
            if (maskBytes == 16u) alphaMask = ReadLe32(dibData + masksOffset + 12);
            masksOffset += maskBytes;
        }
    }
    else if (bitCount == 16) {
        redMask = 0x7c00;
        greenMask = 0x03e0;
        blueMask = 0x001f;
    }
    else if (bitCount == 32) {
        redMask = 0x00ff0000;
        greenMask = 0x0000ff00;
        blueMask = 0x000000ff;
        alphaMask = 0xff000000;
    }

    const size_t paletteOffset = masksOffset;
    const size_t paletteEntries = DIBColorTableEntries(*header);
    size_t paletteBytes = 0;
    if (!CheckedMulSize(paletteEntries, sizeof(RGBQUAD), paletteBytes)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: palette size overflow (%zu entries)", paletteEntries);
        return false;
    }
    if (paletteOffset > dibSize || dibSize - paletteOffset < paletteBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: DIB palette is truncated (entries=%zu)", paletteEntries);
        return false;
    }

    const unsigned char* palette = dibData + paletteOffset;
    const size_t pixelOffset = paletteOffset + paletteBytes;
    if (pixelOffset > dibSize || dibSize - pixelOffset < pixelBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: DIB pixel data is truncated (needed=%zu bytes)", pixelBytes);
        return false;
    }

    size_t rgbaBytes = 0;
    if (!CheckedMulSize(width, height, rgbaBytes) || !CheckedMulSize(rgbaBytes, 4u, rgbaBytes)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: RGBA buffer size overflow (width=%zu, height=%zu)", width, height);
        return false;
    }
    std::vector<unsigned char> rgba(rgbaBytes);

    const unsigned char* pixels = dibData + pixelOffset;
    const bool standard32BppBgra = bitCount == 32 && IsStandard32BppBgra(redMask, greenMask, blueMask, alphaMask);
    const bool preserveAlpha = header->biCompression == BI_ALPHABITFIELDS && alphaMask != 0;

    if (standard32BppBgra) {
        for (size_t y = 0; y < height; ++y) {
            const size_t srcY = bottomUp ? (height - 1u - y) : y;
            const unsigned char* srcRow = pixels + srcY * rowStride;
            unsigned char* dst = rgba.data() + (y * width * 4u);
            ConvertBgraToRgbaRow(srcRow, dst, width, preserveAlpha);
        }
    }
    else {
        for (size_t y = 0; y < height; ++y) {
            const size_t srcY = bottomUp ? (height - 1u - y) : y;
            const unsigned char* srcRow = pixels + srcY * rowStride;
            unsigned char* dst = rgba.data() + (y * width * 4u);

            for (size_t x = 0; x < width; ++x) {
                unsigned char r = 0;
                unsigned char g = 0;
                unsigned char b = 0;
                unsigned char a = 255;

                if (bitCount == 24) {
                    const unsigned char* px = srcRow + x * 3u;
                    b = px[0];
                    g = px[1];
                    r = px[2];
                }
                else if (bitCount == 32 || bitCount == 16) {
                    const uint32_t pixel = bitCount == 32 ? ReadLe32(srcRow + x * 4u) : ReadLe16(srcRow + x * 2u);
                    r = ScaleMaskComponent(pixel, redMask);
                    g = ScaleMaskComponent(pixel, greenMask);
                    b = ScaleMaskComponent(pixel, blueMask);
                    if (preserveAlpha) a = ScaleMaskComponent(pixel, alphaMask);
                } else {
                    uint32_t index = 0;
                    if (bitCount == 8) {
                        index = srcRow[x];
                    }
                    else if (bitCount == 4) {
                        const unsigned char packed = srcRow[x / 2u];
                        index = (x % 2u == 0) ? (packed >> 4) : (packed & 0x0f);
                    }
                    else {
                        const unsigned char packed = srcRow[x / 8u];
                        index = (packed >> (7u - (x % 8u))) & 0x01;
                    }

                    if (index >= paletteEntries) {
                        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: palette index %u exceeds %zu entries", index, paletteEntries);
                        return false;
                    }
                    const unsigned char* entry = palette + index * sizeof(RGBQUAD);
                    b = entry[0];
                    g = entry[1];
                    r = entry[2];
                }

                dst[x * 4u + 0u] = r;
                dst[x * 4u + 1u] = g;
                dst[x * 4u + 2u] = b;
                dst[x * 4u + 3u] = a;
            }
        }
    }

    {
        ScopedTimer timer(L"Clipboard DIB to PNG compression (WIC)");
        if (!WicEncodePngFromRgba(rgba, width, height, pngData)) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to encode DIB clipboard image as PNG with WIC");
            return false;
        }
    }

    if (!IsPngStream(pngData)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Encoded clipboard image did not produce a valid PNG stream");
        return false;
    }

    return true;
}

static bool PNGToDIB(const std::vector<unsigned char>& pngData, std::vector<unsigned char>& dibData) {
    dibData.clear();
    if (!IsPngStream(pngData)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image payload: payload is not a PNG stream (%zu bytes)", pngData.size());
        return false;
    }

    std::vector<unsigned char> bgra;
    UINT width = 0;
    UINT height = 0;
    {
        ScopedTimer timer(L"Clipboard PNG to BGRA decode (WIC)");
        if (!WicDecodePngToBgra(pngData, bgra, width, height)) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to decode PNG clipboard image with WIC");
            return false;
        }
    }

    size_t rowBytes = 0;
    size_t pixelBytes = 0;
    if (!CheckedMulSize(width, 4u, rowBytes) || !CheckedMulSize(rowBytes, height, pixelBytes)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode PNG clipboard image: BGRA buffer size overflow (%u x %u)", width, height);
        return false;
    }

    if (width == 0 || height == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode PNG clipboard image: decoded image has invalid dimensions (%u x %u)", width, height);
        return false;
    }
    if (width > static_cast<unsigned>((std::numeric_limits<LONG>::max)()) ||
        height > static_cast<unsigned>((std::numeric_limits<LONG>::max)())) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode PNG clipboard image: dimensions are too large for DIB (%u x %u)", width, height);
        return false;
    }

    if (bgra.size() != pixelBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode PNG clipboard image: decoded BGRA size mismatch (expected=%zu, actual=%zu)", pixelBytes, bgra.size());
        return false;
    }

    const size_t headerBytes = sizeof(BITMAPV5HEADER);
    size_t dibBytes = 0;
    if (pixelBytes > (std::numeric_limits<size_t>::max)() - headerBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode PNG clipboard image: DIB buffer size overflow (%u x %u)", width, height);
        return false;
    }
    dibBytes = headerBytes + pixelBytes;

    dibData.assign(dibBytes, 0);
    auto* header = reinterpret_cast<BITMAPV5HEADER*>(dibData.data());
    header->bV5Size = sizeof(BITMAPV5HEADER);
    header->bV5Width = static_cast<LONG>(width);
    header->bV5Height = -static_cast<LONG>(height);
    header->bV5Planes = 1;
    header->bV5BitCount = 32;
    header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
    header->bV5RedMask = 0x00ff0000;
    header->bV5GreenMask = 0x0000ff00;
    header->bV5BlueMask = 0x000000ff;
    header->bV5AlphaMask = 0xff000000;
    header->bV5CSType = LCS_sRGB;

    unsigned char* pixels = dibData.data() + headerBytes;
    std::memcpy(pixels, bgra.data(), bgra.size());

    return true;
}

static void ClearDelayedClipboardRenderState() {
    std::lock_guard<std::mutex> lock(g_delayedClipboardRenderState.mutex);
    if (g_delayedClipboardRenderState.payload) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Clearing delayed clipboard render payload.");
    }
    g_delayedClipboardRenderState.payload.reset();
}

static void SetDelayedClipboardRenderState(std::shared_ptr<const ClipboardPayload> payload) {
    const size_t payloadBytes = payload ? payload->rawData.size() : 0;
    std::lock_guard<std::mutex> lock(g_delayedClipboardRenderState.mutex);
    g_delayedClipboardRenderState.payload = std::move(payload);
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Stored delayed clipboard render payload reference (encoded bytes: %zu).", payloadBytes);
}

static std::shared_ptr<const ClipboardPayload> DelayedClipboardRenderPayload() {
    std::lock_guard<std::mutex> lock(g_delayedClipboardRenderState.mutex);
    return g_delayedClipboardRenderState.payload;
}

static std::shared_ptr<const ClipboardPayload> MakeDelayedClipboardRenderPayload(
    const ClipboardPayload& payload,
    std::shared_ptr<const ClipboardPayload> delayedRenderPayloadReference) {
    if (delayedRenderPayloadReference) {
        return delayedRenderPayloadReference;
    }

    ClipboardPayload stored = payload;
    if (!stored.isCompressed) {
        if (!stored.ZstdCompress()) {
            return nullptr;
        }
    }
    return std::make_shared<const ClipboardPayload>(std::move(stored));
}

static HGLOBAL CreateClipboardGlobalMemory(const std::vector<unsigned char>& data, const wchar_t* description) {
    if (data.empty()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot allocate empty clipboard data for %ls", description);
        return nullptr;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, data.size());
    if (!hMem) {
        LogLastError(__FUNCTION__, L"Failed to allocate delayed clipboard render buffer");
        return nullptr;
    }

    void* dst = GlobalLock(hMem);
    if (!dst) {
        LogLastError(__FUNCTION__, L"Failed to lock delayed clipboard render buffer");
        GlobalFree(hMem);
        return nullptr;
    }

    std::memcpy(dst, data.data(), data.size());
    GlobalUnlock(hMem);
    return hMem;
}

static bool RenderDelayedClipboardFormat(UINT format) {
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Delayed clipboard render requested for format %u.", format);

    if (format != CF_DIB) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring delayed clipboard render request for unsupported format %u.", format);
        return false;
    }

    auto renderPayload = DelayedClipboardRenderPayload();
    if (!renderPayload) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring delayed CF_DIB render request because no payload is retained.");
        return false;
    }

    ClipboardPayload payload = *renderPayload;
    if (payload.formatId != CF_DIB) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring delayed CF_DIB render request because retained payload format is %u.", payload.formatId);
        return false;
    }

    if (!payload.ZstdDecompress()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Delayed clipboard PNG payload could not be decompressed.");
        return false;
    }

    std::vector<unsigned char> dibData;
    {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Rendering delayed CF_DIB from PNG payload (%zu bytes).", payload.rawData.size());
        ScopedTimer timer(L"Delayed clipboard PNG to CF_DIB rendering");
        if (!PNGToDIB(payload.rawData, dibData)) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to render delayed PNG clipboard payload as CF_DIB.");
            return false;
        }
    }

    HGLOBAL hMem = CreateClipboardGlobalMemory(dibData, L"CF_DIB");
    if (!hMem) {
        return false;
    }

    if (!::SetClipboardData(CF_DIB, hMem)) {
        LogLastError(__FUNCTION__, L"Failed to set delayed CF_DIB clipboard data");
        GlobalFree(hMem);
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Rendered delayed CF_DIB clipboard data from PNG payload (PNG: %zu bytes, DIB: %zu bytes)", payload.rawData.size(), dibData.size());
    return true;
}

static void RenderAllDelayedClipboardFormats(HWND hwnd) {
    if (!DelayedClipboardRenderPayload()) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring WM_RENDERALLFORMATS because no delayed payload is retained.");
        return;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Rendering all delayed clipboard formats before clipboard owner shutdown.");
    if (!OpenClipboard(hwnd)) {
        LogLastError(__FUNCTION__, L"Failed to open clipboard while rendering all delayed formats");
        return;
    }

    RenderDelayedClipboardFormat(CF_DIB);
    CloseClipboard();
    ClearDelayedClipboardRenderState();
}

LRESULT CALLBACK ClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // Register for clipboard update notifications
        AddClipboardFormatListener(hwnd);
        return 0;
    case WM_RENDERFORMAT:
        RenderDelayedClipboardFormat(static_cast<UINT>(wParam));
        return 0;
    case WM_RENDERALLFORMATS:
        RenderAllDelayedClipboardFormats(hwnd);
        return 0;
    case WM_DESTROYCLIPBOARD:
        ClearDelayedClipboardRenderState();
        return 0;
    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_CLIPBOARDUPDATE:
        // Debounce: set (or reset) a one-shot timer for clipboard processing
        SetTimer(hwnd, CLIPBOARD_DEBOUNCE_TIMER_ID, CLIPBOARD_DEBOUNCE_INTERVAL_MS, NULL);
        return 0;
    case WM_TIMER:
        if (wParam == CLIPBOARD_DEBOUNCE_TIMER_ID) {
            KillTimer(hwnd, CLIPBOARD_DEBOUNCE_TIMER_ID);
            if (g_clipboardCallback) g_clipboardCallback(hwnd);
        }
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

HWND CreateClipboardNotificationWindow(ClipboardCallback cb) {
    g_clipboardCallback = cb;
    const wchar_t CLASS_NAME[] = L"ClipboardNotificationWindow";
    WNDCLASS wc = {};
    wc.lpfnWndProc = ClipboardWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);
    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, L"", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    // Hide the window
    ShowWindow(hwnd, SW_HIDE);
    return hwnd;
}

static void ClipboardThreadProc(std::promise<bool> initPromise, ClipboardCallback callback) {
    g_hwnd = CreateClipboardNotificationWindow(callback);
    if (!g_hwnd) {
        initPromise.set_value(false);
        return;
    }
    initPromise.set_value(true);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ClearDelayedClipboardRenderState();
	g_hwnd = nullptr;
}

bool StartClipboardNotification(ClipboardCallback callback) {
    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();
    g_clipboardThread = std::thread(ClipboardThreadProc, std::move(initPromise), callback);
    if (!initFuture.get()) {
        if (g_clipboardThread.joinable())
            g_clipboardThread.join();
        return false;
    }
    return true;
}

void StopClipboardNotification() {
    if (g_hwnd)
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
    if (g_clipboardThread.joinable())
        g_clipboardThread.join();
}

ClipboardPayload ReadClipboardData(HWND hwnd) {
    ClipboardPayload payload{};
    payload.formatId = 0; // 0 indicates empty/unsupported

    bool opened = false;
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(hwnd)) {
            opened = true;
            if (ClipboardHasClippOriginMarker()) {
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring clipboard notification for Clipp-originated clipboard contents.");
                CloseClipboard();
                break;
            }

            // 1. Try to read Text
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    const wchar_t* utf16Str = static_cast<const wchar_t*>(GlobalLock(hData));
                    if (utf16Str) {
                        // Calculate required buffer size for UTF-8 (including null terminator)
                        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, utf16Str, -1, nullptr, 0, nullptr, nullptr);
                        if (utf8Size > 0) {
                            payload.formatId = CF_UNICODETEXT;
                            payload.rawData.resize(utf8Size);
                            // Perform the actual conversion straight into the vector
                            if (WideCharToMultiByte(CP_UTF8, 0, utf16Str, -1,
                                reinterpret_cast<char*>(payload.rawData.data()), utf8Size, nullptr, nullptr) > 0) {
                                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read CF_UNICODETEXT from system clipboard (UTF-8 payload: %zu bytes)", payload.rawData.size());
                            }
                            else {
                                payload.formatId = 0;
                                payload.rawData.clear();
                                LogLastError(__FUNCTION__, L"Failed to convert CF_UNICODETEXT clipboard data to UTF-8");
                            }
                        }
                        else {
                            LogLastError(__FUNCTION__, L"Failed to measure CF_UNICODETEXT clipboard data as UTF-8");
                        }
                        GlobalUnlock(hData);
                    }
                    else {
                        LogLastError(__FUNCTION__, L"Failed to lock CF_UNICODETEXT clipboard data");
                    }
                }
                else {
                    LogLastError(__FUNCTION__, L"Failed to retrieve CF_UNICODETEXT clipboard data");
                }
            }
            // 2. Try to read an Image (if text isn't available)
            else if (IsClipboardFormatAvailable(CF_DIB)) {
                HANDLE hData = GetClipboardData(CF_DIB);
                if (hData) {
                    const unsigned char* dibData = static_cast<const unsigned char*>(GlobalLock(hData));
                    if (dibData) {
                        // GlobalSize tells us exactly how many bytes the DIB takes up in memory.
                        // Encode the local DIB as PNG before placing it into the network payload.
                        SIZE_T dataSize = GlobalSize(hData);
                        if (dataSize > 0) {
                            std::vector<unsigned char> pngData;
							ScopedTimer timer(L"Clipboard DIB to PNG encoding");
                            if (DIBToPNG(dibData, static_cast<size_t>(dataSize), pngData)) {
                                payload.formatId = CF_DIB;
                                payload.rawData = std::move(pngData);
                                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read CF_DIB from system clipboard and encoded PNG payload (DIB: %zu bytes, PNG: %zu bytes)", static_cast<size_t>(dataSize), payload.rawData.size());
                            } else {
                                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Failed to encode CF_DIB clipboard image as PNG; skipping image payload");
                            }
                        } else {
                            g_logger.log(__FUNCTION__, Logger::Level::Debug, L"CF_DIB clipboard data has zero byte GlobalSize; skipping image payload");
                        }
                        GlobalUnlock(hData);
                    } else {
                        LogLastError(__FUNCTION__, L"Failed to lock CF_DIB clipboard data");
                    }
                } else {
                    LogLastError(__FUNCTION__, L"Failed to retrieve CF_DIB clipboard data");
                }
            } else {
                g_logger.log(__FUNCTION__, Logger::Level::Error, L"No supported clipboard format available");
            }
            CloseClipboard();
            break;
        }
        LogLastError(__FUNCTION__, L"OpenClipboard failed while reading; retrying");
        // Yield and wait.
        Sleep(10 + (i * 10));
    }

    if (!opened) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to open system clipboard for reading after retries");
    }

    if (payload.formatId != 0) {
        if (!g_clipboardHashGuard.AcceptCurrent(payload)) {
            g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring clipboard notification for already-current clipboard contents.");
            payload.formatId = 0;
            payload.rawData.clear();
        }
    }

    return payload;
}

bool IsClipboardDataCurrent(const ClipboardPayload& payload) {
    return payload.formatId != 0 && g_clipboardHashGuard.IsCurrent(payload);
}

void SetClipboardData(
    ClipboardPayload& payload,
    bool markAsClippOriginated,
    std::shared_ptr<const ClipboardPayload> delayedRenderPayloadReference) {
    if (markAsClippOriginated && g_clipboardHashGuard.IsCurrent(payload)) {
        g_logger.log(__FUNCTION__, Logger::Level::Info, L"Clipboard contents already current; not setting clipboard data");
        return;
    }

    bool opened = false;
    bool wroteClipboard = false;
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(g_hwnd)) {
            opened = true;
            if (!EmptyClipboard()) {
                LogLastError(__FUNCTION__, L"Failed to empty system clipboard before writing; retrying");
                CloseClipboard();
                Sleep(10 + (i * 10));
                continue;
            }
            ClearDelayedClipboardRenderState();

            if (payload.formatId == CF_UNICODETEXT) {
                char* utf8Data = reinterpret_cast<char*>(payload.rawData.data());
                int utf8Bytes = static_cast<int>(payload.rawData.size());
                if (utf8Bytes > 0) utf8Data[utf8Bytes - 1] = '\0';
                int wideChars = MultiByteToWideChar(CP_UTF8, 0, utf8Data, utf8Bytes, nullptr, 0);
                if (wideChars > 0) {
                    const SIZE_T wideBytes = static_cast<SIZE_T>(wideChars) * sizeof(wchar_t);
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideBytes);
                    if (hMem) {
                        wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
                        if (dst) {
                            if (MultiByteToWideChar(CP_UTF8, 0, utf8Data, utf8Bytes, dst, wideChars) > 0) {
                                GlobalUnlock(hMem);
                                if (!::SetClipboardData(CF_UNICODETEXT, hMem)) {
                                    LogLastError(__FUNCTION__, L"Failed to set CF_UNICODETEXT on system clipboard");
                                    GlobalFree(hMem);
                                }
                                else {
                                    wroteClipboard = true;
                                    g_logger.log(__FUNCTION__, Logger::Level::Info, L"Wrote CF_UNICODETEXT to system clipboard (UTF-8 payload: %zu bytes, UTF-16 bytes: %zu)", payload.rawData.size(), static_cast<size_t>(wideBytes));
                                }
                            }
                            else {
                                LogLastError(__FUNCTION__, L"Failed to convert UTF-8 payload to CF_UNICODETEXT");
                                GlobalUnlock(hMem);
                                GlobalFree(hMem);
                            }
                        }
                        else {
                            LogLastError(__FUNCTION__, L"Failed to lock CF_UNICODETEXT output buffer");
                            GlobalFree(hMem);
                        }
                    }
                    else {
                        LogLastError(__FUNCTION__, L"Failed to allocate CF_UNICODETEXT output buffer");
                    }
                }
                else {
                    LogLastError(__FUNCTION__, L"Failed to measure UTF-8 payload as CF_UNICODETEXT");
                }
            }
            else if (payload.formatId == CF_DIB) {
                if (!IsPngStream(payload.rawData)) {
                    g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Refusing to advertise delayed CF_DIB for invalid PNG payload (%zu bytes)", payload.rawData.size());
                }
                else {
                    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Preparing delayed CF_DIB clipboard rendering for PNG payload (%zu bytes).", payload.rawData.size());
                    auto renderPayload = MakeDelayedClipboardRenderPayload(payload, std::move(delayedRenderPayloadReference));
                    if (!renderPayload) {
                        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to retain delayed CF_DIB render payload.");
                    }
                    else {
                        SetDelayedClipboardRenderState(std::move(renderPayload));

                        SetLastError(ERROR_SUCCESS);
                        HANDLE delayedHandle = ::SetClipboardData(CF_DIB, nullptr);
                        const DWORD delayedError = GetLastError();
                        if (delayedHandle != nullptr || delayedError == ERROR_SUCCESS) {
                            wroteClipboard = true;
                            g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Advertised delayed CF_DIB clipboard rendering from PNG payload (PNG: %zu bytes)", payload.rawData.size());
                        }
                        else {
                            ClearDelayedClipboardRenderState();
                            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to advertise delayed CF_DIB clipboard rendering (GetLastError=%lu)", delayedError);
                        }
                    }
                }
            }
            else {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Unsupported clipboard payload format ID %u; nothing written", payload.formatId);
            }

            if (wroteClipboard && markAsClippOriginated && !SetClippOriginClipboardMarker()) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"System clipboard was written without Clipp origin marker.");
            }

            if (wroteClipboard && markAsClippOriginated) {
                g_clipboardHashGuard.RememberCurrent(payload);
            }

            CloseClipboard();
            break;
        }
        LogLastError(__FUNCTION__, L"OpenClipboard failed while writing; retrying");
        Sleep(10 + (i * 10));
    }

    if (!opened) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to open system clipboard for writing after retries");
    }
    else if (!wroteClipboard) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"System clipboard write did not complete (format ID: %u, payload size: %zu bytes)", payload.formatId, payload.rawData.size());
    }
}
