#include "NetworkView.h"

#ifdef __APPLE__

#include "UiHelpers.h"
#include "platform/uistrings.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#import <AppKit/AppKit.h>

MacOSNetworkView::MacOSNetworkView(PeerDisplay& peerDisplay, std::function<void()> keyViewChangedHandler)
    : peerDisplay_(peerDisplay)
    , keyViewChangedHandler_(std::move(keyViewChangedHandler)) {
    BuildView();
}

NSView* MacOSNetworkView::View() const {
    return container_;
}

void MacOSNetworkView::Poll() {
    auto items = peerDisplay_.Query();
    std::sort(items.begin(), items.end(), ItemLess);

    const auto now = std::chrono::steady_clock::now();
    bool keyViewsChanged = false;

    for (std::size_t index = entries_.size(); index > 0; --index) {
        const auto entryIndex = index - 1;
        const auto found = std::find_if(items.begin(), items.end(), [&](const PeerDisplayItem& item) {
            return SameHostID(entries_[entryIndex].item, item);
        });
        if (found == items.end()) {
            NSView* view = entries_[entryIndex].view->View();
            [itemsPanel_ removeArrangedSubview:view];
            [view removeFromSuperview];
            entries_.erase(entries_.begin() + entryIndex);
            keyViewsChanged = true;
        }
    }

    for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
        const auto& item = items[itemIndex];
        const auto existingIndex = FindEntryByHostID(item);
        if (existingIndex == entries_.size()) {
            Entry entry;
            entry.item = item;
            entry.view = std::make_unique<MacOSNetworkItemView>(item);
            entry.view->RefreshConnectedFor(now);
            NSView* entryView = entry.view->View();
            [itemsPanel_ insertArrangedSubview:entryView atIndex:static_cast<NSUInteger>(itemIndex)];
            [entryView.widthAnchor constraintEqualToAnchor:itemsPanel_.widthAnchor].active = YES;
            entries_.insert(entries_.begin() + itemIndex, std::move(entry));
            keyViewsChanged = true;
            continue;
        }

        if (existingIndex != itemIndex) {
            auto entry = std::move(entries_[existingIndex]);
            entries_.erase(entries_.begin() + existingIndex);
            [itemsPanel_ removeArrangedSubview:entry.view->View()];
            [entry.view->View() removeFromSuperview];

            const auto insertIndex = itemIndex > entries_.size() ? entries_.size() : itemIndex;
            [itemsPanel_ insertArrangedSubview:entry.view->View() atIndex:static_cast<NSUInteger>(insertIndex)];
            entries_.insert(entries_.begin() + insertIndex, std::move(entry));
            keyViewsChanged = true;
        }

        UpdateEntry(entries_[itemIndex], item, now);
    }

    SetEmptyMessageVisible(entries_.empty());
    if (keyViewsChanged && keyViewChangedHandler_) {
        keyViewChangedHandler_();
    }
}

void MacOSNetworkView::AppendKeyViews(NSMutableArray<NSView*>* keyViews) const {
    for (const auto& entry : entries_) {
        NSButton* disclosureButton = entry.view->DisclosureButton();
        if (disclosureButton != nil) {
            [keyViews addObject:disclosureButton];
        }
    }
}

void MacOSNetworkView::BuildView() {
    container_ = [[NSStackView alloc] initWithFrame:NSZeroRect];
    container_.translatesAutoresizingMaskIntoConstraints = NO;
    container_.orientation = NSUserInterfaceLayoutOrientationVertical;
    container_.alignment = NSLayoutAttributeLeading;
    container_.distribution = NSStackViewDistributionFill;
    container_.spacing = 8.0;
    container_.detachesHiddenViews = YES;

    NSTextField* heading = [NSTextField labelWithString:CLP_NS(CLP_UI_PEERS)];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    emptyMessage_ = MacOSMakeWrappingLabel(CLP_NS(CLP_UI_NO_PEERS_HELP),
                                           13.0,
                                           [NSColor secondaryLabelColor]);

    itemsPanel_ = [[NSStackView alloc] initWithFrame:NSZeroRect];
    itemsPanel_.translatesAutoresizingMaskIntoConstraints = NO;
    itemsPanel_.orientation = NSUserInterfaceLayoutOrientationVertical;
    itemsPanel_.alignment = NSLayoutAttributeLeading;
    itemsPanel_.distribution = NSStackViewDistributionFill;
    itemsPanel_.spacing = 8.0;

    [container_ addArrangedSubview:heading];
    [container_ addArrangedSubview:emptyMessage_];
    [container_ addArrangedSubview:itemsPanel_];

    [heading.widthAnchor constraintLessThanOrEqualToAnchor:container_.widthAnchor].active = YES;
    [emptyMessage_.widthAnchor constraintEqualToAnchor:container_.widthAnchor].active = YES;
    [itemsPanel_.widthAnchor constraintEqualToAnchor:container_.widthAnchor].active = YES;
}

bool MacOSNetworkView::ItemLess(const PeerDisplayItem& left, const PeerDisplayItem& right) {
    return left.hostID < right.hostID;
}

bool MacOSNetworkView::SameHostID(const PeerDisplayItem& left, const PeerDisplayItem& right) {
    return left.hostID == right.hostID;
}

std::size_t MacOSNetworkView::FindEntryByHostID(const PeerDisplayItem& item) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return SameHostID(entry.item, item);
    });
    return found == entries_.end() ? entries_.size() : static_cast<std::size_t>(found - entries_.begin());
}

void MacOSNetworkView::SetEmptyMessageVisible(bool visible) {
    emptyMessage_.hidden = !visible;
}

void MacOSNetworkView::UpdateEntry(Entry& entry, const PeerDisplayItem& item, std::chrono::steady_clock::time_point now) {
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

#endif
