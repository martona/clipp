#include "AboutPage.h"

#include "resource.h"
#include "platform/uistrings.h"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
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
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/base.h>

namespace {
winrt::Windows::Storage::Streams::IRandomAccessStream LoadResourceStream(int resourceId)
{
    using namespace winrt::Windows::Storage::Streams;

    HINSTANCE const instance = GetModuleHandleW(nullptr);
    HRSRC const resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return nullptr;
    }

    DWORD const resourceSize = SizeofResource(instance, resource);
    HGLOBAL const resourceData = LoadResource(instance, resource);
    if (!resourceData) {
        return nullptr;
    }

    auto const* bytes = static_cast<const std::uint8_t*>(LockResource(resourceData));
    if (!bytes || resourceSize == 0) {
        return nullptr;
    }

    InMemoryRandomAccessStream stream;
    DataWriter writer(stream.GetOutputStreamAt(0));
    writer.WriteBytes(winrt::array_view<std::uint8_t const>(bytes, bytes + resourceSize));
    writer.StoreAsync().get();
    writer.DetachStream();
    stream.Seek(0);
    return stream;
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

    try {
        if (auto stream = LoadResourceStream(IDB_CLIPP_ABOUT_IMAGE)) {
            BitmapImage bitmap;
            bitmap.SetSource(stream);
            artwork.Source(bitmap);
        }
    } catch (winrt::hresult_error const&) {
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

AboutPage::AboutPage() {
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

    content.Children().Append(CreateTextBlock(CLP_W(CLP_UI_ABOUT_TITLE), 28, 1.0, true));
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
    AppendAcknowledgement(acknowledgements, CLP_W(CLP_UI_ACK_LODEPNG));
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
