#include "SettingsPage.h"

#include "ClipboardActivityStore.h"
#include "MDNSThread.h"
#include "NetworkRuntime.h"
#include "PeerManager.h"
#include "Settings.h"
#include "platform.h"
#include "platform/uiSettingsPage.h"
#include "platform/uistrings.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

extern Settings g_settings;
extern NetworkRuntime g_networkRuntime;
extern PeerManager g_peerManager;
extern ClipboardActivityStore g_clipboardActivityStore;

namespace {
struct LimitStop {
    uint64_t value;
    const wchar_t* label;
};

constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * kMiB;

constexpr LimitStop kHistoryMemoryStops[] = {
    { 1ull * kMiB, L"1 MB" },
    { 8ull * kMiB, L"8 MB" },
    { 32ull * kMiB, L"32 MB" },
    { 128ull * kMiB, L"128 MB" },
    { Settings::DefaultClipboardHistoryMemoryLimitBytes, L"256 MB" },
    { 512ull * kMiB, L"512 MB" },
    { 1ull * kGiB, L"1 GB" },
    { 2ull * kGiB, L"2 GB" },
    { Settings::UnlimitedClipboardHistoryLimit, CLP_W(CLP_UI_UNLIMITED) },
};

constexpr LimitStop kHistoryAgeStops[] = {
    { 1, L"1 second" },
    { 10, L"10 seconds" },
    { 60, L"1 minute" },
    { 10ull * 60ull, L"10 minutes" },
    { 60ull * 60ull, L"1 hour" },
    { 6ull * 60ull * 60ull, L"6 hours" },
    { Settings::DefaultClipboardHistoryMaxAgeSeconds, L"1 day" },
    { 7ull * 24ull * 60ull * 60ull, L"7 days" },
    { 30ull * 24ull * 60ull * 60ull, L"30 days" },
    { Settings::UnlimitedClipboardHistoryLimit, CLP_W(CLP_UI_UNLIMITED) },
};

constexpr LimitStop kHistoryItemStops[] = {
    { 1, L"1 item" },
    { 10, L"10 items" },
    { 50, L"50 items" },
    { 100, L"100 items" },
    { 500, L"500 items" },
    { Settings::DefaultClipboardHistoryMaxItems, L"1000 items" },
    { 5000, L"5000 items" },
    { 10000, L"10000 items" },
    { Settings::UnlimitedClipboardHistoryLimit, CLP_W(CLP_UI_UNLIMITED) },
};

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

winrt::Windows::UI::Xaml::Controls::Slider MakeLimitSlider(std::size_t stopCount) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Controls::Primitives;

    Slider slider;
    slider.VerticalAlignment(VerticalAlignment::Center);
    slider.HorizontalAlignment(HorizontalAlignment::Stretch);
    slider.Minimum(0);
    slider.Maximum(stopCount > 0 ? static_cast<double>(stopCount - 1) : 0);
    slider.StepFrequency(1);
    slider.TickFrequency(1);
    slider.TickPlacement(TickPlacement::BottomRight);
    slider.SnapsTo(SliderSnapsTo::StepValues);
    slider.IsThumbToolTipEnabled(false);
    return slider;
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

void AddSliderSettingRow(
    winrt::Windows::UI::Xaml::Controls::Grid const& grid,
    int rowIndex,
    winrt::Windows::UI::Xaml::Controls::TextBlock const& label,
    winrt::Windows::UI::Xaml::Controls::Slider const& slider,
    winrt::Windows::UI::Xaml::Controls::TextBlock const& value)
{
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    RowDefinition row;
    row.Height(GridLength{ 1, GridUnitType::Auto });
    grid.RowDefinitions().Append(row);

    Grid::SetRow(label, rowIndex);
    Grid::SetColumn(label, 0);
    Grid::SetRow(slider, rowIndex);
    Grid::SetColumn(slider, 1);
    Grid::SetRow(value, rowIndex);
    Grid::SetColumn(value, 2);

    grid.Children().Append(label);
    grid.Children().Append(slider);
    grid.Children().Append(value);
}

winrt::Windows::UI::Xaml::Controls::Button MakeButton(const wchar_t* text) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    Button button;
    button.Content(winrt::box_value(winrt::hstring(text)));
    button.VerticalAlignment(VerticalAlignment::Center);
    button.HorizontalAlignment(HorizontalAlignment::Right);
    return button;
}

void AddHostIDRow(
    winrt::Windows::UI::Xaml::Controls::Grid const& grid,
    int rowIndex,
    winrt::Windows::UI::Xaml::Controls::TextBlock const& label,
    winrt::Windows::UI::Xaml::Controls::TextBlock const& value,
    winrt::Windows::UI::Xaml::Controls::Button const& resetButton)
{
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    RowDefinition row;
    row.Height(GridLength{ 1, GridUnitType::Auto });
    grid.RowDefinitions().Append(row);

    Grid::SetRow(label, rowIndex);
    Grid::SetColumn(label, 0);
    Grid::SetRow(value, rowIndex);
    Grid::SetColumn(value, 1);
    Grid::SetRow(resetButton, rowIndex);
    Grid::SetColumn(resetButton, 2);

    grid.Children().Append(label);
    grid.Children().Append(value);
    grid.Children().Append(resetButton);
}

template <std::size_t N>
std::size_t FindStopIndex(const LimitStop(&stops)[N], uint64_t value) {
    for (std::size_t i = 0; i < N; ++i) {
        if (stops[i].value == value) {
            return i;
        }
    }

    for (std::size_t i = 0; i + 1 < N; ++i) {
        if (value <= stops[i].value) {
            return i;
        }
    }

    return N - 1;
}

template <std::size_t N>
std::size_t SliderStopIndex(winrt::Windows::UI::Xaml::Controls::Slider const& slider, const LimitStop(&)[N]) {
    const double value = slider.Value();
    if (value <= 0) {
        return 0;
    }

    const auto index = static_cast<std::size_t>(value + 0.5);
    return index < N ? index : N - 1;
}

template <std::size_t N>
uint64_t SliderStopValue(winrt::Windows::UI::Xaml::Controls::Slider const& slider, const LimitStop(&stops)[N]) {
    return stops[SliderStopIndex(slider, stops)].value;
}

template <std::size_t N>
const wchar_t* SliderStopLabel(winrt::Windows::UI::Xaml::Controls::Slider const& slider, const LimitStop(&stops)[N]) {
    return stops[SliderStopIndex(slider, stops)].label;
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
    heading.Text(CLP_W(CLP_UI_SETTINGS));
    heading.FontSize(28);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    heading.TextWrapping(TextWrapping::Wrap);
    content.Children().Append(heading);

    TextBlock networkHeader;
    networkHeader.Text(CLP_W(CLP_UI_NETWORK));
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

    AddSettingRow(section, 0, MakeLabel(CLP_W(CLP_UI_TCP_PORT)), tcpPortField_);
    AddSettingRow(section, 1, MakeLabel(CLP_W(CLP_UI_UDP_PORT)), udpPortField_);
    AddSettingRow(section, 2, MakeLabel(CLP_W(CLP_UI_LISTENER_IP)), listenerIpField_);
    AddSettingRow(section, 3, MakeLabel(CLP_W(CLP_UI_MULTICAST_IP)), multicastIpField_);

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

    TextBlock historyHeader;
    historyHeader.Text(CLP_W(CLP_UI_CLIPBOARD_HISTORY));
    historyHeader.FontSize(16);
    historyHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    content.Children().InsertAt(1, historyHeader);

    Grid historySection;
    historySection.CornerRadius(CornerRadius{ 4 });
    historySection.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    historySection.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    historySection.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    historySection.RowSpacing(12);
    historySection.ColumnSpacing(16);

    ColumnDefinition historyLabelColumn;
    historyLabelColumn.Width(GridLength{ 130, GridUnitType::Pixel });
    ColumnDefinition historySliderColumn;
    historySliderColumn.Width(GridLength{ 1, GridUnitType::Star });
    ColumnDefinition historyValueColumn;
    historyValueColumn.Width(GridLength{ 112, GridUnitType::Pixel });
    historySection.ColumnDefinitions().Append(historyLabelColumn);
    historySection.ColumnDefinitions().Append(historySliderColumn);
    historySection.ColumnDefinitions().Append(historyValueColumn);

    historyMemorySlider_ = MakeLimitSlider(cntof(kHistoryMemoryStops));
    historyAgeSlider_ = MakeLimitSlider(cntof(kHistoryAgeStops));
    historyItemSlider_ = MakeLimitSlider(cntof(kHistoryItemStops));

    historyMemoryValue_ = MakeLabel(L"");
    historyAgeValue_ = MakeLabel(L"");
    historyItemValue_ = MakeLabel(L"");
    historyMemoryValue_.HorizontalAlignment(HorizontalAlignment::Right);
    historyAgeValue_.HorizontalAlignment(HorizontalAlignment::Right);
    historyItemValue_.HorizontalAlignment(HorizontalAlignment::Right);

    AddSliderSettingRow(historySection, 0, MakeLabel(CLP_W(CLP_UI_HISTORY_MEMORY_LIMIT)), historyMemorySlider_, historyMemoryValue_);
    AddSliderSettingRow(historySection, 1, MakeLabel(CLP_W(CLP_UI_HISTORY_TIME_LIMIT)), historyAgeSlider_, historyAgeValue_);
    AddSliderSettingRow(historySection, 2, MakeLabel(CLP_W(CLP_UI_HISTORY_ITEM_LIMIT)), historyItemSlider_, historyItemValue_);

    historyMemorySlider_.ValueChanged([this](auto const&, auto const&) {
        ApplyClipboardHistorySettingChange();
    });
    historyAgeSlider_.ValueChanged([this](auto const&, auto const&) {
        ApplyClipboardHistorySettingChange();
    });
    historyItemSlider_.ValueChanged([this](auto const&, auto const&) {
        ApplyClipboardHistorySettingChange();
    });

    content.Children().InsertAt(2, historySection);

    TextBlock hostIDHeader;
    hostIDHeader.Text(CLP_W(CLP_UI_HOST_ID));
    hostIDHeader.FontSize(16);
    hostIDHeader.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    content.Children().Append(hostIDHeader);

    Grid hostIDSection;
    hostIDSection.CornerRadius(CornerRadius{ 4 });
    hostIDSection.BorderThickness(ThicknessHelper::FromLengths(1, 1, 1, 1));
    hostIDSection.BorderBrush(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(50, 150, 150, 150)));
    hostIDSection.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
    hostIDSection.ColumnSpacing(16);

    ColumnDefinition hostIDLabelColumn;
    hostIDLabelColumn.Width(GridLength{ 130, GridUnitType::Pixel });
    ColumnDefinition hostIDValueColumn;
    hostIDValueColumn.Width(GridLength{ 1, GridUnitType::Star });
    ColumnDefinition hostIDButtonColumn;
    hostIDButtonColumn.Width(GridLength{ 1, GridUnitType::Auto });
    hostIDSection.ColumnDefinitions().Append(hostIDLabelColumn);
    hostIDSection.ColumnDefinitions().Append(hostIDValueColumn);
    hostIDSection.ColumnDefinitions().Append(hostIDButtonColumn);

    hostIDValue_ = TextBlock();
    hostIDValue_.VerticalAlignment(VerticalAlignment::Center);
    hostIDValue_.TextWrapping(TextWrapping::NoWrap);
    hostIDValue_.FontSize(12);
    hostIDValue_.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily(L"Consolas"));
    resetHostIDButton_ = MakeButton(CLP_W(CLP_UI_RESET));
    resetHostIDButton_.Click([this](auto const&, auto const&) {
        ResetHostID();
    });

    AddHostIDRow(hostIDSection, 0, MakeLabel(CLP_W(CLP_UI_CURRENT_HOST_ID)), hostIDValue_, resetHostIDButton_);
    content.Children().Append(hostIDSection);

    hostIDWarning_ = TextBlock();
    hostIDWarning_.Text(CLP_W(CLP_UI_HOST_ID_COLLISION_WARNING));
    hostIDWarning_.FontSize(13);
    hostIDWarning_.Opacity(0.85);
    hostIDWarning_.Foreground(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(255, 198, 116, 0)));
    hostIDWarning_.TextWrapping(TextWrapping::WrapWholeWords);
    hostIDWarning_.Visibility(Visibility::Collapsed);
    content.Children().Append(hostIDWarning_);

    ScrollViewer scroll;
    scroll.HorizontalAlignment(HorizontalAlignment::Stretch);
    scroll.VerticalAlignment(VerticalAlignment::Stretch);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scroll.Content(content);

    statusMessage_ = TextBlock();
    statusMessage_.Text(CLP_W(CLP_UI_NETWORK_SETTINGS_APPLIED));
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

    loadingSettings_ = true;
    tcpPortField_.Text(winrt::to_hstring(g_settings.tcpPort()));
    udpPortField_.Text(winrt::to_hstring(g_settings.mdnsPort()));
    listenerIpField_.Text(ToHString(g_settings.listenerIp()));
    multicastIpField_.Text(ToHString(g_settings.multicastIp()));
    RefreshClipboardHistoryControls();
    loadingSettings_ = false;

    RefreshHostIDDisplay();
    RefreshHostIDWarning();
}

void SettingsPage::ApplyNetworkSettingChange() {
    g_networkRuntime.Restart();
    statusMessage_.Text(CLP_W(CLP_UI_NETWORK_SETTINGS_APPLIED));
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

void SettingsPage::RefreshHostIDDisplay() {
    if (!hostIDValue_) {
        return;
    }

    HostId hostID;
    if (!g_settings.ensureHostID(hostID)) {
        hostIDValue_.Text(CLP_W(CLP_UI_UNAVAILABLE));
        return;
    }

    hostIDValue_.Text(winrt::hstring(hostID.ToHexWString()));
}

void SettingsPage::RefreshHostIDWarning() {
    if (hostIDWarning_) {
        hostIDWarning_.Visibility(MDNSHasHostIDCollisionWarning()
            ? winrt::Windows::UI::Xaml::Visibility::Visible
            : winrt::Windows::UI::Xaml::Visibility::Collapsed);
    }
}

void SettingsPage::ResetHostID() {
    HostId hostID;
    if (!g_settings.resetHostID(hostID)) {
        statusMessage_.Text(CLP_W(CLP_UI_UNABLE_TO_RESET_HOST_ID));
        ShowStatusMessage();
        return;
    }

    MDNSNotifyHostIDChange();
    g_peerManager.ClearPeers();
    RefreshHostIDDisplay();
    RefreshHostIDWarning();
    statusMessage_.Text(CLP_W(CLP_UI_HOST_ID_RESET));
    ShowStatusMessage();
}

void SettingsPage::RefreshClipboardHistoryControls() {
    if (!historyMemorySlider_ || !historyAgeSlider_ || !historyItemSlider_) {
        return;
    }

    historyMemorySlider_.Value(static_cast<double>(
        FindStopIndex(kHistoryMemoryStops, g_settings.clipboardHistoryMemoryLimitBytes())));
    historyAgeSlider_.Value(static_cast<double>(
        FindStopIndex(kHistoryAgeStops, g_settings.clipboardHistoryMaxAgeSeconds())));
    historyItemSlider_.Value(static_cast<double>(
        FindStopIndex(kHistoryItemStops, g_settings.clipboardHistoryMaxItems())));
    UpdateClipboardHistoryValueLabels();
}

void SettingsPage::UpdateClipboardHistoryValueLabels() {
    if (historyMemoryValue_) {
        historyMemoryValue_.Text(SliderStopLabel(historyMemorySlider_, kHistoryMemoryStops));
    }
    if (historyAgeValue_) {
        historyAgeValue_.Text(SliderStopLabel(historyAgeSlider_, kHistoryAgeStops));
    }
    if (historyItemValue_) {
        historyItemValue_.Text(SliderStopLabel(historyItemSlider_, kHistoryItemStops));
    }
}

void SettingsPage::ApplyClipboardHistorySettingChange() {
    if (loadingSettings_) {
        UpdateClipboardHistoryValueLabels();
        return;
    }

    UpdateClipboardHistoryValueLabels();

    const uint64_t memoryLimitBytes = SliderStopValue(historyMemorySlider_, kHistoryMemoryStops);
    const uint64_t maxAgeSeconds = SliderStopValue(historyAgeSlider_, kHistoryAgeStops);
    const uint64_t maxItems = SliderStopValue(historyItemSlider_, kHistoryItemStops);

    bool changed = false;
    if (memoryLimitBytes != g_settings.clipboardHistoryMemoryLimitBytes()) {
        changed = g_settings.set_clipboardHistoryMemoryLimitBytes(memoryLimitBytes) || changed;
    }
    if (maxAgeSeconds != g_settings.clipboardHistoryMaxAgeSeconds()) {
        changed = g_settings.set_clipboardHistoryMaxAgeSeconds(maxAgeSeconds) || changed;
    }
    if (maxItems != g_settings.clipboardHistoryMaxItems()) {
        changed = g_settings.set_clipboardHistoryMaxItems(maxItems) || changed;
    }

    if (!changed) {
        return;
    }

    g_clipboardActivityStore.SetLimits(
        g_settings.clipboardHistoryMemoryLimitBytes(),
        g_settings.clipboardHistoryMaxAgeSeconds(),
        g_settings.clipboardHistoryMaxItems());

    statusMessage_.Text(CLP_W(CLP_UI_CLIPBOARD_HISTORY_SETTINGS_APPLIED));
    ShowStatusMessage();
}

winrt::hstring SettingsPage::ToHString(const std::string& value) {
    return winrt::to_hstring(value);
}
