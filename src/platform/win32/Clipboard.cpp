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
#include "NetworkDefs.h"
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

// Chromium / password-manager convention: a clipboard item that should not be
// retained in any clipboard history (Win+V, cloud sync, third-party managers)
// is written with both of these registered formats present. Chrome writes the
// value 0; we don't care about the value, only the presence.
static UINT CanIncludeInClipboardHistoryFormat() {
    static const UINT format = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    return format;
}

static UINT CanUploadToCloudClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    return format;
}

// De-facto-standard registered format for lossless images. There is no official
// CF_PNG, but every modern producer/consumer agrees on the registered name "PNG"
// (browsers, Office, Snipping Tool, Paint.NET, GIMP, Pinta). It is the ONE image
// format Windows will never synthesize from a DIB, so it must be offered (write)
// and probed (read) explicitly; CF_DIB/CF_DIBV5/CF_BITMAP remain available for
// free via Windows' own DIB-family synthesis.
static UINT PngClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
}

// The MIME spelling of PNG. Some consumers (e.g. GTK-based apps like Pinta) probe
// "image/png" rather than the shorter "PNG"; we offer both so whichever a consumer
// prefers is present. Costs nothing extra -- both are delay-rendered.
static UINT PngMimeClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"image/png");
    return format;
}

// JPEG family. macOS/iOS shares frequently arrive as JPEG with no PNG alternative,
// and we carry those bytes to Windows verbatim. "JFIF" is the registered name
// Windows producers use for JPEG (see the Snipping Tool / Pinta format dumps);
// "image/jpeg" is the cross-platform MIME spelling. Offered only when the payload
// is actually JPEG. We never need to CONSUME these on Windows -- CF_DIBV5 remains
// the universal fallback so a JPEG-origin image still pastes into consumers that
// don't read JPEG (i.e. essentially every native Windows app).
static UINT JfifClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"JFIF");
    return format;
}

static UINT JpegMimeClipboardFormat() {
    static const UINT format = RegisterClipboardFormatW(L"image/jpeg");
    return format;
}

// Defined further down (next to ReadClipboardData); forward-declared here so the
// delayed-render and window-proc paths can resolve a format id to a readable name.
static const wchar_t* ClipboardFormatLabel(UINT format, wchar_t* buf, int bufLen);

// Remote-session clipboard redirectors (RDP and friends) set
// CanIncludeInClipboardHistory and CanUploadToCloudClipboard on every
// redirected clipboard write, regardless of the source app — they treat
// remote-session content as not-for-history by default. Those markers
// therefore aren't a meaningful "the source app marked this private" signal
// when one of these processes owns the clipboard, and trusting them would
// flag every RDP-pasted item as private.
//
// `rdpclip.exe` is the redirector on the RDP server side (when Clipp runs on
// the RDP host); `mstsc.exe` / `msrdc.exe` are the redirectors on the RDP
// client side (when Clipp runs on the local box that RDP'd out). Both
// directions matter.
static bool ClipboardOwnerIsRemoteSessionRedirector() {
    const HWND owner = GetClipboardOwner();
    if (owner == nullptr) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    if (pid == 0) {
        return false;
    }

    const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        return false;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool isRedirector = false;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        const wchar_t* name = base != nullptr ? base + 1 : path;

        static constexpr const wchar_t* kKnownRedirectors[] = {
            L"rdpclip.exe", // Microsoft RDP server-side redirector
            L"mstsc.exe",   // Microsoft Remote Desktop Connection (client)
            L"msrdc.exe",   // Microsoft Remote Desktop (newer client)
        };
        for (const wchar_t* known : kKnownRedirectors) {
            if (_wcsicmp(name, known) == 0) {
                isRedirector = true;
                break;
            }
        }
    }
    CloseHandle(proc);
    return isRedirector;
}

static bool ClipboardSourceMarkedPrivate() {
    // If a remote-session redirector wrote the clipboard, the privacy markers
    // it sets are session-policy, not a source-app signal. Treat as unmarked.
    if (ClipboardOwnerIsRemoteSessionRedirector()) {
        return false;
    }

    const UINT historyFormat = CanIncludeInClipboardHistoryFormat();
    const UINT cloudFormat = CanUploadToCloudClipboardFormat();
    if (historyFormat == 0 || cloudFormat == 0) {
        return false;
    }

    return IsClipboardFormatAvailable(historyFormat) != FALSE
        && IsClipboardFormatAvailable(cloudFormat) != FALSE;
}

static bool WriteClipboardPrivacyMarker(UINT format, const wchar_t* description) {
    if (format == 0) {
        LogLastError(__FUNCTION__, L"Failed to register clipboard privacy format");
        return false;
    }

    static constexpr uint32_t marker = 0u;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(marker));
    if (!hMem) {
        LogLastError(__FUNCTION__, description);
        return false;
    }

    void* dst = GlobalLock(hMem);
    if (!dst) {
        LogLastError(__FUNCTION__, description);
        GlobalFree(hMem);
        return false;
    }

    std::memcpy(dst, &marker, sizeof(marker));
    GlobalUnlock(hMem);

    if (!::SetClipboardData(format, hMem)) {
        LogLastError(__FUNCTION__, description);
        GlobalFree(hMem);
        return false;
    }

    return true;
}

static void SetClipboardSourceMarkedPrivateMarkers() {
    WriteClipboardPrivacyMarker(CanIncludeInClipboardHistoryFormat(),
        L"Failed to write CanIncludeInClipboardHistory clipboard marker");
    WriteClipboardPrivacyMarker(CanUploadToCloudClipboardFormat(),
        L"Failed to write CanUploadToCloudClipboard clipboard marker");
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

static bool WicDecodeImageToBgra(
    const std::vector<unsigned char>& imageData,
    uint32_t formatId,
    std::vector<unsigned char>& bgra,
    UINT& width,
    UINT& height) {
    bgra.clear();
    width = 0;
    height = 0;

    if (imageData.empty() || imageData.size() > (std::numeric_limits<DWORD>::max)()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image with WIC: invalid %ls payload size (%zu bytes)",
            ClippClipboardFormatNameW(formatId),
            imageData.size());
        return false;
    }

    ScopedComInitializer com;
    if (!com.Ok()) {
        LogHResult(__FUNCTION__, L"Cannot initialize COM for WIC image decoding", com.Result());
        return false;
    }

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC imaging factory for image decoding", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC image input stream", hr);
        return false;
    }

    hr = stream->InitializeFromMemory(
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(imageData.data())),
        static_cast<DWORD>(imageData.size()));
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to initialize WIC image input stream", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(
        stream.Get(),
        nullptr,
        WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC image decoder", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to read WIC image frame", hr);
        return false;
    }

    hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to read WIC image dimensions", hr);
        return false;
    }
    if (width == 0 || height == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image with WIC: decoded %ls has invalid dimensions (%u x %u)",
            ClippClipboardFormatNameW(formatId),
            width,
            height);
        return false;
    }

    size_t rowStride = 0;
    size_t pixelBytes = 0;
    if (!CheckedMulSize(width, 4u, rowStride) ||
        !CheckedMulSize(rowStride, height, pixelBytes) ||
        rowStride > (std::numeric_limits<UINT>::max)() ||
        pixelBytes > (std::numeric_limits<UINT>::max)()) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image with WIC: BGRA buffer size is too large (%u x %u)", width, height);
        return false;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to create WIC image decode format converter", hr);
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
        LogHResult(__FUNCTION__, L"Failed to initialize WIC image decode format converter", hr);
        return false;
    }

    bgra.assign(pixelBytes, 0);
    hr = converter->CopyPixels(
        nullptr,
        static_cast<UINT>(rowStride),
        static_cast<UINT>(pixelBytes),
        bgra.data());
    if (FAILED(hr)) {
        LogHResult(__FUNCTION__, L"Failed to copy WIC image pixels as BGRA", hr);
        bgra.clear();
        return false;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"WIC decoded clipboard %ls as BGRA (%u x %u, encoded: %zu bytes, pixels: %zu bytes)",
        ClippClipboardFormatNameW(formatId),
        width,
        height,
        imageData.size(),
        bgra.size());
    return true;
}

static void LogClipboardOwnerDossier(const char* function, const wchar_t* context) {
    const HWND owner = GetClipboardOwner();
    DWORD pid = 0;
    DWORD tid = 0;
    if (owner != nullptr) {
        tid = GetWindowThreadProcessId(owner, &pid);
    }

    wchar_t path[MAX_PATH];
    wcscpy_s(path, L"<unavailable>");
    if (pid != 0) {
        const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc != nullptr) {
            wchar_t buf[MAX_PATH] = {};
            DWORD size = ARRAYSIZE(buf);
            if (QueryFullProcessImageNameW(proc, 0, buf, &size)) {
                wcscpy_s(path, buf);
            }
            CloseHandle(proc);
        }
    }
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* name = slash != nullptr ? slash + 1 : path;

    wchar_t title[256] = {};
    if (owner != nullptr) {
        GetWindowTextW(owner, title, ARRAYSIZE(title));
    }

    g_logger.log(function, Logger::Level::Error,
        L"%ls -- clipboard owner: pid=%lu tid=%lu hwnd=0x%p name=\"%ls\" title=\"%ls\" path=\"%ls\"",
        context, pid, tid, static_cast<void*>(owner), name, title, path);
}

// VirtualQuery the whole span up front because clipboard data is owned by another
// process: the declared size (GlobalSize) is the owner's claim, not a promise the
// memory is actually committed. RDP clipboard redirection, for one, advertises the
// full size and then streams the bytes in, so a read that races the transfer runs
// off the end of the committed pages. Used for both the CF_DIB pixel buffer and the
// "PNG" blob. On a short buffer, *readableBytes receives the readable-prefix length.
static bool ClipboardSpanIsReadable(const void* base, size_t bytes, size_t* readableBytes) {
    const unsigned char* const start = static_cast<const unsigned char*>(base);
    size_t readable = 0;
    while (readable < bytes) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(start + readable, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State != MEM_COMMIT) break;
        if (mbi.Protect & PAGE_GUARD) break;
        switch (mbi.Protect & 0xFFu) {
            case PAGE_READONLY:
            case PAGE_READWRITE:
            case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ:
            case PAGE_EXECUTE_READWRITE:
            case PAGE_EXECUTE_WRITECOPY:
                break;
            default:
                goto done;
        }
        const unsigned char* const regionEnd =
            static_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= start + readable) break;
        readable = static_cast<size_t>(regionEnd - start);
    }
done:
    if (readableBytes != nullptr) {
        *readableBytes = (readable > bytes) ? bytes : readable;
    }
    return readable >= bytes;
}

// Decode a single little-endian uint32 (DWORD) out of a registered clipboard format.
// Returns true and sets *outValue only when the format is present AND backed by at
// least a readable DWORD; returns false (without touching *outValue) otherwise.
//
// Several Windows clipboard formats carry exactly this: a serialized DWORD. Per the
// MS contract (and confirmed in Chromium's clipboard_win.cc), "CanIncludeInClipboard-
// History" and "CanUploadToCloudClipboard" hold 0 = opt OUT, 1 = opt IN. Chrome only
// ever writes 0 (to flag password fields); RDP redirection writes them indiscriminately.
//
// The clipboard must already be open (GetClipboardData requires it). The backing
// HGLOBAL belongs to the clipboard owner -- often another process -- so we VirtualQuery
// the 4-byte span before dereferencing, exactly like the image read paths.
static bool TryReadClipboardUint32(UINT format, uint32_t* outValue) {
    if (format == 0 || outValue == nullptr) {
        return false;
    }
    if (!IsClipboardFormatAvailable(format)) {
        return false;
    }

    HANDLE hData = GetClipboardData(format);
    if (hData == nullptr) {
        LogLastError(__FUNCTION__, L"Failed to retrieve clipboard data for uint32 decode");
        return false;
    }

    const void* raw = GlobalLock(hData);
    if (raw == nullptr) {
        LogLastError(__FUNCTION__, L"Failed to lock clipboard data for uint32 decode");
        return false;
    }

    bool ok = false;
    const SIZE_T dataSize = GlobalSize(hData);
    size_t readable = 0;
    if (dataSize >= sizeof(uint32_t) && ClipboardSpanIsReadable(raw, sizeof(uint32_t), &readable)) {
        uint32_t value = 0;
        std::memcpy(&value, raw, sizeof(value)); // little-endian DWORD on Windows/x86-64
        *outValue = value;
        ok = true;
    }
    else {
        g_logger.log(__FUNCTION__, Logger::Level::Warning,
            L"Clipboard format %u present but not a readable DWORD (declared=%zu bytes, readable=%zu)",
            format, static_cast<size_t>(dataSize), readable);
    }

    GlobalUnlock(hData);
    return ok;
}

// Diagnostic: decode and log the Windows privacy-hint DWORDs so we can see what a
// source actually wrote vs. merely that the format is present. This is the data that
// distinguishes a genuine "exclude me" (value 0, e.g. Chrome password fields) from
// RDP's indiscriminate stamping. DDebug to stay out of normal logs (bump to Debug if
// you want it alongside the "Source app marked clipboard content as private" line).
// Clipboard must already be open.
static void LogClipboardPrivacyHints(const char* function) {
    struct { const wchar_t* name; UINT format; } hints[] = {
        { L"CanIncludeInClipboardHistory", CanIncludeInClipboardHistoryFormat() },
        { L"CanUploadToCloudClipboard",    CanUploadToCloudClipboardFormat() },
    };
    for (const auto& hint : hints) {
        if (hint.format == 0 || !IsClipboardFormatAvailable(hint.format)) {
            continue;
        }
        uint32_t value = 0;
        if (TryReadClipboardUint32(hint.format, &value)) {
            g_logger.log(function, Logger::Level::DDebug,
                L"Privacy hint \"%ls\" present: value=%u (0=opt-out, 1=opt-in)", hint.name, value);
        }
        else {
            g_logger.log(function, Logger::Level::DDebug,
                L"Privacy hint \"%ls\" present but value could not be decoded as a DWORD", hint.name);
        }
    }
}

enum class DibDecodeResult { Ok, Faulted, InvalidData };

struct DibPixelDecode {
    unsigned char* dst;
    const unsigned char* pixels;
    const unsigned char* palette;
    size_t width;
    size_t height;
    size_t rowStride;
    size_t paletteEntries;
    uint32_t redMask;
    uint32_t greenMask;
    uint32_t blueMask;
    uint32_t alphaMask;
    uint16_t bitCount;
    bool bottomUp;
    bool standard32BppBgra;
    bool preserveAlpha;
};

static int DibReadExceptionFilter(DWORD code) {
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

// No locals with destructors may live here: under /EHsc a structured exception
// unwinding through this frame would not run C++ destructors, so keeping the body
// trivially destructible is what makes catching the access violation safe. All
// RAII (rgba, timers, GlobalUnlock, CloseClipboard) stays in the callers and
// unwinds normally via the returned result.
static DibDecodeResult SafeDecodeDibPixels(const DibPixelDecode& d) {
    __try {
        if (d.standard32BppBgra) {
            for (size_t y = 0; y < d.height; ++y) {
                const size_t srcY = d.bottomUp ? (d.height - 1u - y) : y;
                const unsigned char* srcRow = d.pixels + srcY * d.rowStride;
                unsigned char* dst = d.dst + (y * d.width * 4u);
                ConvertBgraToRgbaRow(srcRow, dst, d.width, d.preserveAlpha);
            }
            return DibDecodeResult::Ok;
        }

        for (size_t y = 0; y < d.height; ++y) {
            const size_t srcY = d.bottomUp ? (d.height - 1u - y) : y;
            const unsigned char* srcRow = d.pixels + srcY * d.rowStride;
            unsigned char* dst = d.dst + (y * d.width * 4u);

            for (size_t x = 0; x < d.width; ++x) {
                unsigned char r = 0;
                unsigned char g = 0;
                unsigned char b = 0;
                unsigned char a = 255;

                if (d.bitCount == 24) {
                    const unsigned char* px = srcRow + x * 3u;
                    b = px[0];
                    g = px[1];
                    r = px[2];
                }
                else if (d.bitCount == 32 || d.bitCount == 16) {
                    const uint32_t pixel = d.bitCount == 32 ? ReadLe32(srcRow + x * 4u) : ReadLe16(srcRow + x * 2u);
                    r = ScaleMaskComponent(pixel, d.redMask);
                    g = ScaleMaskComponent(pixel, d.greenMask);
                    b = ScaleMaskComponent(pixel, d.blueMask);
                    if (d.preserveAlpha) a = ScaleMaskComponent(pixel, d.alphaMask);
                } else {
                    uint32_t index = 0;
                    if (d.bitCount == 8) {
                        index = srcRow[x];
                    }
                    else if (d.bitCount == 4) {
                        const unsigned char packed = srcRow[x / 2u];
                        index = (x % 2u == 0) ? (packed >> 4) : (packed & 0x0f);
                    }
                    else {
                        const unsigned char packed = srcRow[x / 8u];
                        index = (packed >> (7u - (x % 8u))) & 0x01;
                    }

                    if (index >= d.paletteEntries) {
                        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot encode clipboard DIB as PNG: palette index %u exceeds %zu entries", index, d.paletteEntries);
                        return DibDecodeResult::InvalidData;
                    }
                    const unsigned char* entry = d.palette + index * sizeof(RGBQUAD);
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
        return DibDecodeResult::Ok;
    }
    __except (DibReadExceptionFilter(GetExceptionCode())) {
        return DibDecodeResult::Faulted;
    }
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

    size_t readablePixelBytes = 0;
    if (!ClipboardSpanIsReadable(pixels, pixelBytes, &readablePixelBytes)) {
        const size_t readableRows = rowStride != 0 ? readablePixelBytes / rowStride : 0;
        g_logger.log(__FUNCTION__, Logger::Level::Error,
            L"Refusing to encode clipboard DIB as PNG: pixel buffer is not fully committed "
            L"(declared %zu bytes / %zu rows, only %zu bytes / %zu rows readable)",
            pixelBytes, height, readablePixelBytes, readableRows);
        LogClipboardOwnerDossier(__FUNCTION__, L"truncated CF_DIB pixel buffer");
        return false;
    }

    const DibPixelDecode decode{
        rgba.data(), pixels, palette,
        width, height, rowStride, paletteEntries,
        redMask, greenMask, blueMask, alphaMask,
        bitCount, bottomUp, standard32BppBgra, preserveAlpha,
    };
    const DibDecodeResult decodeResult = SafeDecodeDibPixels(decode);
    if (decodeResult == DibDecodeResult::Faulted) {
        g_logger.log(__FUNCTION__, Logger::Level::Error,
            L"Access violation reading clipboard DIB pixels (%zu x %zu, %u bpp): "
            L"the clipboard owner's buffer is shorter than it advertised",
            width, height, bitCount);
        LogClipboardOwnerDossier(__FUNCTION__, L"faulting CF_DIB pixel buffer");
        return false;
    }
    if (decodeResult != DibDecodeResult::Ok) {
        return false;
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

static bool EncodedImageToDIB(const std::vector<unsigned char>& imageData, uint32_t formatId, std::vector<unsigned char>& dibData) {
    dibData.clear();

    std::vector<unsigned char> bgra;
    UINT width = 0;
    UINT height = 0;
    {
        ScopedTimer timer(L"Clipboard encoded image to BGRA decode (WIC)");
        if (!WicDecodeImageToBgra(imageData, formatId, bgra, width, height)) {
            g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to decode %ls clipboard image with WIC", ClippClipboardFormatNameW(formatId));
            return false;
        }
    }

    size_t rowBytes = 0;
    size_t pixelBytes = 0;
    if (!CheckedMulSize(width, 4u, rowBytes) || !CheckedMulSize(rowBytes, height, pixelBytes)) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image: BGRA buffer size overflow (%u x %u)", width, height);
        return false;
    }

    if (width == 0 || height == 0) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image: decoded image has invalid dimensions (%u x %u)", width, height);
        return false;
    }
    if (width > static_cast<unsigned>((std::numeric_limits<LONG>::max)()) ||
        height > static_cast<unsigned>((std::numeric_limits<LONG>::max)())) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image: dimensions are too large for DIB (%u x %u)", width, height);
        return false;
    }

    if (bgra.size() != pixelBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image: decoded BGRA size mismatch (expected=%zu, actual=%zu)", pixelBytes, bgra.size());
        return false;
    }

    const size_t headerBytes = sizeof(BITMAPV5HEADER);
    size_t dibBytes = 0;
    if (pixelBytes > (std::numeric_limits<size_t>::max)() - headerBytes) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Cannot decode clipboard image: DIB buffer size overflow (%u x %u)", width, height);
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
    const size_t payloadBytes = payload ? payload->EncodedBytes().size() : 0;
    std::lock_guard<std::mutex> lock(g_delayedClipboardRenderState.mutex);
    g_delayedClipboardRenderState.payload = std::move(payload);
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Stored delayed clipboard render payload reference (encoded bytes: %zu).", payloadBytes);
}

static std::shared_ptr<const ClipboardPayload> DelayedClipboardRenderPayload() {
    std::lock_guard<std::mutex> lock(g_delayedClipboardRenderState.mutex);
    return g_delayedClipboardRenderState.payload;
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
    wchar_t requestedName[256];
    const wchar_t* requestedLabel = ClipboardFormatLabel(format, requestedName, ARRAYSIZE(requestedName));
    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Delayed clipboard render requested for format id=%u (0x%04x) name=\"%ls\".", format, format, requestedLabel);

    auto renderPayload = DelayedClipboardRenderPayload();
    if (!renderPayload) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring delayed render request because no payload is retained.");
        return false;
    }

    const ClipboardPayload& payload = *renderPayload;
    if (!IsClippImageFormat(payload.meta.formatId)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring delayed render request because retained payload format is %ls (%u).",
            ClippClipboardFormatNameW(payload.meta.formatId),
            payload.meta.formatId);
        return false;
    }

    // Images are never zstd-compressed (see SetUncompressedBytes), so EncodedBytes() IS the image bytes.
    const std::vector<unsigned char>& imageBytes = payload.EncodedBytes();

    // Verbatim path: the requested registered format matches the payload's own
    // encoding, so the stored bytes ARE the answer -- no decode, no re-encode.
    const bool requestedPng = (format == PngClipboardFormat() || format == PngMimeClipboardFormat());
    const bool requestedJpeg = (format == JfifClipboardFormat() || format == JpegMimeClipboardFormat());
    const bool payloadIsPng = (payload.meta.formatId == CLIPP_FORMAT_PNG);
    const bool payloadIsJpeg = (payload.meta.formatId == CLIPP_FORMAT_JPEG);

    if ((requestedPng && payloadIsPng) || (requestedJpeg && payloadIsJpeg)) {
        HGLOBAL hMem = CreateClipboardGlobalMemory(imageBytes, requestedLabel);
        if (!hMem) {
            return false;
        }
        if (!::SetClipboardData(format, hMem)) {
            LogLastError(__FUNCTION__, L"Failed to set delayed encoded-image clipboard data");
            GlobalFree(hMem);
            return false;
        }
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Rendered delayed \"%ls\" clipboard data verbatim from %ls payload (%zu bytes)",
            requestedLabel,
            ClippClipboardFormatNameW(payload.meta.formatId),
            imageBytes.size());
        return true;
    }

    // Compatibility path: decode the encoded image into a DIB. Windows synthesizes
    // CF_DIB / CF_BITMAP from CF_DIBV5, so this one branch serves every legacy
    // consumer regardless of whether the payload was PNG or JPEG.
    if (format == CF_DIBV5) {
        std::vector<unsigned char> dibData;
        {
            g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Rendering delayed CF_DIBV5 from %ls payload (%zu bytes).",
                ClippClipboardFormatNameW(payload.meta.formatId),
                imageBytes.size());
            ScopedTimer timer(L"Delayed clipboard image to CF_DIBV5 rendering");
            if (!EncodedImageToDIB(imageBytes, payload.meta.formatId, dibData)) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to render delayed %ls clipboard payload as CF_DIBV5.", ClippClipboardFormatNameW(payload.meta.formatId));
                return false;
            }
        }

        HGLOBAL hMem = CreateClipboardGlobalMemory(dibData, L"CF_DIBV5");
        if (!hMem) {
            return false;
        }
        if (!::SetClipboardData(CF_DIBV5, hMem)) {
            LogLastError(__FUNCTION__, L"Failed to set delayed CF_DIBV5 clipboard data");
            GlobalFree(hMem);
            return false;
        }
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Rendered delayed CF_DIBV5 clipboard data from %ls payload (encoded: %zu bytes, DIB: %zu bytes)",
            ClippClipboardFormatNameW(payload.meta.formatId),
            imageBytes.size(),
            dibData.size());
        return true;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring delayed render request for unexpected format id=%u name=\"%ls\".", format, requestedLabel);
    return false;
}

static void RenderAllDelayedClipboardFormats(HWND hwnd) {
    auto renderPayload = DelayedClipboardRenderPayload();
    if (!renderPayload) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring WM_RENDERALLFORMATS because no delayed payload is retained.");
        return;
    }

    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Rendering all delayed clipboard formats before clipboard owner shutdown.");
    if (!OpenClipboard(hwnd)) {
        LogLastError(__FUNCTION__, L"Failed to open clipboard while rendering all delayed formats");
        return;
    }

    // The owner is going away, so every advertised format must be materialized now.
    // This is the one path where delayed rendering is forced to go eager -- and the
    // only place the per-format memory multiplies -- so it mirrors exactly what the
    // write path advertised for this payload type, then the CF_DIBV5 fallback.
    const uint32_t fmtId = renderPayload->meta.formatId;
    if (fmtId == CLIPP_FORMAT_PNG) {
        RenderDelayedClipboardFormat(PngClipboardFormat());
        RenderDelayedClipboardFormat(PngMimeClipboardFormat());
    }
    else if (fmtId == CLIPP_FORMAT_JPEG) {
        RenderDelayedClipboardFormat(JfifClipboardFormat());
        RenderDelayedClipboardFormat(JpegMimeClipboardFormat());
    }
    RenderDelayedClipboardFormat(CF_DIBV5);

    CloseClipboard();
    ClearDelayedClipboardRenderState();
}

LRESULT CALLBACK ClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // Register for clipboard update notifications
        AddClipboardFormatListener(hwnd);
        return 0;
    case WM_RENDERFORMAT: {
        // A real consumer is pasting and has asked for exactly this format. Log it
        // at INFO -- this is the signal for which clipboard format consumers actually
        // pull from us (note: a consumer wanting CF_DIB/CF_BITMAP shows up here as
        // CF_DIBV5, the synthesis source Windows asks us to render).
        const UINT requested = static_cast<UINT>(wParam);
        wchar_t nameBuf[256];
        const wchar_t* label = ClipboardFormatLabel(requested, nameBuf, ARRAYSIZE(nameBuf));
        g_logger.log(__FUNCTION__, Logger::Level::Info, L"WM_RENDERFORMAT: clipboard consumer requested format id=%u (0x%04x) name=\"%ls\"", requested, requested, label);
        RenderDelayedClipboardFormat(requested);
        return 0;
    }
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

// Resolve a clipboard format id to a human-readable label: the standard CF_*
// constants by name, registered formats via GetClipboardFormatNameW, everything
// else as <unnamed>. `buf` backs the registered-name case; the return value is
// either a static string or `buf`.
static const wchar_t* ClipboardFormatLabel(UINT format, wchar_t* buf, int bufLen) {
    switch (format) {
        case CF_TEXT:          return L"CF_TEXT";
        case CF_BITMAP:        return L"CF_BITMAP";
        case CF_METAFILEPICT:  return L"CF_METAFILEPICT";
        case CF_SYLK:          return L"CF_SYLK";
        case CF_DIF:           return L"CF_DIF";
        case CF_TIFF:          return L"CF_TIFF";
        case CF_OEMTEXT:       return L"CF_OEMTEXT";
        case CF_DIB:           return L"CF_DIB";
        case CF_PALETTE:       return L"CF_PALETTE";
        case CF_PENDATA:       return L"CF_PENDATA";
        case CF_RIFF:          return L"CF_RIFF";
        case CF_WAVE:          return L"CF_WAVE";
        case CF_UNICODETEXT:   return L"CF_UNICODETEXT";
        case CF_ENHMETAFILE:   return L"CF_ENHMETAFILE";
        case CF_HDROP:         return L"CF_HDROP";
        case CF_LOCALE:        return L"CF_LOCALE";
        case CF_DIBV5:         return L"CF_DIBV5";
        default:               break;
    }
    if (buf != nullptr && bufLen > 0) {
        if (GetClipboardFormatNameW(format, buf, bufLen) > 0) {
            return buf;
        }
        buf[0] = L'\0';
    }
    return L"<unnamed>";
}

// Diagnostic: dump every format currently on the clipboard at DDebug so it stays
// out of normal debug logs. Caller must already hold the clipboard open, since
// EnumClipboardFormats requires it. Reads only format ids/names -- never calls
// GetClipboardData -- so it cannot trigger a delayed render or disturb ownership.
static void LogAvailableClipboardFormats(const char* function) {
    UINT format = 0;
    int index = 0;
    while ((format = EnumClipboardFormats(format)) != 0) {
        wchar_t name[256];
        const wchar_t* label = ClipboardFormatLabel(format, name, ARRAYSIZE(name));
        g_logger.log(function, Logger::Level::DDebug, L"Clipboard format [%d]: id=%u (0x%04x) name=\"%ls\"",
            index, format, format, label);
        ++index;
    }
    if (index == 0) {
        const DWORD err = GetLastError();
        g_logger.log(function, Logger::Level::DDebug, L"Clipboard exposes no formats (GetLastError=%lu).", err);
    } else {
        g_logger.log(function, Logger::Level::DDebug, L"Clipboard exposes %d format(s) total.", index);
    }
}

ClipboardPayload ReadClipboardData(HWND hwnd) {
    ClipboardPayload payload{};
    payload.meta.formatId = CLIPP_FORMAT_NONE;
    bool sourceMarkedPrivate = false;
    std::vector<unsigned char> bytes;

    bool opened = false;
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(hwnd)) {
            opened = true;
            LogAvailableClipboardFormats(__FUNCTION__);
            if (ClipboardHasClippOriginMarker()) {
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring clipboard notification for Clipp-originated clipboard contents.");
                CloseClipboard();
                break;
            }

            LogClipboardPrivacyHints(__FUNCTION__);
            sourceMarkedPrivate = ClipboardSourceMarkedPrivate();
            if (sourceMarkedPrivate) {
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Source app marked clipboard content as private (CanIncludeInClipboardHistory + CanUploadToCloudClipboard both present).");
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
                            payload.meta.formatId = CLIPP_FORMAT_UTF8;
                            bytes.resize(utf8Size);
                            // Perform the actual conversion straight into the vector
                            if (WideCharToMultiByte(CP_UTF8, 0, utf16Str, -1,
                                reinterpret_cast<char*>(bytes.data()), utf8Size, nullptr, nullptr) > 0) {
                                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read CF_UNICODETEXT from system clipboard (UTF-8 payload: %zu bytes)", bytes.size());
                            }
                            else {
                                payload.meta.formatId = CLIPP_FORMAT_NONE;
                                bytes.clear();
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
            // 2a. Prefer the registered "PNG" format: it's lossless, preserves
            // alpha, needs no DIB decode, and sidesteps the CF_DIB truncation
            // hazard entirely. Every modern source offers it (browsers, Office,
            // Snipping Tool, Paint.NET, Pinta). The bytes are already a PNG, so
            // they become the payload verbatim.
            else if (PngClipboardFormat() != 0 && IsClipboardFormatAvailable(PngClipboardFormat())) {
                HANDLE hData = GetClipboardData(PngClipboardFormat());
                if (hData) {
                    const unsigned char* pngBytes = static_cast<const unsigned char*>(GlobalLock(hData));
                    if (pngBytes) {
                        const SIZE_T dataSize = GlobalSize(hData);
                        size_t readablePngBytes = 0;
                        if (dataSize > 0 && !ClipboardSpanIsReadable(pngBytes, static_cast<size_t>(dataSize), &readablePngBytes)) {
                            // Same foreign-memory hazard as the CF_DIB path: GlobalSize is
                            // the owner's claim, not a guarantee the pages are committed.
                            // Refuse rather than copy off the end of mapped memory.
                            g_logger.log(__FUNCTION__, Logger::Level::Error,
                                L"Refusing to read \"PNG\" clipboard data: buffer is not fully committed (declared %zu bytes, only %zu readable)",
                                static_cast<size_t>(dataSize), readablePngBytes);
                            LogClipboardOwnerDossier(__FUNCTION__, L"truncated \"PNG\" clipboard buffer");
                        }
                        else if (dataSize > 0) {
                            std::vector<unsigned char> pngData(pngBytes, pngBytes + static_cast<size_t>(dataSize));
                            if (IsPngStream(pngData)) {
                                payload.meta.formatId = CLIPP_FORMAT_PNG;
                                bytes = std::move(pngData);
                                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read \"PNG\" clipboard format from system clipboard (%zu bytes)", static_cast<size_t>(dataSize));
                            } else {
                                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"\"PNG\" clipboard format present but bytes are not a PNG stream; skipping image payload");
                            }
                        } else {
                            g_logger.log(__FUNCTION__, Logger::Level::Debug, L"\"PNG\" clipboard data has zero byte GlobalSize; skipping image payload");
                        }
                        GlobalUnlock(hData);
                    } else {
                        LogLastError(__FUNCTION__, L"Failed to lock \"PNG\" clipboard data");
                    }
                } else {
                    LogLastError(__FUNCTION__, L"Failed to retrieve \"PNG\" clipboard data");
                }
            }
            // 2b. Fall back to CF_DIB (Windows synthesizes it from whatever the
            // source offered). DIBToPNG re-encodes it under the truncation guard.
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
                                payload.meta.formatId = CLIPP_FORMAT_PNG;
                                bytes = std::move(pngData);
                                g_logger.log(__FUNCTION__, Logger::Level::Info, L"Read CF_DIB from system clipboard and encoded PNG payload (DIB: %zu bytes, PNG: %zu bytes)", static_cast<size_t>(dataSize), bytes.size());
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

    if (payload.meta.formatId == CLIPP_FORMAT_NONE) {
        return payload;
    }

    if (!payload.SetUncompressedBytes(std::move(bytes))) {
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to encode clipboard payload; dropping.");
        payload.meta.formatId = CLIPP_FORMAT_NONE;
        return payload;
    }

    if (!g_clipboardHashGuard.AcceptCurrent(payload)) {
        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Ignoring clipboard notification for already-current clipboard contents.");
        payload.meta.formatId = CLIPP_FORMAT_NONE;
        payload.SetEncodedBytes({});
        return payload;
    }

    if (sourceMarkedPrivate) {
        payload.meta.flags |= NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE;
    }

    return payload;
}

bool IsClipboardDataCurrent(const ClipboardPayload& payload) {
    return payload.meta.formatId != CLIPP_FORMAT_NONE && g_clipboardHashGuard.IsCurrent(payload);
}

void SetClipboardData(
    std::shared_ptr<const ClipboardPayload> payload,
    bool markAsClippOriginated) {
    if (!payload) {
        return;
    }

    if (markAsClippOriginated && g_clipboardHashGuard.IsCurrent(*payload)) {
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

            if (payload->meta.formatId == CLIPP_FORMAT_UTF8) {
                // Localized form: the wire is LF-canonical, so this re-expands to CRLF
                // for the Windows clipboard (what native apps expect on paste).
                const std::vector<unsigned char>* utf8 = payload->TryGetLocalizedBytes();
                if (utf8 == nullptr) {
                    g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to obtain plaintext for clipboard text payload; nothing written.");
                } else {
                    const char* utf8Data = reinterpret_cast<const char*>(utf8->data());
                    int utf8Bytes = static_cast<int>(utf8->size());
                    // Trim a trailing NUL if present so WideCharToMultiByte's null-terminating
                    // mode lines up the length we measured below.
                    if (utf8Bytes > 0 && utf8Data[utf8Bytes - 1] == '\0') {
                        --utf8Bytes;
                    }
                    int wideChars = MultiByteToWideChar(CP_UTF8, 0, utf8Data, utf8Bytes, nullptr, 0);
                    if (wideChars > 0) {
                        // +1 for the NUL terminator we'll append below for CF_UNICODETEXT.
                        const SIZE_T wideBytes = static_cast<SIZE_T>(wideChars + 1) * sizeof(wchar_t);
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideBytes);
                        if (hMem) {
                            wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
                            if (dst) {
                                if (MultiByteToWideChar(CP_UTF8, 0, utf8Data, utf8Bytes, dst, wideChars) > 0) {
                                    dst[wideChars] = L'\0';
                                    GlobalUnlock(hMem);
                                    if (!::SetClipboardData(CF_UNICODETEXT, hMem)) {
                                        LogLastError(__FUNCTION__, L"Failed to set CF_UNICODETEXT on system clipboard");
                                        GlobalFree(hMem);
                                    }
                                    else {
                                        wroteClipboard = true;
                                        g_logger.log(__FUNCTION__, Logger::Level::Info, L"Wrote CF_UNICODETEXT to system clipboard (UTF-8 payload: %zu bytes, UTF-16 bytes: %zu)", utf8->size(), static_cast<size_t>(wideBytes));
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
            }
            else if (IsClippImageFormat(payload->meta.formatId)) {
                // Stash the same shared_ptr we were handed for delayed image rendering.
                // No copy, no re-encode — the bytes are already the storage form.
                g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Preparing delayed clipboard image rendering for %ls payload (%zu bytes).",
                    ClippClipboardFormatNameW(payload->meta.formatId),
                    payload->EncodedBytes().size());
                SetDelayedClipboardRenderState(payload);

                // Advertise (delayed) the registered encoded-image formats that match
                // the payload, then CF_DIBV5 as the universal compatibility path:
                //   PNG payload  -> "PNG", "image/png"
                //   JPEG payload -> "JFIF", "image/jpeg"
                //   either       -> CF_DIBV5 (Windows synthesizes CF_DIB / CF_BITMAP)
                // Every format is advertised with a NULL handle: nothing is materialized
                // until a consumer actually requests one (WM_RENDERFORMAT), so a copy
                // that nobody pastes costs zero memory. Encoded-image formats are
                // advertised FIRST so they lead the clipboard's enumeration order; the
                // origin + privacy markers are written afterward.
                UINT encodedFormats[2] = { 0, 0 };
                int encodedFormatCount = 0;
                if (payload->meta.formatId == CLIPP_FORMAT_PNG) {
                    encodedFormats[encodedFormatCount++] = PngClipboardFormat();
                    encodedFormats[encodedFormatCount++] = PngMimeClipboardFormat();
                }
                else if (payload->meta.formatId == CLIPP_FORMAT_JPEG) {
                    encodedFormats[encodedFormatCount++] = JfifClipboardFormat();
                    encodedFormats[encodedFormatCount++] = JpegMimeClipboardFormat();
                }

                bool advertisedAny = false;
                for (int idx = 0; idx < encodedFormatCount; ++idx) {
                    const UINT fmt = encodedFormats[idx];
                    if (fmt == 0) {
                        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Could not register an encoded-image clipboard format for %ls payload; relying on CF_DIBV5.",
                            ClippClipboardFormatNameW(payload->meta.formatId));
                        continue;
                    }
                    SetLastError(ERROR_SUCCESS);
                    HANDLE h = ::SetClipboardData(fmt, nullptr);
                    const DWORD err = GetLastError();
                    if (h != nullptr || err == ERROR_SUCCESS) {
                        advertisedAny = true;
                        g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Advertised delayed clipboard image format id=%u from %ls payload (encoded: %zu bytes)",
                            fmt,
                            ClippClipboardFormatNameW(payload->meta.formatId),
                            payload->EncodedBytes().size());
                    }
                    else {
                        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to advertise delayed clipboard image format id=%u (GetLastError=%lu)", fmt, err);
                    }
                }

                // CF_DIBV5 compatibility path (also delayed). Windows synthesizes
                // CF_DIB / CF_BITMAP from it on demand, even after this process exits,
                // so legacy consumers (Paint, Word) still get a clean classic DIB.
                SetLastError(ERROR_SUCCESS);
                HANDLE delayedHandle = ::SetClipboardData(CF_DIBV5, nullptr);
                const DWORD delayedError = GetLastError();
                if (delayedHandle != nullptr || delayedError == ERROR_SUCCESS) {
                    advertisedAny = true;
                    g_logger.log(__FUNCTION__, Logger::Level::Debug, L"Advertised delayed CF_DIBV5 clipboard rendering from %ls payload (encoded: %zu bytes)",
                        ClippClipboardFormatNameW(payload->meta.formatId),
                        payload->EncodedBytes().size());
                }
                else {
                    g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Failed to advertise delayed CF_DIBV5 clipboard rendering (GetLastError=%lu)", delayedError);
                }

                if (advertisedAny) {
                    wroteClipboard = true;
                }
                else {
                    ClearDelayedClipboardRenderState();
                }
            }
            else {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"Unsupported clipboard payload format %ls (%u); nothing written",
                    ClippClipboardFormatNameW(payload->meta.formatId),
                    payload->meta.formatId);
            }

            if (wroteClipboard && markAsClippOriginated && !SetClippOriginClipboardMarker()) {
                g_logger.log(__FUNCTION__, Logger::Level::Warning, L"System clipboard was written without Clipp origin marker.");
            }

            // Propagate source-marked-private signal to downstream consumers:
            // Win+V history, third-party clipboard managers, and cloud clipboard
            // sync all honor the CanIncludeInClipboardHistory and
            // CanUploadToCloudClipboard registered formats. Mirroring the
            // sender's privacy intent keeps the protection end-to-end.
            if (wroteClipboard
                && (payload->meta.flags & NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE) != 0) {
                SetClipboardSourceMarkedPrivateMarkers();
            }

            if (wroteClipboard && markAsClippOriginated) {
                g_clipboardHashGuard.RememberCurrent(*payload);
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
        g_logger.log(__FUNCTION__, Logger::Level::Warning, L"System clipboard write did not complete (format: %ls, ID: %u, payload size: %zu bytes)",
            ClippClipboardFormatNameW(payload->meta.formatId),
            payload->meta.formatId,
            payload->EncodedBytes().size());
    }
}
