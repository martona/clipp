#pragma once

// BitmapImage from encoded (PNG/JPEG) bytes, decode-capped at a display width.
// Shared by the settings page's activity rows and the popup's preview pane.
// Include after platform.h / <Windows.h> (needs IStream + the WinRT headers).

#include <shcore.h>

#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/base.h>

#include <cstdint>
#include <vector>

inline winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage BitmapFromImageBytes(
    const std::vector<unsigned char>& bytes,
    int32_t decodePixelWidth)
{
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::UI::Xaml::Media::Imaging;

    BitmapImage bitmap;
    if (bytes.empty()) {
        return bitmap;
    }

    try {
        // Decode at display size (DIPs) so large source images don't sit in the visual tree
        // as full-resolution bitmaps. XAML scales DecodePixelWidth by the current DPI when
        // DecodePixelType is Logical, and preserves aspect ratio when only width is set.
        bitmap.DecodePixelType(DecodePixelType::Logical);
        bitmap.DecodePixelWidth(decodePixelWidth);

        // We need the image bytes inside an IRandomAccessStream that BitmapImage
        // can decode from. The obvious path — DataWriter::WriteBytes +
        // StoreAsync().get() — trips C++/WinRT's STA-blocking-wait assert in
        // Debug builds because .get() on an IAsyncOperation while the caller is
        // on the STA (UI) thread is *technically* a deadlock hazard, even when
        // the underlying op is synchronous-in-practice (in-memory stream).
        //
        // Bypass the async wrapper by writing through the COM IStream interface
        // adapter instead. ISequentialStream::Write is plain synchronous COM;
        // no IAsyncOperation, no assert, no thread hop.
        InMemoryRandomAccessStream stream;
        winrt::com_ptr<IStream> rawStream;
        winrt::check_hresult(::CreateStreamOverRandomAccessStream(
            winrt::get_unknown(stream), IID_PPV_ARGS(rawStream.put())));
        ULONG written = 0;
        winrt::check_hresult(rawStream->Write(
            bytes.data(), static_cast<ULONG>(bytes.size()), &written));
        stream.Seek(0);
        bitmap.SetSource(stream);
    } catch (const winrt::hresult_error&) {
    }

    return bitmap;
}
