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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>       // IStream, IID_PPV_ARGS, COM goop
#include <shcore.h>        // CreateStreamOverRandomAccessStream (synchronous COM bridge
                           // we use to write image bytes into an in-memory stream
                           // without tripping the C++/WinRT STA-blocking-wait assert
                           // that fires on DataWriter::StoreAsync().get()).

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/base.h>

extern KeyManager g_keyManager;

namespace {
namespace Automation = winrt::Windows::UI::Xaml::Automation;
constexpr double kActivityFollowTopTolerance = 48.0;
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
    case ClipboardActivityPayloadKind::PrivatePlaceholder:
        return CLP_W(CLP_UI_PRIVATE_PLACEHOLDER_TITLE);
    case ClipboardActivityPayloadKind::Link:
        return CLP_W(CLP_UI_LINK);
    case ClipboardActivityPayloadKind::Image:
        return CLP_W(CLP_UI_IMAGE);
    case ClipboardActivityPayloadKind::Unsupported:
    default:
        return CLP_W(CLP_UI_UNSUPPORTED_CLIPBOARD_ITEM);
    }
}

winrt::Windows::UI::Xaml::Media::Imaging::BitmapImage BitmapFromImageBytes(
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

    root_.Children().Append(BuildActivitySection());

    uiDispatcher_ = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    RefreshActivityItems(activityStore_.Snapshot());
}

winrt::Windows::UI::Xaml::Controls::Grid ClippPage::BuildActivitySection() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    Grid section;
    section.HorizontalAlignment(HorizontalAlignment::Stretch);
    section.VerticalAlignment(VerticalAlignment::Stretch);

    activityItemsPanel_ = StackPanel();
    activityItemsPanel_.Orientation(Orientation::Vertical);
    activityItemsPanel_.Spacing(12);
    activityItemsPanel_.Padding(ThicknessHelper::FromLengths(24, 16, 24, 24));

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

    section.Children().Append(activityScroll_);
    section.Children().Append(activityEmptyState_);
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

    const bool isOutgoing = display->direction == ClipboardActivityDirection::Outgoing;
    const bool isPrivatePlaceholder =
        display->kind == ClipboardActivityPayloadKind::PrivatePlaceholder;
    const bool showCopyAction = !isPrivatePlaceholder;

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
    const std::wstring metaText = display->deviceName + L" - " + FormatActivityTime(display->header.timestamp);
    meta.Text(metaText);
    meta.FontSize(12);
    meta.Opacity(0.68);
    meta.TextWrapping(TextWrapping::WrapWholeWords);

    FontIcon copyIcon;
    copyIcon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    copyIcon.Glyph(L"\xE8C8");
    copyIcon.FontSize(13);
    copyIcon.Width(28);
    copyIcon.Height(28);
    copyIcon.VerticalAlignment(VerticalAlignment::Center);
    copyIcon.HorizontalAlignment(HorizontalAlignment::Center);
    copyIcon.Opacity(0.0);
    copyIcon.IsHitTestVisible(false);
    // Placeholder rows have no copyable content; suppress the hover affordance.
    copyIcon.Visibility(showCopyAction ? Visibility::Visible : Visibility::Collapsed);

    Grid header;
    ColumnDefinition metaColumn;
    metaColumn.Width(GridLength{ 1, GridUnitType::Star });
    ColumnDefinition actionColumn;
    actionColumn.Width(GridLength{ 1, GridUnitType::Auto });
    header.ColumnDefinitions().Append(metaColumn);
    header.ColumnDefinitions().Append(actionColumn);
    Grid::SetColumn(meta, 0);
    Grid::SetColumn(copyIcon, 1);
    header.Children().Append(meta);
    header.Children().Append(copyIcon);
    content.Children().Append(header);

    if (display->kind == ClipboardActivityPayloadKind::Image && display->imageData && !display->imageData->empty()) {
        Image image;
        const int32_t decodeWidth = static_cast<int32_t>(kActivityBubbleMaxWidth - 24);
        image.Source(BitmapFromImageBytes(*display->imageData, decodeWidth));
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
                   display->kind == ClipboardActivityPayloadKind::PrivatePlaceholder ||
                   display->kind == ClipboardActivityPayloadKind::Unsupported) {
            StackPanel labelRow;
            labelRow.Orientation(Orientation::Horizontal);
            labelRow.Spacing(8);

            TextBlock kindLabel;
            kindLabel.Text(PayloadKindLabel(display->kind));
            kindLabel.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            kindLabel.TextWrapping(TextWrapping::WrapWholeWords);
            kindLabel.VerticalAlignment(VerticalAlignment::Center);
            labelRow.Children().Append(kindLabel);

            if (display->sourceMarked) {
                // Small pill that distinguishes "marker-driven" private items
                // from heuristic-driven ones. Reader can tell at a glance whether
                // the source app explicitly asked for privacy or the receive-side
                // heuristic guessed.
                Border badge;
                badge.Padding(ThicknessHelper::FromLengths(8, 1, 8, 1));
                badge.CornerRadius(CornerRadius{ 8 });
                badge.Background(MakeBrush(40, 198, 116, 0));
                badge.VerticalAlignment(VerticalAlignment::Center);

                TextBlock badgeText;
                badgeText.Text(CLP_W(CLP_UI_PRIVATE_BADGE));
                badgeText.FontSize(11);
                badgeText.Opacity(0.95);
                badge.Child(badgeText);
                labelRow.Children().Append(badge);
            }

            content.Children().Append(labelRow);
        }

        if (display->kind == ClipboardActivityPayloadKind::PrivatePlaceholder) {
            TextBlock detail;
            detail.Text(CLP_W(CLP_UI_PRIVATE_PLACEHOLDER_DETAIL));
            detail.FontSize(12);
            detail.Opacity(0.7);
            detail.TextWrapping(TextWrapping::WrapWholeWords);
            content.Children().Append(detail);
        } else {
            TextBlock preview;
            preview.Text(display->previewText.empty() ? PayloadKindLabel(display->kind) : display->previewText);
            preview.TextWrapping(TextWrapping::WrapWholeWords);
            preview.IsTextSelectionEnabled(false);
            preview.MaxLines(8);
            preview.TextTrimming(TextTrimming::WordEllipsis);
            content.Children().Append(preview);
        }
    }

    const auto setCopyIconVisible = [copyIcon](bool visible) {
        copyIcon.Opacity(visible ? 1.0 : 0.0);
    };

    Button rowButton;
    rowButton.HorizontalAlignment(HorizontalAlignment::Stretch);
    rowButton.VerticalAlignment(VerticalAlignment::Stretch);
    rowButton.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    rowButton.VerticalContentAlignment(VerticalAlignment::Stretch);
    rowButton.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
    rowButton.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
    rowButton.Background(MakeBrush(0, 0, 0, 0));
    rowButton.Content(bubble);

    if (showCopyAction) {
        rowButton.Click([this, itemID](auto const&, auto const&) {
            CopyActivityItem(itemID);
        });
        rowButton.PointerEntered([setCopyIconVisible](auto const&, auto const&) {
            setCopyIconVisible(true);
        });
        rowButton.PointerExited([setCopyIconVisible](auto const&, auto const&) {
            setCopyIconVisible(false);
        });
        rowButton.GotFocus([setCopyIconVisible](auto const&, auto const&) {
            setCopyIconVisible(true);
        });
        rowButton.LostFocus([setCopyIconVisible](auto const&, auto const&) {
            setCopyIconVisible(false);
        });

        MenuFlyout contextMenu;
        MenuFlyoutItem copyMenuItem;
        copyMenuItem.Text(winrt::hstring{ CLP_W(CLP_UI_COPY) });
        copyMenuItem.Click([this, itemID](auto const&, auto const&) {
            CopyActivityItem(itemID);
        });
        contextMenu.Items().Append(copyMenuItem);
        rowButton.ContextFlyout(contextMenu);
        ToolTipService::SetToolTip(rowButton, winrt::box_value(winrt::hstring{ CLP_W(CLP_UI_COPY) }));
        Automation::AutomationProperties::SetName(rowButton, winrt::hstring{ metaText });
        Automation::AutomationProperties::SetHelpText(rowButton, winrt::hstring{ CLP_W(CLP_UI_COPY) });
    } else {
        // Placeholder rows are informational — no click, no copy menu, no tooltip.
        rowButton.IsTabStop(false);
        Automation::AutomationProperties::SetName(rowButton, winrt::hstring{ metaText });
    }

    bubble.Children().Append(content);
    row.Children().Append(rowButton);
    return row;
}

void ClippPage::RefreshActivityItems(const std::vector<ClipboardActivityItemHeader>& items) {
    if (!activityItemsPanel_) {
        return;
    }

    activityItemsPanel_.Children().Clear();
    activityItemIDs_.clear();

    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        auto row = BuildActivityRow(item->id);
        if (!row) {
            continue;
        }
        activityItemsPanel_.Children().Append(row);
        activityItemIDs_.push_back(item->id);
    }

    SetActivityEmptyMessageVisible(activityItemIDs_.empty());
    ScrollActivityToTop();
}

void ClippPage::AddActivityItem(uint64_t itemID) {
    if (!activityItemsPanel_ || itemID == 0) {
        return;
    }

    if (std::find(activityItemIDs_.begin(), activityItemIDs_.end(), itemID) != activityItemIDs_.end()) {
        return;
    }

    const bool shouldFollow = IsActivityNearTop();
    auto row = BuildActivityRow(itemID);
    if (!row) {
        return;
    }

    activityItemsPanel_.Children().InsertAt(0, row);
    activityItemIDs_.insert(activityItemIDs_.begin(), itemID);
    SetActivityEmptyMessageVisible(false);

    if (shouldFollow) {
        ScrollActivityToTop();
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

bool ClippPage::IsActivityNearTop() const {
    if (!activityScroll_) {
        return true;
    }

    return activityScroll_.VerticalOffset() <= kActivityFollowTopTolerance;
}

void ClippPage::ScrollActivityToTop() const {
    if (!activityScroll_) {
        return;
    }

    activityScroll_.UpdateLayout();
    activityScroll_.ChangeView(nullptr, 0.0, nullptr, true);
}

void ClippPage::CopyActivityItem(uint64_t itemID) {
    auto payload = activityStore_.PayloadReference(itemID);
    if (!payload) {
        return;
    }

    // Source-marked-private placeholder payloads carry no content and exist
    // only to inform the user that something happened. Writing an empty
    // clipboard would be both useless and destructive of whatever's there now.
    if ((payload->meta.flags & NetworkDefs::CLPM_FLAG_SOURCE_MARKED_PRIVATE) != 0
        && payload->EncodedBytes().empty()) {
        return;
    }

    SetClipboardData(std::move(payload), true);
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
