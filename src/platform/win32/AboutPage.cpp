#include "AboutPage.h"

#include "resource.h"
#include "platform/uistrings.h"
#include "version.h"

#include <cstdint>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>      // WIC: decode the app .ico into 32bpp BGRA pixels.
#include <wrl/client.h>    // Microsoft::WRL::ComPtr (matches Clipboard.cpp's WIC usage).
#include <robuffer.h>      // IBufferByteAccess — raw write into the WriteableBitmap buffer.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#ifdef FindText
#undef FindText
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/base.h>

namespace {
// Loads the application icon (IDI_CLIPP_ICON, already embedded for the window/tray)
// at the given square size and decodes it into a XAML WriteableBitmap, alpha intact.
//
// Why not just SetSource() a stream like ClippPage's image path? That path decodes a
// compressed PNG/JPEG byte stream; an .ico is a GDI icon resource, not such a stream.
// So we go HICON -> WIC (CreateBitmapFromHICON merges the icon's color + AND-mask into
// straight 32bpp BGRA) -> premultiplied BGRA -> raw pixels copied into a
// WriteableBitmap. This lets us drop the separate ~720KB About PNG and reuse the icon
// the binary already ships. Returns nullptr on any failure (caller leaves the image
// blank).
winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap LoadAppIconBitmap(int pixelSize)
{
    using namespace winrt::Windows::UI::Xaml::Media::Imaging;
    using Microsoft::WRL::ComPtr;

    HINSTANCE const instance = GetModuleHandleW(nullptr);
    // LR_DEFAULTSIZE off + explicit size => WIC/User picks the best-fit frame from the
    // multi-resolution .ico (it has a 256px frame) and scales to pixelSize.
    HICON const icon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CLIPP_ICON), IMAGE_ICON, pixelSize, pixelSize, LR_DEFAULTCOLOR));
    if (!icon) {
        return nullptr;
    }

    WriteableBitmap result{ nullptr };
    try {
        ComPtr<IWICImagingFactory> factory;
        winrt::check_hresult(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));

        ComPtr<IWICBitmap> wicBitmap;
        // Pulls in the color bitmap AND the transparency mask, producing 32bpp BGRA.
        winrt::check_hresult(factory->CreateBitmapFromHICON(icon, &wicBitmap));

        // XAML WriteableBitmap expects premultiplied BGRA (PBGRA); convert to be safe.
        ComPtr<IWICFormatConverter> converter;
        winrt::check_hresult(factory->CreateFormatConverter(&converter));
        winrt::check_hresult(converter->Initialize(
            wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));

        UINT w = 0, h = 0;
        winrt::check_hresult(wicBitmap->GetSize(&w, &h));
        if (w == 0 || h == 0) {
            DestroyIcon(icon);
            return nullptr;
        }

        result = WriteableBitmap(static_cast<int32_t>(w), static_cast<int32_t>(h));
        winrt::Windows::Storage::Streams::IBuffer pixelBuffer = result.PixelBuffer();

        // Reach the raw bytes behind the IBuffer via IBufferByteAccess (the documented
        // way to fill a WriteableBitmap from native code).
        ComPtr<Windows::Storage::Streams::IBufferByteAccess> byteAccess;
        winrt::check_hresult(reinterpret_cast<::IUnknown*>(winrt::get_abi(pixelBuffer))
            ->QueryInterface(IID_PPV_ARGS(&byteAccess)));
        BYTE* dest = nullptr;
        winrt::check_hresult(byteAccess->Buffer(&dest));

        const UINT stride = w * 4;
        const UINT sizeBytes = stride * h;
        winrt::check_hresult(converter->CopyPixels(nullptr, stride, sizeBytes, dest));
    } catch (const winrt::hresult_error&) {
        DestroyIcon(icon);
        return nullptr;
    }

    DestroyIcon(icon);
    return result;
}

winrt::Windows::UI::Xaml::Controls::TextBlock CreateTextBlock(
    const wchar_t* text,
    double fontSize,
    double opacity = 1.0,
    bool semiBold = false)
{
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    TextBlock block;
    block.Text(text);
    block.FontSize(fontSize);
    block.TextWrapping(TextWrapping::WrapWholeWords);
    block.Opacity(opacity);
    if (semiBold) {
        block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    }
    return block;
}

winrt::Windows::UI::Xaml::Controls::Grid CreateArtworkView()
{
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;
    using namespace winrt::Windows::UI::Xaml::Media::Imaging;

    Grid view;
    view.HorizontalAlignment(HorizontalAlignment::Left);
    view.VerticalAlignment(VerticalAlignment::Top);

    Image artwork;
    artwork.HorizontalAlignment(HorizontalAlignment::Stretch);
    artwork.VerticalAlignment(VerticalAlignment::Stretch);
    artwork.Stretch(Stretch::Uniform);

    // Decode the app icon at 256px (the .ico's largest frame) and let Stretch::Uniform
    // scale it down to the ~112-160px the band renders at — crisp on HiDPI, and no
    // separate About PNG to embed.
    if (auto bitmap = LoadAppIconBitmap(256)) {
        artwork.Source(bitmap);
    }

    view.Children().Append(artwork);
    return view;
}

double ContentHeight(winrt::Windows::UI::Xaml::FrameworkElement const& element)
{
    double const stretchedHeight = element.ActualHeight();
    double const desiredHeight = element.DesiredSize().Height;
    if (desiredHeight > 0 && desiredHeight < stretchedHeight) {
        return desiredHeight;
    }
    return stretchedHeight;
}

void AppendAcknowledgement(
    winrt::Windows::UI::Xaml::Controls::StackPanel const& panel,
    const wchar_t* text)
{
    auto item = CreateTextBlock(text, 13, 0.78);
    item.Margin(winrt::Windows::UI::Xaml::ThicknessHelper::FromLengths(0, 0, 0, 2));
    panel.Children().Append(item);
}
}

AboutPage::AboutPage(std::function<void()> diagnosticsCallback)
    : diagnosticsCallback_(std::move(diagnosticsCallback)) {
    using namespace winrt::Windows::Foundation;
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    root_ = Grid();
    root_.HorizontalAlignment(HorizontalAlignment::Stretch);
    root_.VerticalAlignment(VerticalAlignment::Stretch);

    StackPanel content;
    content.Orientation(Orientation::Vertical);
    content.Padding(ThicknessHelper::FromUniformLength(24));
    content.Spacing(14);

    content.Children().Append(CreateTextBlock(
        CLP_W(CLP_UI_ABOUT_TITLE) L" v" CLP_W(CLIPP_VERSION_STRING_3PART),
        28, 1.0, true));
    content.Children().Append(CreateTextBlock(
        CLP_W(CLP_UI_TAGLINE),
        14,
        0.8));

    StackPanel project;
    project.Orientation(Orientation::Vertical);
    project.Spacing(6);
    project.Children().Append(CreateTextBlock(CLP_W(CLP_UI_PROJECT), 16, 1.0, true));
    project.Children().Append(CreateTextBlock(CLP_W(CLP_UI_COPYRIGHT), 13, 0.82));
    project.Children().Append(CreateTextBlock(CLP_W(CLP_UI_MIT_LICENSE), 13, 0.82));

    HyperlinkButton repoLink;
    repoLink.Content(winrt::box_value(winrt::hstring{ CLP_W(CLP_UI_REPOSITORY_LABEL) }));
    repoLink.NavigateUri(Uri{ CLP_W(CLP_UI_REPOSITORY_URL) });
    repoLink.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
    repoLink.HorizontalAlignment(HorizontalAlignment::Left);
    project.Children().Append(repoLink);

    Grid projectBand;
    projectBand.HorizontalAlignment(HorizontalAlignment::Stretch);
    ColumnDefinition artworkColumn;
    artworkColumn.Width(GridLength{ 1, GridUnitType::Auto });
    ColumnDefinition detailsColumn;
    detailsColumn.Width(GridLength{ 1, GridUnitType::Star });
    projectBand.ColumnDefinitions().Append(artworkColumn);
    projectBand.ColumnDefinitions().Append(detailsColumn);

    project.Margin(ThicknessHelper::FromLengths(24, 0, 0, 0));
    Grid::SetColumn(project, 1);
    projectBand.Children().Append(project);

    auto artworkView = CreateArtworkView();
    artworkView.Width(0);
    artworkView.Height(0);
    project.SizeChanged([artworkView](auto const& sender, auto const&) {
        auto projectElement = sender.as<FrameworkElement>();
        double const side = ContentHeight(projectElement);
        artworkView.Width(side);
        artworkView.Height(side);
    });
    Grid::SetColumn(artworkView, 0);
    projectBand.Children().Append(artworkView);
    content.Children().Append(projectBand);

    StackPanel acknowledgements;
    acknowledgements.Orientation(Orientation::Vertical);
    acknowledgements.Spacing(6);
    acknowledgements.Children().Append(CreateTextBlock(CLP_W(CLP_UI_OPEN_SOURCE_ACKNOWLEDGEMENTS), 16, 1.0, true));
    AppendAcknowledgement(acknowledgements, CLP_W(CLP_UI_ACK_LIBSODIUM));
    AppendAcknowledgement(acknowledgements, CLP_W(CLP_UI_ACK_XXHASH));
    AppendAcknowledgement(acknowledgements, CLP_W(CLP_UI_ACK_ZSTD));
    AppendAcknowledgement(acknowledgements, L"Microsoft C++/WinRT and Microsoft Toolkit Win32 UI SDK - MIT-licensed Windows UI integration");
    AppendAcknowledgement(acknowledgements, L"darkmode32plus - BSD-3-Clause; includes portions from win32-darkmode (MIT), darkmodelib (MPL-2.0), PolyHook 2.0 (MIT), and UAH menu bar code by Adam D. Walling (MIT)");
    content.Children().Append(acknowledgements);

    TextBlock note = CreateTextBlock(
        CLP_W(CLP_UI_THIRD_PARTY_LICENSE_NOTE),
        12,
        0.68);
    content.Children().Append(note);

    Button diagnosticsButton;
    diagnosticsButton.Content(winrt::box_value(winrt::hstring{ CLP_W(CLP_UI_DIAGNOSTICS) }));
    diagnosticsButton.HorizontalAlignment(HorizontalAlignment::Left);
    diagnosticsButton.Padding(ThicknessHelper::FromLengths(12, 6, 12, 6));
    diagnosticsButton.Click([this](auto const&, auto const&) {
        if (diagnosticsCallback_) {
            diagnosticsCallback_();
        }
    });
    content.Children().Append(diagnosticsButton);

    ScrollViewer scroll;
    scroll.HorizontalAlignment(HorizontalAlignment::Stretch);
    scroll.VerticalAlignment(VerticalAlignment::Stretch);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.Content(content);

    root_.Children().Append(scroll);
}

winrt::Windows::UI::Xaml::Controls::Grid AboutPage::View() const {
    return root_;
}
