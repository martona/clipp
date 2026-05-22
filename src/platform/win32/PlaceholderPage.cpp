#include "PlaceholderPage.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

namespace {
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

void AppendAcknowledgement(
    winrt::Windows::UI::Xaml::Controls::StackPanel const& panel,
    const wchar_t* text)
{
    auto item = CreateTextBlock(text, 13, 0.78);
    item.Margin(winrt::Windows::UI::Xaml::ThicknessHelper::FromLengths(0, 0, 0, 2));
    panel.Children().Append(item);
}
}

PlaceholderPage::PlaceholderPage(const wchar_t* title, const wchar_t* body) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    root_ = Grid();
    root_.HorizontalAlignment(HorizontalAlignment::Stretch);
    root_.VerticalAlignment(VerticalAlignment::Stretch);

    StackPanel content;
    content.Orientation(Orientation::Vertical);
    content.Padding(ThicknessHelper::FromUniformLength(24));
    content.Spacing(16);

    TextBlock heading;
    heading.Text(title);
    heading.FontSize(28);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    heading.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(heading);

    TextBlock placeholder;
    placeholder.Text(body);
    placeholder.FontSize(14);
    placeholder.TextWrapping(TextWrapping::WrapWholeWords);
    placeholder.Opacity(0.75);
    content.Children().Append(placeholder);

    root_.Children().Append(content);
}

winrt::Windows::UI::Xaml::Controls::Grid PlaceholderPage::View() const {
    return root_;
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

    content.Children().Append(CreateTextBlock(L"About Clipp", 28, 1.0, true));
    content.Children().Append(CreateTextBlock(
        L"Secure cross-platform clipboard sync for trusted machines on your network.",
        14,
        0.8));

    StackPanel project;
    project.Orientation(Orientation::Vertical);
    project.Spacing(6);
    project.Children().Append(CreateTextBlock(L"Project", 16, 1.0, true));
    project.Children().Append(CreateTextBlock(L"Copyright (C) 2026 Marton Anka", 13, 0.82));
    project.Children().Append(CreateTextBlock(L"Released under the MIT License.", 13, 0.82));

    HyperlinkButton repoLink;
    repoLink.Content(winrt::box_value(winrt::hstring{ L"github.com/martona/clipp" }));
    repoLink.NavigateUri(Uri{ L"https://github.com/martona/clipp" });
    repoLink.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
    repoLink.HorizontalAlignment(HorizontalAlignment::Left);
    project.Children().Append(repoLink);
    content.Children().Append(project);

    StackPanel acknowledgements;
    acknowledgements.Orientation(Orientation::Vertical);
    acknowledgements.Spacing(6);
    acknowledgements.Children().Append(CreateTextBlock(L"Open Source Acknowledgements", 16, 1.0, true));
    AppendAcknowledgement(acknowledgements, L"libsodium - ISC-licensed cryptography library");
    AppendAcknowledgement(acknowledgements, L"lodepng - zlib-licensed PNG encoder/decoder");
    AppendAcknowledgement(acknowledgements, L"xxHash - BSD-2-Clause non-cryptographic hashing");
    AppendAcknowledgement(acknowledgements, L"Zstandard (zstd) - BSD-licensed compression");
    AppendAcknowledgement(acknowledgements, L"Microsoft C++/WinRT and Microsoft Toolkit Win32 UI SDK - MIT-licensed Windows UI integration");
    AppendAcknowledgement(acknowledgements, L"darkmode32plus - BSD-3-Clause; includes portions from win32-darkmode (MIT), darkmodelib (MPL-2.0), PolyHook 2.0 (MIT), and UAH menu bar code by Adam D. Walling (MIT)");
    content.Children().Append(acknowledgements);

    TextBlock note = CreateTextBlock(
        L"Third-party license terms remain with their respective projects.",
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
