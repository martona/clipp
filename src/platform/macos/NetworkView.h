#pragma once

#ifdef __APPLE__

#include "PeerDisplay.h"
#include "NetworkItem.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#import <AppKit/AppKit.h>

class MacOSNetworkView {
public:
    MacOSNetworkView(PeerDisplay& peerDisplay, std::function<void()> keyViewChangedHandler);

    NSView* View() const;
    void Poll();
    void AppendKeyViews(NSMutableArray<NSView*>* keyViews) const;

private:
    struct Entry {
        PeerDisplayItem item;
        std::unique_ptr<MacOSNetworkItemView> view;
    };

    void BuildView();
    static bool ItemLess(const PeerDisplayItem& left, const PeerDisplayItem& right);
    static bool SameHostID(const PeerDisplayItem& left, const PeerDisplayItem& right);
    std::size_t FindEntryByHostID(const PeerDisplayItem& item) const;
    void SetEmptyMessageVisible(bool visible);
    void UpdateEntry(Entry& entry, const PeerDisplayItem& item, std::chrono::steady_clock::time_point now);

    PeerDisplay& peerDisplay_;
    std::function<void()> keyViewChangedHandler_;
    NSStackView* container_ = nullptr;
    NSTextField* emptyMessage_ = nullptr;
    NSStackView* itemsPanel_ = nullptr;
    std::vector<Entry> entries_;
};

#endif
