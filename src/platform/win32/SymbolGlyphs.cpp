#include "SymbolGlyphs.h"

#include <windows.h>

#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <robuffer.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

#include <winrt/Windows.Storage.Streams.h>

namespace clipp::win32 {

namespace {

// Oversample: the font is drawn into a kRenderPx square and the XAML Image
// displays it at ~16-18 logical px, so it stays crisp up to high DPI scales.
constexpr UINT kRenderPx = 48;
constexpr wchar_t kFontFamily[] = L"Clipp Symbols";
constexpr wchar_t kResourceName[] = L"CLIPP_SYMBOLS_FONT";

std::wstring Utf16FromCodepoint(char32_t cp) {
    std::wstring s;
    if (cp <= 0xFFFF) {
        s.push_back(static_cast<wchar_t>(cp));
    } else {
        // Astral plane (e.g. Material Design icons at U+F0000+) -> surrogate pair.
        cp -= 0x10000;
        s.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
        s.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    }
    return s;
}

uint32_t ColorU32(winrt::Windows::UI::Color c) {
    return (static_cast<uint32_t>(c.A) << 24) |
           (static_cast<uint32_t>(c.R) << 16) |
           (static_cast<uint32_t>(c.G) << 8) |
           static_cast<uint32_t>(c.B);
}

}  // namespace

struct SymbolGlyphs::Impl {
    winrt::com_ptr<IDWriteFactory5> dwrite;
    winrt::com_ptr<IDWriteInMemoryFontFileLoader> loader;
    winrt::com_ptr<IDWriteFontCollection1> collection;
    winrt::com_ptr<ID2D1Factory> d2d;
    winrt::com_ptr<IWICImagingFactory> wic;
    bool ready = false;
    // Keyed by (codepoint, fill ARGB, halo ARGB, halo-fraction x1000).
    std::map<std::tuple<char32_t, uint32_t, uint32_t, int>,
             winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap> cache;
};

SymbolGlyphs& SymbolGlyphs::Instance() {
    static SymbolGlyphs instance;
    return instance;
}

SymbolGlyphs::SymbolGlyphs() : impl_(std::make_shared<Impl>()) {
    try {
        HMODULE mod = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(mod, kResourceName, RT_RCDATA);
        if (!res) return;
        HGLOBAL handle = LoadResource(mod, res);
        const DWORD size = SizeofResource(mod, res);
        const void* data = handle ? LockResource(handle) : nullptr;
        if (!data || size == 0) return;

        winrt::check_hresult(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
            reinterpret_cast<IUnknown**>(impl_->dwrite.put())));

        winrt::check_hresult(impl_->dwrite->CreateInMemoryFontFileLoader(impl_->loader.put()));
        winrt::check_hresult(impl_->dwrite->RegisterFontFileLoader(impl_->loader.get()));

        winrt::com_ptr<IDWriteFontFile> file;
        winrt::check_hresult(impl_->loader->CreateInMemoryFontFileReference(
            impl_->dwrite.get(), data, size, nullptr, file.put()));

        winrt::com_ptr<IDWriteFontSetBuilder> builder0;
        winrt::check_hresult(impl_->dwrite->CreateFontSetBuilder(builder0.put()));
        auto builder = builder0.as<IDWriteFontSetBuilder1>();
        winrt::check_hresult(builder->AddFontFile(file.get()));

        winrt::com_ptr<IDWriteFontSet> fontSet;
        winrt::check_hresult(builder->CreateFontSet(fontSet.put()));
        winrt::check_hresult(impl_->dwrite->CreateFontCollectionFromFontSet(
            fontSet.get(), impl_->collection.put()));

        winrt::check_hresult(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
            nullptr, impl_->d2d.put_void()));

        winrt::check_hresult(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory), impl_->wic.put_void()));

        impl_->ready = true;
    } catch (...) {
        // Leave ready=false; Glyph() returns null and the row simply omits the icon.
        impl_->ready = false;
    }
}

winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap
SymbolGlyphs::Glyph(char32_t codepoint, winrt::Windows::UI::Color fill,
                    winrt::Windows::UI::Color halo, float haloFrac) {
    using winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap;

    if (codepoint == 0 || !impl_->ready) return nullptr;

    const bool withHalo = (haloFrac > 0.0f && halo.A > 0);
    const auto key = std::make_tuple(codepoint, ColorU32(fill),
                                     withHalo ? ColorU32(halo) : 0u,
                                     withHalo ? static_cast<int>(haloFrac * 1000.0f + 0.5f) : 0);
    if (auto it = impl_->cache.find(key); it != impl_->cache.end()) {
        return it->second;
    }

    try {
        winrt::com_ptr<IWICBitmap> wicBitmap;
        winrt::check_hresult(impl_->wic->CreateBitmap(
            kRenderPx, kRenderPx, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, wicBitmap.put()));

        const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        winrt::com_ptr<ID2D1RenderTarget> rt;
        winrt::check_hresult(impl_->d2d->CreateWicBitmapRenderTarget(
            wicBitmap.get(), rtProps, rt.put()));

        // The halo dilates the glyph outward, so shrink the glyph to leave room.
        const float haloR = withHalo ? haloFrac * static_cast<float>(kRenderPx) : 0.0f;
        const float fontSize = (static_cast<float>(kRenderPx) - 2.0f * haloR) * 0.92f;

        winrt::com_ptr<IDWriteTextFormat> fmt;
        winrt::check_hresult(impl_->dwrite->CreateTextFormat(
            kFontFamily, impl_->collection.get(),
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"", fmt.put()));
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        auto makeBrush = [&](winrt::Windows::UI::Color c) {
            winrt::com_ptr<ID2D1SolidColorBrush> b;
            winrt::check_hresult(rt->CreateSolidColorBrush(
                D2D1::ColorF(c.R / 255.0f, c.G / 255.0f, c.B / 255.0f, c.A / 255.0f), b.put()));
            return b;
        };
        auto fillBrush = makeBrush(fill);
        winrt::com_ptr<ID2D1SolidColorBrush> haloBrush;
        if (withHalo) {
            haloBrush = makeBrush(halo);
        }

        const std::wstring text = Utf16FromCodepoint(codepoint);
        auto drawAt = [&](ID2D1SolidColorBrush* brush, float dx, float dy) {
            const D2D1_RECT_F lr = D2D1::RectF(dx, dy,
                                               static_cast<float>(kRenderPx) + dx,
                                               static_cast<float>(kRenderPx) + dy);
            rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), fmt.get(),
                         lr, brush, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        };

        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        if (withHalo) {
            // Stamp the glyph in the halo color around a ring -> a shape-hugging
            // outline that fakes the bold weight the icon font doesn't have.
            constexpr int kDirs = 16;
            for (int i = 0; i < kDirs; ++i) {
                const float a = static_cast<float>(i) * 6.2831853f / static_cast<float>(kDirs);
                drawAt(haloBrush.get(), haloR * std::cos(a), haloR * std::sin(a));
            }
        }
        drawAt(fillBrush.get(), 0.0f, 0.0f);
        winrt::check_hresult(rt->EndDraw());

        // Copy the premultiplied-BGRA WIC pixels into a WriteableBitmap (same format).
        WriteableBitmap wb(static_cast<int32_t>(kRenderPx), static_cast<int32_t>(kRenderPx));
        WICRect rcLock{ 0, 0, static_cast<INT>(kRenderPx), static_cast<INT>(kRenderPx) };
        winrt::com_ptr<IWICBitmapLock> lock;
        winrt::check_hresult(wicBitmap->Lock(&rcLock, WICBitmapLockRead, lock.put()));
        UINT srcStride = 0;
        UINT srcSize = 0;
        BYTE* src = nullptr;
        winrt::check_hresult(lock->GetStride(&srcStride));
        winrt::check_hresult(lock->GetDataPointer(&srcSize, &src));

        auto buffer = wb.PixelBuffer();
        auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();
        BYTE* dst = nullptr;
        winrt::check_hresult(byteAccess->Buffer(&dst));
        const UINT dstStride = kRenderPx * 4;
        for (UINT y = 0; y < kRenderPx; ++y) {
            std::memcpy(dst + y * dstStride, src + y * srcStride, dstStride);
        }
        lock = nullptr;
        wb.Invalidate();

        impl_->cache.emplace(key, wb);
        return wb;
    } catch (...) {
        return nullptr;
    }
}

}  // namespace clipp::win32
