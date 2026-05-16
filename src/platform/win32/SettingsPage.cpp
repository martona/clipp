#include "SettingsPage.h"

#include "NetworkRuntime.h"
#include "Settings.h"
#include "platform.h"
#include "platform/uiSettingsPage.h"

#include <string>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

extern Settings g_settings;
extern NetworkRuntime g_networkRuntime;

namespace {
winrt::Windows::UI::Xaml::Controls::TextBlock MakeLabel(const wchar_t* text) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    TextBlock label;
    label.Text(text);
    label.VerticalAlignment(VerticalAlignment::Center);
    label.FontSize(13);
    return label;
}

winrt::Windows::UI::Xaml::Controls::TextBox MakeTextBox(int maxLength, double width) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    TextBox textBox;
    textBox.VerticalAlignment(VerticalAlignment::Center);
    textBox.HorizontalAlignment(HorizontalAlignment::Left);
    textBox.Width(width);
    textBox.MaxLength(maxLength);
    return textBox;
}

void AddSettingRow(
    winrt::Windows::UI::Xaml::Controls::Grid const& grid,
    int rowIndex,
    winrt::Windows::UI::Xaml::Controls::TextBlock const& label,
    winrt::Windows::UI::Xaml::Controls::TextBox const& field)
{
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    RowDefinition row;
    row.Height(GridLength{ 1, GridUnitType::Auto });
    grid.RowDefinitions().Append(row);

    Grid::SetRow(label, rowIndex);
    Grid::SetColumn(label, 0);
    Grid::SetRow(field, rowIndex);
    Grid::SetColumn(field, 1);

    grid.Children().Append(label);
    grid.Children().Append(field);
}
}

SettingsPage::SettingsPage() {
    BuildView();
    LoadSettingsIntoFields();
}

winrt::Windows::UI::Xaml::Controls::Grid SettingsPage::View() const {
    return root_;
}

void SettingsPage::OnShown() {
    LoadSettingsIntoFields();
}

void SettingsPage::BuildView() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    root_ = Grid();
    root_.HorizontalAlignment(HorizontalAlignment::Stretch);
    root_.VerticalAlignment(VerticalAlignment::Stretch);

    RowDefinition contentRow;
    contentRow.Height(GridLength{ 1, GridUnitType::Star });
    RowDefinition messageRow;
    messageRow.Height(GridLength{ 1, GridUnitType::Auto });
    root_.RowDefinitions().Append(contentRow);
    root_.RowDefinitions().Append(messageRow);

    StackPanel content;
    content.Orientation(Orientation::Vertical);
    content.Padding(ThicknessHelper::FromUniformLength(24));
    content.Spacing(16);

    TextBlock heading;
    heading.Text(L"Settings");
    heading.FontSize(28);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    heading.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(heading);

    TextBlock networkHeader;
    networkHeader.Text(L"Network");
    networkHeader.FontSize(16);
    networkHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    content.Children().Append(networkHeader);

    Grid section;
    section.CornerRadius(CornerRadius{ 4 });
    section.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    section.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    section.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    section.RowSpacing(12);
    section.ColumnSpacing(16);

    ColumnDefinition labelColumn;
    labelColumn.Width(GridLength{ 130, GridUnitType::Pixel });
    ColumnDefinition fieldColumn;
    fieldColumn.Width(GridLength{ 1, GridUnitType::Star });
    section.ColumnDefinitions().Append(labelColumn);
    section.ColumnDefinitions().Append(fieldColumn);

    tcpPortField_ = MakeTextBox(5, 110);
    udpPortField_ = MakeTextBox(5, 110);
    listenerIpField_ = MakeTextBox(15, 190);
    multicastIpField_ = MakeTextBox(15, 190);

    AddSettingRow(section, 0, MakeLabel(L"TCP Port"), tcpPortField_);
    AddSettingRow(section, 1, MakeLabel(L"UDP Port"), udpPortField_);
    AddSettingRow(section, 2, MakeLabel(L"Listener IP"), listenerIpField_);
    AddSettingRow(section, 3, MakeLabel(L"Multicast IP"), multicastIpField_);

    tcpPortField_.LostFocus([this](auto const&, auto const&) {
        ValidateTcpPort();
    });
    udpPortField_.LostFocus([this](auto const&, auto const&) {
        ValidateUdpPort();
    });
    listenerIpField_.LostFocus([this](auto const&, auto const&) {
        ValidateListenerIp();
    });
    multicastIpField_.LostFocus([this](auto const&, auto const&) {
        ValidateMulticastIp();
    });

    content.Children().Append(section);

    ScrollViewer scroll;
    scroll.HorizontalAlignment(HorizontalAlignment::Stretch);
    scroll.VerticalAlignment(VerticalAlignment::Stretch);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.Content(content);

    statusMessage_ = TextBlock();
    statusMessage_.Text(L"Network settings applied.");
    statusMessage_.FontSize(13);
    statusMessage_.Opacity(0.75);
    statusMessage_.TextWrapping(TextWrapping::WrapWholeWords);
    statusMessage_.Padding(ThicknessHelper::FromLengths(24, 0, 24, 24));
    statusMessage_.Visibility(Visibility::Collapsed);

    Grid::SetRow(scroll, 0);
    Grid::SetRow(statusMessage_, 1);
    root_.Children().Append(scroll);
    root_.Children().Append(statusMessage_);
}

void SettingsPage::LoadSettingsIntoFields() {
    if (!tcpPortField_ || !udpPortField_ || !listenerIpField_ || !multicastIpField_) {
        return;
    }

    tcpPortField_.Text(winrt::to_hstring(g_settings.tcpPort()));
    udpPortField_.Text(winrt::to_hstring(g_settings.mdnsPort()));
    listenerIpField_.Text(ToHString(g_settings.listenerIp()));
    multicastIpField_.Text(ToHString(g_settings.multicastIp()));
}

void SettingsPage::ApplyNetworkSettingChange() {
    g_networkRuntime.Restart();
    ShowStatusMessage();
}

void SettingsPage::ShowStatusMessage() {
    if (statusMessage_) {
        statusMessage_.Visibility(winrt::Windows::UI::Xaml::Visibility::Visible);
    }
}

void SettingsPage::ValidateTcpPort() {
    int port = 0;
    const int currentValue = g_settings.tcpPort();
    if (!uiSettingsPage::TryParsePort(winrt::to_string(tcpPortField_.Text()), port)) {
        tcpPortField_.Text(winrt::to_hstring(currentValue));
        return;
    }

    if (port != currentValue && g_settings.set_tcpPort(port)) {
        ApplyNetworkSettingChange();
    }
    tcpPortField_.Text(winrt::to_hstring(g_settings.tcpPort()));
}

void SettingsPage::ValidateUdpPort() {
    int port = 0;
    const int currentValue = g_settings.mdnsPort();
    if (!uiSettingsPage::TryParsePort(winrt::to_string(udpPortField_.Text()), port)) {
        udpPortField_.Text(winrt::to_hstring(currentValue));
        return;
    }

    if (port != currentValue && g_settings.set_mdnsPort(port)) {
        ApplyNetworkSettingChange();
    }
    udpPortField_.Text(winrt::to_hstring(g_settings.mdnsPort()));
}

void SettingsPage::ValidateListenerIp() {
    const std::string value = uiSettingsPage::TrimAscii(winrt::to_string(listenerIpField_.Text()));
    const std::string currentValue = g_settings.listenerIp();
    if (!Settings::IsValidListenerIp(value)) {
        listenerIpField_.Text(ToHString(currentValue));
        return;
    }

    if (value != currentValue && g_settings.set_listenerIp(value)) {
        ApplyNetworkSettingChange();
    }
    listenerIpField_.Text(ToHString(g_settings.listenerIp()));
}

void SettingsPage::ValidateMulticastIp() {
    const std::string value = uiSettingsPage::TrimAscii(winrt::to_string(multicastIpField_.Text()));
    const std::string currentValue = g_settings.multicastIp();
    if (!Settings::IsValidMulticastIp(value)) {
        multicastIpField_.Text(ToHString(currentValue));
        return;
    }

    if (value != currentValue && g_settings.set_multicastIp(value)) {
        ApplyNetworkSettingChange();
    }
    multicastIpField_.Text(ToHString(g_settings.multicastIp()));
}

winrt::hstring SettingsPage::ToHString(const std::string& value) {
    return winrt::to_hstring(value);
}
