#include "ClippPage.h"

#include "Clipboard.h"
#include "ClipboardActions.h"
#include "KeyManager.h"
#include "platform/uiClippPage.h"
#include "platform/uistrings.h"
#include "utils.h"

#include <algorithm>
#include <cwchar>
#include <ctime>
#include <memory>
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
    // Masked private text with content present gets a peek toggle; placeholder
    // rows carry no bytes, so there is nothing to reveal.
    const bool showPeekAction =
        display->kind == ClipboardActivityPayloadKind::PrivateText
        && !display->revealedPreviewText.empty();

    Grid row;
    row.HorizontalAlignment(HorizontalAlignment::Stretch);

    // The bubble splits into text content and a right-hand action rail so the
    // peek toggle can sit directly below the copy glyph.
    Grid bubble;
    bubble.MaxWidth(kActivityBubbleMaxWidth);
    bubble.HorizontalAlignment(isOutgoing ? HorizontalAlignment::Right : HorizontalAlignment::Left);
    bubble.CornerRadius(CornerRadius{ 8 });
    bubble.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
    bubble.Background(isOutgoing ? MakeBrush(34, 0, 120, 215) : MakeBrush(24, 127, 127, 127));
    bubble.ColumnSpacing(8);

    ColumnDefinition contentColumn;
    contentColumn.Width(GridLength{ 1, GridUnitType::Star });
    ColumnDefinition railColumn;
    railColumn.Width(GridLength{ 1, GridUnitType::Auto });
    bubble.ColumnDefinitions().Append(contentColumn);
    bubble.ColumnDefinitions().Append(railColumn);

    StackPanel content;
    content.Orientation(Orientation::Vertical);
    content.Spacing(7);
    Grid::SetColumn(content, 0);

    TextBlock meta;
    const std::wstring metaText = display->deviceName + L" - " + FormatActivityTime(display->header.timestamp);
    meta.Text(metaText);
    meta.FontSize(12);
    meta.Opacity(0.68);
    meta.TextWrapping(TextWrapping::WrapWholeWords);
    content.Children().Append(meta);

    const auto makeRailButton = [](const wchar_t* glyph, const wchar_t* label) {
        FontIcon icon;
        icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
        icon.Glyph(glyph);
        icon.FontSize(13);
        Button button;
        button.Content(icon);
        button.Width(28);
        button.Height(28);
        button.MinWidth(0);
        button.MinHeight(0);
        button.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
        button.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
        button.Background(MakeBrush(0, 0, 0, 0));
        button.Opacity(0.0);
        button.IsHitTestVisible(false);
        const winrt::hstring name{ label };
        ToolTipService::SetToolTip(button, winrt::box_value(name));
        Automation::AutomationProperties::SetName(button, name);
        return button;
    };

    // The rail glyphs are the one-click affordances; a plain row click does
    // NOTHING on purpose — copy is an MRU re-share that reorders every list
    // on the mesh, far too intrusive for a stray click. Double-click and the
    // copy glyph share the action.
    Button copyButton{ nullptr };
    if (showCopyAction) {
        copyButton = makeRailButton(L"\xE8C8", CLP_W(CLP_UI_COPY));
        copyButton.Click([this, itemID](auto const&, auto const&) {
            CopyActivityItem(itemID);
        });
    }
    Button deleteButton = makeRailButton(L"\xE74D", CLP_W(CLP_UI_DELETE));
    deleteButton.Click([itemID](auto const&, auto const&) {
        clipp::DeleteActivityItemEverywhere(itemID);
    });

    StackPanel actionRail;
    actionRail.Orientation(Orientation::Vertical);
    actionRail.Spacing(4);
    actionRail.VerticalAlignment(VerticalAlignment::Top);
    Grid::SetColumn(actionRail, 1);
    if (copyButton) {
        actionRail.Children().Append(copyButton);
    }

    Button peekButton{ nullptr };
    FontIcon peekIcon{ nullptr };
    if (showPeekAction) {
        peekIcon = FontIcon();
        peekIcon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
        peekIcon.FontSize(13);

        peekButton = Button();
        peekButton.Content(peekIcon);
        peekButton.Width(28);
        peekButton.Height(28);
        peekButton.MinWidth(0);
        peekButton.MinHeight(0);
        peekButton.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
        peekButton.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 0));
        peekButton.Background(MakeBrush(0, 0, 0, 0));
        peekButton.Opacity(0.0);
        peekButton.IsHitTestVisible(false);
        actionRail.Children().Append(peekButton);
    }

    // Delete rides every row — placeholders included; removing the trace
    // everywhere is the whole point of the affordance.
    actionRail.Children().Append(deleteButton);

    TextBlock preview{ nullptr };

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
            preview = TextBlock();
            preview.Text(display->previewText.empty() ? PayloadKindLabel(display->kind) : display->previewText);
            // Private strings are whitespace-free, so WrapWholeWords would peek
            // a single truncated line; allow mid-word breaks for those rows.
            preview.TextWrapping(showPeekAction ? TextWrapping::Wrap : TextWrapping::WrapWholeWords);
            preview.IsTextSelectionEnabled(false);
            preview.MaxLines(8);
            preview.TextTrimming(TextTrimming::WordEllipsis);
            content.Children().Append(preview);
        }
    }

    // Hover/focus state shared by the row and the rail buttons, which follow
    // it — except that an active peek stays visible so its toggled state is
    // never ambiguous.
    auto pointerOverRow = std::make_shared<bool>(false);
    const auto updateActionIcons = [copyButton, deleteButton, peekButton, itemID, pointerOverRow]() {
        const bool over = *pointerOverRow;
        if (copyButton) {
            copyButton.Opacity(over ? 1.0 : 0.0);
            copyButton.IsHitTestVisible(over);
        }
        deleteButton.Opacity(over ? 1.0 : 0.0);
        deleteButton.IsHitTestVisible(over);
        if (peekButton) {
            const bool show = over || uiClippPage::IsItemPeeked(itemID);
            peekButton.Opacity(show ? 1.0 : 0.0);
            peekButton.IsHitTestVisible(show);
        }
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

    // Hover affordances apply to every row — delete is universal; the copy
    // wiring below stays gated on copyable content.
    rowButton.PointerEntered([updateActionIcons, pointerOverRow](auto const&, auto const&) {
        *pointerOverRow = true;
        updateActionIcons();
    });
    rowButton.PointerExited([updateActionIcons, pointerOverRow](auto const&, auto const&) {
        *pointerOverRow = false;
        updateActionIcons();
    });
    rowButton.GotFocus([updateActionIcons, pointerOverRow](auto const&, auto const&) {
        *pointerOverRow = true;
        updateActionIcons();
    });
    rowButton.LostFocus([updateActionIcons, pointerOverRow](auto const&, auto const&) {
        *pointerOverRow = false;
        updateActionIcons();
    });

    if (showCopyAction) {
        rowButton.DoubleTapped([this, itemID](auto const&, auto const&) {
            CopyActivityItem(itemID);
        });

        MenuFlyout contextMenu;
        MenuFlyoutItem copyMenuItem;
        copyMenuItem.Text(winrt::hstring{ CLP_W(CLP_UI_COPY) });
        copyMenuItem.Click([this, itemID](auto const&, auto const&) {
            CopyActivityItem(itemID);
        });
        contextMenu.Items().Append(copyMenuItem);

        if (showPeekAction && preview) {
            MenuFlyoutItem peekMenuItem;
            const std::wstring maskedText = display->previewText;
            const std::wstring revealedText = display->revealedPreviewText;
            const auto applyPeekState =
                [preview, peekIcon, peekButton, peekMenuItem, maskedText, revealedText](bool revealed) {
                    preview.Text(revealed ? revealedText : maskedText);
                    // Segoe MDL2 "RedEye" / "Hide".
                    peekIcon.Glyph(revealed ? L"\xED1A" : L"\xE7B3");
                    const winrt::hstring label{ revealed ? CLP_W(CLP_UI_PEEK_HIDE) : CLP_W(CLP_UI_PEEK) };
                    ToolTipService::SetToolTip(peekButton, winrt::box_value(label));
                    Automation::AutomationProperties::SetName(peekButton, label);
                    peekMenuItem.Text(label);
                };
            const auto togglePeek = [itemID, applyPeekState, updateActionIcons]() {
                applyPeekState(uiClippPage::ToggleItemPeeked(itemID));
                updateActionIcons();
            };

            peekButton.Click([togglePeek](auto const&, auto const&) {
                togglePeek();
            });
            peekButton.GotFocus([updateActionIcons, pointerOverRow](auto const&, auto const&) {
                *pointerOverRow = true;
                updateActionIcons();
            });
            peekButton.LostFocus([updateActionIcons, pointerOverRow](auto const&, auto const&) {
                *pointerOverRow = false;
                updateActionIcons();
            });

            peekMenuItem.Click([togglePeek](auto const&, auto const&) {
                togglePeek();
            });
            contextMenu.Items().Append(peekMenuItem);

            applyPeekState(uiClippPage::IsItemPeeked(itemID));
            updateActionIcons();
        }

        MenuFlyoutItem deleteMenuItem;
        deleteMenuItem.Text(winrt::hstring{ CLP_W(CLP_UI_DELETE) });
        deleteMenuItem.Click([itemID](auto const&, auto const&) {
            // Mesh-wide, best-effort; the store's Removed event tears the row down.
            clipp::DeleteActivityItemEverywhere(itemID);
        });
        contextMenu.Items().Append(deleteMenuItem);

        rowButton.KeyDown([itemID](auto const&, winrt::Windows::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Delete) {
                args.Handled(true);
                clipp::DeleteActivityItemEverywhere(itemID);
            }
        });

        rowButton.ContextFlyout(contextMenu);
        Automation::AutomationProperties::SetName(rowButton, winrt::hstring{ metaText });
    } else {
        // Placeholder rows are informational — no click, no copy, no tooltip.
        // Still deletable, so the trace can be removed everywhere.
        rowButton.IsTabStop(false);
        MenuFlyout placeholderMenu;
        MenuFlyoutItem deleteOnlyItem;
        deleteOnlyItem.Text(winrt::hstring{ CLP_W(CLP_UI_DELETE) });
        deleteOnlyItem.Click([itemID](auto const&, auto const&) {
            clipp::DeleteActivityItemEverywhere(itemID);
        });
        placeholderMenu.Items().Append(deleteOnlyItem);
        rowButton.ContextFlyout(placeholderMenu);
        Automation::AutomationProperties::SetName(rowButton, winrt::hstring{ metaText });
    }

    bubble.Children().Append(content);
    bubble.Children().Append(actionRail);
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
    uiClippPage::ForgetPeekedItem(itemID);
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

    // Same focus re-anchor as MoveActivityItem: a removed focused row would
    // otherwise leave the island with null focus and a dead mouse wheel.
    if (!activityItemIDs_.empty()
        && winrt::Windows::UI::Xaml::Input::FocusManager::GetFocusedElement() == nullptr) {
        const uint32_t focusIndex =
            (std::min)(index, static_cast<uint32_t>(activityItemIDs_.size() - 1));
        if (const auto nextRow = activityItemsPanel_.Children().GetAt(focusIndex).try_as<winrt::Windows::UI::Xaml::Controls::Grid>()) {
            if (const auto control = nextRow.Children().GetAt(0).try_as<winrt::Windows::UI::Xaml::Controls::Control>()) {
                control.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
            }
        }
    }
}

void ClippPage::MoveActivityItem(uint64_t itemID) {
    if (!activityItemsPanel_) {
        return;
    }

    const auto movedPayload = activityStore_.PayloadReference(itemID);
    if (!movedPayload) {
        RemoveActivityItem(itemID);
        return;
    }

    // Take the old row out WITHOUT forgetting peek state — the id (and the
    // user's peek choice) survives a relocation.
    const auto found = std::find(activityItemIDs_.begin(), activityItemIDs_.end(), itemID);
    if (found != activityItemIDs_.end()) {
        const auto index = static_cast<uint32_t>(found - activityItemIDs_.begin());
        activityItemsPanel_.Children().RemoveAt(index);
        activityItemIDs_.erase(found);
    }

    const bool shouldFollow = IsActivityNearTop();
    auto row = BuildActivityRow(itemID);
    if (!row) {
        SetActivityEmptyMessageVisible(activityItemIDs_.empty());
        return;
    }

    // Panel is newest-first; insert before the first row whose payload
    // timestamp is at-or-below the moved one (ties: moved item on top).
    const uint64_t movedTs = movedPayload->meta.timestamp;
    uint32_t insertIndex = static_cast<uint32_t>(activityItemIDs_.size());
    for (uint32_t i = 0; i < activityItemIDs_.size(); ++i) {
        const auto payload = activityStore_.PayloadReference(activityItemIDs_[i]);
        if (payload && payload->meta.timestamp <= movedTs) {
            insertIndex = i;
            break;
        }
    }

    activityItemsPanel_.Children().InsertAt(insertIndex, row);
    activityItemIDs_.insert(activityItemIDs_.begin() + insertIndex, itemID);
    SetActivityEmptyMessageVisible(false);

    // Removing the (typically just-clicked, focused) old row drops XAML focus
    // to null, which kills mouse-wheel routing in the island until something
    // takes focus again. Re-anchor on the row's replacement.
    if (winrt::Windows::UI::Xaml::Input::FocusManager::GetFocusedElement() == nullptr) {
        if (const auto control = row.Children().GetAt(0).try_as<winrt::Windows::UI::Xaml::Controls::Control>()) {
            control.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
        }
    }

    if (shouldFollow) {
        ScrollActivityToTop();
    }
}

void ClippPage::ClearActivityItems() {
    uiClippPage::ForgetAllPeekedItems();
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
    // Copy-from-history is an MRU re-share: it sets the local clipboard AND
    // makes the item current mesh-wide (peer clipboards follow, lists
    // relocate). The placeholder/no-content guards live inside the helper.
    clipp::ReshareActivityItem(itemID);
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
    // Peeks are session-scoped: revealing a private item lasts until the
    // window goes away, then must be redone.
    uiClippPage::ForgetAllPeekedItems();
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
        case ClipboardActivityUpdate::Type::Moved:
            page->MoveActivityItem(update.itemID);
            break;
        }
    });
}
