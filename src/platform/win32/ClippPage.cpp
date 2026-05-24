#include "ClippPage.h"

#include "Clipboard.h"
#include "KeyManager.h"
#include "platform/uistrings.h"
#include "utils.h"

#include <algorithm>
#include <cwchar>
#include <ctime>
#include <string>
#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/base.h>

extern KeyManager g_keyManager;

namespace {
constexpr double kActivityFollowBottomTolerance = 48.0;
constexpr double kActivityBubbleMaxWidth = 460.0;

winrt::Windows::UI::Xaml::Media::SolidColorBrush MakeBrush(uint8_t alpha, uint8_t red, uint8_t green, uint8_t blue) {
    return winrt::Windows::UI::Xaml::Media::SolidColorBrush(
        winrt::Windows::UI::ColorHelper::FromArgb(alpha, red, green, blue));
}

std::wstring FormatActivityTime(std::chrono::system_clock::time_point timestamp) {
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(timestamp);
    std::tm localTime{};
    if (localtime_safe(&localTime, &rawTime) != 0) {
        return {};
    }

    wchar_t buffer[32]{};
    if (std::wcsftime(buffer, cntof(buffer), L"%H:%M", &localTime) == 0) {
        return {};
    }
    return buffer;
}

std::wstring PayloadKindLabel(ClipboardActivityPayloadKind kind) {
    switch (kind) {
    case ClipboardActivityPayloadKind::Text:
        return CLP_W(CLP_UI_TEXT);
    case ClipboardActivityPayloadKind::PrivateText:
        return CLP_W(CLP_UI_PRIVATE_TEXT);
    case ClipboardActivityPayloadKind::Link:
        return CLP_W(CLP_UI_LINK);
    case ClipboardActivityPayloadKind::Image:
        return CLP_W(CLP_UI_IMAGE);
    case ClipboardActivityPayloadKind::Unsupported:
    default:
        return CLP_W(CLP_UI_UNSUPPORTED_CLIPBOARD_ITEM);
    }
}

winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage BitmapFromPngBytes(const std::vector<unsigned char>& bytes) {
    using namespace winrt::Windows::Storage::Streams;
    using namespace winrt::Windows::UI::Xaml::Media::Imaging;

    BitmapImage bitmap;
    if (bytes.empty()) {
        return bitmap;
    }

    try {
        InMemoryRandomAccessStream stream;
        DataWriter writer(stream.GetOutputStreamAt(0));
        writer.WriteBytes(winrt::array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        writer.StoreAsync().get();
        writer.FlushAsync().get();
        writer.DetachStream();
        stream.Seek(0);
        bitmap.SetSource(stream);
    } catch (const winrt::hresult_error&) {
    }

    return bitmap;
}
}

ClippPage::ClippPage(ClipboardActivityStore& activityStore, NavigateCallback showNetworkPage)
    : activityStore_(activityStore)
    , showNetworkPage_(std::move(showNetworkPage)) {
    BuildView();
}

ClippPage::~ClippPage() {
    OnDestroy();
}

winrt::Windows::UI::Xaml::Controls::Grid ClippPage::View() const {
    return root_;
}

void ClippPage::BuildView() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    root_ = Grid();
    root_.HorizontalAlignment(HorizontalAlignment::Stretch);
    root_.VerticalAlignment(VerticalAlignment::Stretch);

    auto activitySection = BuildActivitySection();
    activitySection.Margin(ThicknessHelper::FromLengths(24, 16, 24, 24));

    root_.Children().Append(activitySection);

    uiDispatcher_ = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    RefreshActivityItems(activityStore_.Snapshot());
}

winrt::Windows::UI::Xaml::Controls::Grid ClippPage::BuildActivitySection() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    Grid section;
    section.HorizontalAlignment(HorizontalAlignment::Stretch);
    section.VerticalAlignment(VerticalAlignment::Stretch);

    Grid listHost;
    listHost.HorizontalAlignment(HorizontalAlignment::Stretch);
    listHost.VerticalAlignment(VerticalAlignment::Stretch);

    activityItemsPanel_ = StackPanel();
    activityItemsPanel_.Orientation(Orientation::Vertical);
    activityItemsPanel_.Spacing(12);
    activityItemsPanel_.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));

    activityScroll_ = ScrollViewer();
    activityScroll_.HorizontalAlignment(HorizontalAlignment::Stretch);
    activityScroll_.VerticalAlignment(VerticalAlignment::Stretch);
    activityScroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    activityScroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    activityScroll_.Content(activityItemsPanel_);

    activityEmptyState_ = StackPanel();
    activityEmptyState_.Orientation(Orientation::Vertical);
    activityEmptyState_.Spacing(10);
    activityEmptyState_.HorizontalAlignment(HorizontalAlignment::Center);
    activityEmptyState_.VerticalAlignment(VerticalAlignment::Center);
    activityEmptyState_.Margin(ThicknessHelper::FromLengths(24, 24, 24, 24));

    activityEmptyMessage_ = TextBlock();
    activityEmptyMessage_.FontSize(14);
    activityEmptyMessage_.Opacity(0.65);
    activityEmptyMessage_.TextWrapping(TextWrapping::WrapWholeWords);
    activityEmptyMessage_.HorizontalAlignment(HorizontalAlignment::Center);

    activityEmptyNetworkButton_ = Button();
    activityEmptyNetworkButton_.Content(winrt::box_value(winrt::hstring{ CLP_W(CLP_UI_NETWORK) }));
    activityEmptyNetworkButton_.HorizontalAlignment(HorizontalAlignment::Center);
    activityEmptyNetworkButton_.Padding(ThicknessHelper::FromLengths(16, 6, 16, 6));
    activityEmptyNetworkButton_.Click([this](auto const&, auto const&) {
        if (showNetworkPage_) {
            showNetworkPage_();
        }
    });

    activityEmptyState_.Children().Append(activityEmptyMessage_);
    activityEmptyState_.Children().Append(activityEmptyNetworkButton_);

    listHost.Children().Append(activityScroll_);
    listHost.Children().Append(activityEmptyState_);

    section.Children().Append(listHost);
    UpdateActivityEmptyState();
    return section;
}

winrt::Windows::UI::Xaml::Controls::Grid ClippPage::BuildActivityRow(uint64_t itemID) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    const auto display = activityStore_.DisplayItem(itemID);
    if (!display) {
        return Grid{ nullptr };
    }

    const bool isOutgoing = display->header.direction == ClipboardActivityDirection::Outgoing;

    Grid row;
    row.HorizontalAlignment(HorizontalAlignment::Stretch);

    Grid bubble;
    bubble.MaxWidth(kActivityBubbleMaxWidth);
    bubble.HorizontalAlignment(isOutgoing ? HorizontalAlignment::Right : HorizontalAlignment::Left);
    bubble.CornerRadius(CornerRadius{ 8 });
    bubble.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
    bubble.Background(isOutgoing ? MakeBrush(34, 0, 120, 215) : MakeBrush(24, 127, 127, 127));

    StackPanel content;
    content.Orientation(Orientation::Vertical);
    content.Spacing(7);

    TextBlock meta;
    std::wstring deviceName = display->header.deviceName;
    if (deviceName.empty()) {
        deviceName = isOutgoing ? CLP_W(CLP_UI_THIS_DEVICE) : CLP_W(CLP_UI_UNKNOWN_HOST);
    }
    meta.Text(deviceName + L" - " + FormatActivityTime(display->header.timestamp));
    meta.FontSize(12);
    meta.Opacity(0.68);
    meta.TextWrapping(TextWrapping::WrapWholeWords);
    content.Children().Append(meta);

    if (display->kind == ClipboardActivityPayloadKind::Image && !display->imagePngData.empty()) {
        Image image;
        image.Source(BitmapFromPngBytes(display->imagePngData));
        image.Stretch(Stretch::Uniform);
        image.MaxWidth(kActivityBubbleMaxWidth - 24);
        image.MaxHeight(260);
        image.HorizontalAlignment(HorizontalAlignment::Left);
        content.Children().Append(image);

        TextBlock kindLabel;
        kindLabel.Text(PayloadKindLabel(display->kind));
        kindLabel.FontSize(12);
        kindLabel.Opacity(0.68);
        content.Children().Append(kindLabel);
    } else {
        if (display->kind == ClipboardActivityPayloadKind::Link && !display->linkHost.empty()) {
            TextBlock host;
            host.Text(display->linkHost);
            host.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            host.TextWrapping(TextWrapping::WrapWholeWords);
            content.Children().Append(host);
        } else if (display->kind == ClipboardActivityPayloadKind::PrivateText ||
                   display->kind == ClipboardActivityPayloadKind::Unsupported) {
            TextBlock kindLabel;
            kindLabel.Text(PayloadKindLabel(display->kind));
            kindLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            kindLabel.TextWrapping(TextWrapping::WrapWholeWords);
            content.Children().Append(kindLabel);
        }

        TextBlock preview;
        preview.Text(display->previewText.empty() ? PayloadKindLabel(display->kind) : display->previewText);
        preview.TextWrapping(TextWrapping::WrapWholeWords);
        preview.IsTextSelectionEnabled(display->kind != ClipboardActivityPayloadKind::PrivateText);
        preview.MaxLines(8);
        preview.TextTrimming(TextTrimming::WordEllipsis);
        content.Children().Append(preview);
    }

    Button copyButton;
    copyButton.Content(winrt::box_value(winrt::hstring{ CLP_W(CLP_UI_COPY) }));
    copyButton.HorizontalAlignment(HorizontalAlignment::Right);
    copyButton.Padding(ThicknessHelper::FromLengths(10, 4, 10, 4));
    copyButton.Click([this, itemID](auto const&, auto const&) {
        CopyActivityItem(itemID);
    });
    content.Children().Append(copyButton);

    bubble.Children().Append(content);
    row.Children().Append(bubble);
    return row;
}

void ClippPage::RefreshActivityItems(const std::vector<ClipboardActivityItemHeader>& items) {
    if (!activityItemsPanel_) {
        return;
    }

    activityItemsPanel_.Children().Clear();
    activityItemIDs_.clear();

    for (const auto& item : items) {
        auto row = BuildActivityRow(item.id);
        if (!row) {
            continue;
        }
        activityItemsPanel_.Children().Append(row);
        activityItemIDs_.push_back(item.id);
    }

    SetActivityEmptyMessageVisible(activityItemIDs_.empty());
    ScrollActivityToBottom();
}

void ClippPage::AddActivityItem(uint64_t itemID) {
    if (!activityItemsPanel_ || itemID == 0) {
        return;
    }

    if (std::find(activityItemIDs_.begin(), activityItemIDs_.end(), itemID) != activityItemIDs_.end()) {
        return;
    }

    const bool shouldFollow = IsActivityNearBottom();
    auto row = BuildActivityRow(itemID);
    if (!row) {
        return;
    }

    activityItemsPanel_.Children().Append(row);
    activityItemIDs_.push_back(itemID);
    SetActivityEmptyMessageVisible(false);

    if (shouldFollow) {
        ScrollActivityToBottom();
    }
}

void ClippPage::RemoveActivityItem(uint64_t itemID) {
    if (!activityItemsPanel_) {
        return;
    }

    const auto found = std::find(activityItemIDs_.begin(), activityItemIDs_.end(), itemID);
    if (found == activityItemIDs_.end()) {
        return;
    }

    const auto index = static_cast<uint32_t>(found - activityItemIDs_.begin());
    activityItemsPanel_.Children().RemoveAt(index);
    activityItemIDs_.erase(found);
    SetActivityEmptyMessageVisible(activityItemIDs_.empty());
}

void ClippPage::ClearActivityItems() {
    if (!activityItemsPanel_) {
        return;
    }

    activityItemsPanel_.Children().Clear();
    activityItemIDs_.clear();
    SetActivityEmptyMessageVisible(true);
}

void ClippPage::SetActivityEmptyMessageVisible(bool visible) {
    if (!activityEmptyState_) {
        return;
    }

    UpdateActivityEmptyState();
    activityEmptyState_.Visibility(visible
        ? winrt::Windows::UI::Xaml::Visibility::Visible
        : winrt::Windows::UI::Xaml::Visibility::Collapsed);
}

void ClippPage::UpdateActivityEmptyState() {
    if (!activityEmptyMessage_ || !activityEmptyNetworkButton_) {
        return;
    }

    const bool haveNetworkKey = g_keyManager.HaveNetworkKey();
    activityEmptyMessage_.Text(haveNetworkKey
        ? CLP_W(CLP_UI_CLIPBOARD_EMPTY)
        : CLP_W(CLP_UI_NO_NETWORK_KEY_CONFIGURED));
    activityEmptyNetworkButton_.Visibility(haveNetworkKey
        ? winrt::Windows::UI::Xaml::Visibility::Collapsed
        : winrt::Windows::UI::Xaml::Visibility::Visible);
}

bool ClippPage::IsActivityNearBottom() const {
    if (!activityScroll_) {
        return true;
    }

    const double scrollableHeight = activityScroll_.ScrollableHeight();
    if (scrollableHeight <= 0) {
        return true;
    }

    return (scrollableHeight - activityScroll_.VerticalOffset()) <= kActivityFollowBottomTolerance;
}

void ClippPage::ScrollActivityToBottom() const {
    if (!activityScroll_) {
        return;
    }

    activityScroll_.UpdateLayout();
    activityScroll_.ChangeView(nullptr, activityScroll_.ScrollableHeight(), nullptr, true);
}

void ClippPage::CopyActivityItem(uint64_t itemID) {
    auto payload = activityStore_.PayloadForClipboard(itemID);
    if (!payload) {
        return;
    }

    SetClipboardData(*payload, true, activityStore_.PayloadReference(itemID));
}

void ClippPage::OnShown() {
    BeginActivityNotifications();
    if (uiDispatcher_) {
        uiDispatcher_.TryEnqueue([this]() {
            UpdateActivityEmptyState();
            RefreshActivityItems(activityStore_.Snapshot());
        });
    }
}

void ClippPage::OnHidden() {
    EndActivityNotifications();
}

void ClippPage::OnDestroy() {
    OnHidden();
    activityItemsPanel_ = nullptr;
    activityScroll_ = nullptr;
    activityEmptyState_ = nullptr;
    activityEmptyMessage_ = nullptr;
    activityEmptyNetworkButton_ = nullptr;
}

void ClippPage::OnNetworkKeyChanged() {
    if (uiDispatcher_) {
        uiDispatcher_.TryEnqueue([this]() {
            UpdateActivityEmptyState();
        });
    }
}

void ClippPage::BeginActivityNotifications() {
    if (activityWatcherID_ != 0) {
        return;
    }

    const auto registration = activityStore_.QueryAndRegister(ClipboardActivityWatcher, this);
    activityWatcherID_ = registration.watcherID;
    RefreshActivityItems(registration.items);
}

void ClippPage::EndActivityNotifications() {
    if (activityWatcherID_ == 0) {
        return;
    }

    activityStore_.Unregister(activityWatcherID_);
    activityWatcherID_ = 0;
}

void ClippPage::ClipboardActivityWatcher(const ClipboardActivityUpdate& update, void* userData) {
    auto* page = reinterpret_cast<ClippPage*>(userData);
    if (!page || !page->uiDispatcher_) {
        return;
    }

    page->uiDispatcher_.TryEnqueue([page, update]() {
        switch (update.type) {
        case ClipboardActivityUpdate::Type::Added:
            page->AddActivityItem(update.itemID);
            break;
        case ClipboardActivityUpdate::Type::Removed:
            page->RemoveActivityItem(update.itemID);
            break;
        case ClipboardActivityUpdate::Type::Cleared:
            page->ClearActivityItems();
            break;
        }
    });
}
