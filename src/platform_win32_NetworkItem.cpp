#include "platform_win32_NetworkItem.h"

#include "platform/uiClippPage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#include "HostId.h"

namespace {
winrt::Windows::UI::Xaml::Controls::ColumnDefinition MakeColumn(double value, winrt::Windows::UI::Xaml::GridUnitType type) {
    winrt::Windows::UI::Xaml::Controls::ColumnDefinition column;
    column.Width(winrt::Windows::UI::Xaml::GridLength{ value, type });
    return column;
}

winrt::Windows::UI::Xaml::Controls::TextBlock AddDetailRow(
    winrt::Windows::UI::Xaml::Controls::Grid const& grid,
    int rowIndex,
    const wchar_t* labelText) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    RowDefinition row;
    row.Height(GridLength{ 1, GridUnitType::Auto });
    grid.RowDefinitions().Append(row);

    TextBlock label;
    label.Text(labelText);
    label.FontSize(13);
    label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    label.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
    Grid::SetColumn(label, 0);
    Grid::SetRow(label, rowIndex);

    TextBlock value;
    value.FontSize(13);
    value.Opacity(0.7);
    value.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
    Grid::SetColumn(value, 1);
    Grid::SetRow(value, rowIndex);

    grid.Children().Append(label);
    grid.Children().Append(value);
    return value;
}

std::wstring ToWideAscii(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}
}

NetworkItemView::NetworkItemView(const PeerDisplayItem& item) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Controls::Primitives;
    using namespace winrt::Windows::UI::Xaml::Media;

    card_ = StackPanel();
    card_.CornerRadius(CornerRadius{ 4 });
    card_.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    card_.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));

    Grid headerGrid;
    headerGrid.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    headerGrid.ColumnDefinitions().Append(MakeColumn(40, GridUnitType::Pixel));
    headerGrid.ColumnDefinitions().Append(MakeColumn(1, GridUnitType::Star));
    headerGrid.ColumnDefinitions().Append(MakeColumn(45, GridUnitType::Pixel));
    headerGrid.ColumnDefinitions().Append(MakeColumn(40, GridUnitType::Pixel));

    FontIcon netIcon;
    netIcon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    netIcon.Glyph(L"\xE839");
    netIcon.FontSize(18);
    netIcon.VerticalAlignment(VerticalAlignment::Center);
    netIcon.HorizontalAlignment(HorizontalAlignment::Left);
    Grid::SetColumn(netIcon, 0);

    StackPanel textStack;
    textStack.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(textStack, 1);

    title_ = TextBlock();
    title_.FontSize(14);
    title_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());

    subtitle_ = TextBlock();
    subtitle_.FontSize(12);
    subtitle_.Opacity(0.6);

    textStack.Children().Append(title_);
    textStack.Children().Append(subtitle_);

    ToggleButton chevron;
    chevron.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    chevron.Content(winrt::box_value(L"\xE70D"));
    chevron.Background(SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    chevron.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
    chevron.VerticalAlignment(VerticalAlignment::Center);
    chevron.HorizontalAlignment(HorizontalAlignment::Right);
    Grid::SetColumn(chevron, 3);

    StackPanel statusIcons;
    statusIcons.Orientation(Orientation::Horizontal);
    statusIcons.VerticalAlignment(VerticalAlignment::Center);
    statusIcons.HorizontalAlignment(HorizontalAlignment::Right);
    Grid::SetColumn(statusIcons, 2);

    inIcon_ = FontIcon();
    inIcon_.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    inIcon_.Glyph(L"\xE118");
    inIcon_.FontSize(12);
    inIcon_.Width(20);
    inIcon_.Foreground(SolidColorBrush(winrt::Windows::UI::Colors::LimeGreen()));

    outIcon_ = FontIcon();
    outIcon_.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    outIcon_.Glyph(L"\xE11C");
    outIcon_.FontSize(12);
    outIcon_.Width(20);
    outIcon_.Foreground(SolidColorBrush(winrt::Windows::UI::Colors::DeepSkyBlue()));

    statusIcons.Children().Append(inIcon_);
    statusIcons.Children().Append(outIcon_);

    headerGrid.Children().Append(netIcon);
    headerGrid.Children().Append(textStack);
    headerGrid.Children().Append(chevron);
    headerGrid.Children().Append(statusIcons);

    Grid contentGrid;
    contentGrid.Visibility(Visibility::Collapsed);
    contentGrid.Padding(ThicknessHelper::FromLengths(56, 0, 16, 16));
    contentGrid.ColumnSpacing(24);
    contentGrid.ColumnDefinitions().Append(MakeColumn(130, GridUnitType::Pixel));
    contentGrid.ColumnDefinitions().Append(MakeColumn(1, GridUnitType::Star));

    //hostIDValue_ = AddDetailRow(contentGrid, 0, L"Host ID:");
    bytesSentValue_ = AddDetailRow(contentGrid, 0, L"Bytes sent:");
    bytesReceivedValue_ = AddDetailRow(contentGrid, 1, L"Bytes received:");
    incomingValue_ = AddDetailRow(contentGrid, 2, L"Incoming:");
    outgoingValue_ = AddDetailRow(contentGrid, 3, L"Outgoing:");

    chevron.Click([=](winrt::Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&) {
        auto toggle = sender.as<ToggleButton>();
        if (toggle.IsChecked().GetBoolean()) {
            contentGrid.Visibility(Visibility::Visible);
            toggle.Content(winrt::box_value(L"\xE70E"));
        } else {
            contentGrid.Visibility(Visibility::Collapsed);
            toggle.Content(winrt::box_value(L"\xE70D"));
        }
    });

    card_.Children().Append(headerGrid);
    card_.Children().Append(contentGrid);

    UpdateHostName(item.hostName);
    UpdateHostID(item.hostID);
    UpdateIncomingConnection(item.hasIncomingConnection);
    UpdateOutgoingConnection(item.hasOutgoingConnection);
    UpdateBytesSent(item.bytesSent);
    UpdateBytesReceived(item.bytesReceived);
    UpdateConnectedSince(item.connectedSince);
}

winrt::Windows::UI::Xaml::Controls::StackPanel NetworkItemView::View() const {
    return card_;
}

void NetworkItemView::UpdateHostName(const std::wstring& hostName) {
    title_.Text(DisplayHostName(hostName));
}

void NetworkItemView::UpdateHostID(const HostId& hostID) {
    //hostIDValue_.Text(FormatHostID(hostID));
}

void NetworkItemView::UpdateIncomingConnection(bool connected) {
    inIcon_.Visibility(connected ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed);
    incomingValue_.Text(FormatConnectionState(connected));
}

void NetworkItemView::UpdateOutgoingConnection(bool connected) {
    outIcon_.Visibility(connected ? winrt::Windows::UI::Xaml::Visibility::Visible : winrt::Windows::UI::Xaml::Visibility::Collapsed);
    outgoingValue_.Text(FormatConnectionState(connected));
}

void NetworkItemView::UpdateBytesSent(uint64_t bytesSent) {
    bytesSentValue_.Text(FormatByteCounter(bytesSent));
}

void NetworkItemView::UpdateBytesReceived(uint64_t bytesReceived) {
    bytesReceivedValue_.Text(FormatByteCounter(bytesReceived));
}

void NetworkItemView::UpdateConnectedSince(std::chrono::steady_clock::time_point connectedSince) {
    connectedSince_ = connectedSince;
    connectedForText_.clear();
    RefreshConnectedFor();
}

void NetworkItemView::RefreshConnectedFor(std::chrono::steady_clock::time_point now) {
    const auto text = FormatConnectedFor(connectedSince_, now);
    if (text != connectedForText_) {
        connectedForText_ = text;
        subtitle_.Text(text);
    }
}

std::wstring NetworkItemView::DisplayHostName(const std::wstring& hostName) {
    return uiClippPage::DisplayHostNameOrUnknown(hostName);
}

std::wstring NetworkItemView::FormatHostID(const std::array<unsigned char, 32>& hostID) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text;
    text.reserve((hostID.size() * 2) + 7);
    for (std::size_t i = 0; i < hostID.size(); ++i) {
        if (i > 0 && (i % 4) == 0) {
            text.push_back(L'-');
        }
        text.push_back(digits[(hostID[i] >> 4) & 0x0F]);
        text.push_back(digits[hostID[i] & 0x0F]);
    }
    return text;
}

std::wstring NetworkItemView::FormatByteCounter(uint64_t bytes) {
    return ToWideAscii(uiClippPage::FormatByteCounter(bytes));
}

std::wstring NetworkItemView::FormatConnectionState(bool connected) {
    return ToWideAscii(uiClippPage::FormatConnectionState(connected));
}

std::wstring NetworkItemView::FormatConnectedFor(std::chrono::steady_clock::time_point connectedSince, std::chrono::steady_clock::time_point now) {
    return ToWideAscii(uiClippPage::FormatConnectedFor(connectedSince, now));
}
