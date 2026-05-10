#include "platform_win32_PlaceholderPage.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

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

AboutPage::AboutPage()
    : PlaceholderPage(L"About", L"About Clipp details, version information, and credits will be added here in a future update.") {
}
