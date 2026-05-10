#include "platform_win32_NetworkView.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>

NetworkView::NetworkView(PeerDisplay& peerDisplay)
    : peerDisplay_(peerDisplay) {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    container_ = StackPanel();
    container_.Orientation(Orientation::Vertical);
    container_.Spacing(8);

    TextBlock heading;
    heading.Text(L"Peers");
    heading.FontSize(16);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    container_.Children().Append(heading);

    emptyMessage_ = TextBlock();
    emptyMessage_.Text(L"Make sure your devices are using the exact same network name and secret. Both are case-sensitive.");
    emptyMessage_.FontSize(13);
    emptyMessage_.Opacity(0.7);
    emptyMessage_.Margin(ThicknessHelper::FromLengths(0, 0, 0, 4));
    emptyMessage_.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::Wrap);
    container_.Children().Append(emptyMessage_);

    itemsPanel_ = StackPanel();
    itemsPanel_.Orientation(Orientation::Vertical);
    itemsPanel_.Spacing(8);
    container_.Children().Append(itemsPanel_);
}

winrt::Windows::UI::Xaml::Controls::StackPanel NetworkView::View() const {
    return container_;
}

void NetworkView::Poll() {
    auto items = peerDisplay_.Query();
    std::sort(items.begin(), items.end(), ItemLess);

    const auto now = std::chrono::steady_clock::now();

    for (std::size_t index = entries_.size(); index > 0; --index) {
        const auto entryIndex = index - 1;
        const auto found = std::find_if(items.begin(), items.end(), [&](const PeerDisplayItem& item) {
            return SameHostID(entries_[entryIndex].item, item);
        });
        if (found == items.end()) {
            itemsPanel_.Children().RemoveAt(static_cast<uint32_t>(entryIndex));
            entries_.erase(entries_.begin() + entryIndex);
        }
    }

    for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
        const auto& item = items[itemIndex];
        const auto existingIndex = FindEntryByHostID(item);
        if (existingIndex == entries_.size()) {
            Entry entry;
            entry.item = item;
            entry.view = std::make_unique<NetworkItemView>(item);
            entry.view->RefreshConnectedFor(now);
            itemsPanel_.Children().InsertAt(static_cast<uint32_t>(itemIndex), entry.view->View());
            entries_.insert(entries_.begin() + itemIndex, std::move(entry));
            continue;
        }

        if (existingIndex != itemIndex) {
            auto entry = std::move(entries_[existingIndex]);
            entries_.erase(entries_.begin() + existingIndex);
            itemsPanel_.Children().RemoveAt(static_cast<uint32_t>(existingIndex));

            const auto insertIndex = itemIndex > entries_.size() ? entries_.size() : itemIndex;
            itemsPanel_.Children().InsertAt(static_cast<uint32_t>(insertIndex), entry.view->View());
            entries_.insert(entries_.begin() + insertIndex, std::move(entry));
        }

        UpdateEntry(entries_[itemIndex], item, now);
    }

    SetEmptyMessageVisible(entries_.empty());
}

bool NetworkView::ItemLess(const PeerDisplayItem& left, const PeerDisplayItem& right) {
	return left.hostID < right.hostID;
}

bool NetworkView::SameHostID(const PeerDisplayItem& left, const PeerDisplayItem& right) {
    return left.hostID == right.hostID;
}

std::size_t NetworkView::FindEntryByHostID(const PeerDisplayItem& item) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return SameHostID(entry.item, item);
    });
    return found == entries_.end() ? entries_.size() : static_cast<std::size_t>(found - entries_.begin());
}

void NetworkView::SetEmptyMessageVisible(bool visible) {
    emptyMessage_.Visibility(visible
        ? winrt::Windows::UI::Xaml::Visibility::Visible
        : winrt::Windows::UI::Xaml::Visibility::Collapsed);
}

void NetworkView::UpdateEntry(Entry& entry, const PeerDisplayItem& item, std::chrono::steady_clock::time_point now) {
    if (entry.item.hostName != item.hostName) {
        entry.view->UpdateHostName(item.hostName);
    }
    if (entry.item.hostID != item.hostID) {
        entry.view->UpdateHostID(item.hostID);
    }
    if (entry.item.hasIncomingConnection != item.hasIncomingConnection) {
        entry.view->UpdateIncomingConnection(item.hasIncomingConnection);
    }
    if (entry.item.hasOutgoingConnection != item.hasOutgoingConnection) {
        entry.view->UpdateOutgoingConnection(item.hasOutgoingConnection);
    }
    if (entry.item.bytesSent != item.bytesSent) {
        entry.view->UpdateBytesSent(item.bytesSent);
    }
    if (entry.item.bytesReceived != item.bytesReceived) {
        entry.view->UpdateBytesReceived(item.bytesReceived);
    }
    if (entry.item.connectedSince != item.connectedSince) {
        entry.view->UpdateConnectedSince(item.connectedSince);
    } else {
        entry.view->RefreshConnectedFor(now);
    }

    entry.item = item;
}
