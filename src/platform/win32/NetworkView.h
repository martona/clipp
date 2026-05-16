#pragma once

#include "PeerDisplay.h"
#include "NetworkItem.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.Collections.h>

class NetworkView {
public:
    explicit NetworkView(PeerDisplay& peerDisplay);

    winrt::Windows::UI::Xaml::Controls::StackPanel View() const;
    void Poll();

private:
    struct Entry {
        PeerDisplayItem item;
        std::unique_ptr<NetworkItemView> view;
    };

    static bool ItemLess(const PeerDisplayItem& left, const PeerDisplayItem& right);
    static bool SameHostID(const PeerDisplayItem& left, const PeerDisplayItem& right);
    std::size_t FindEntryByHostID(const PeerDisplayItem& item) const;
    void SetEmptyMessageVisible(bool visible);
    void UpdateEntry(Entry& entry, const PeerDisplayItem& item, std::chrono::steady_clock::time_point now);

    PeerDisplay& peerDisplay_;
    winrt::Windows::UI::Xaml::Controls::StackPanel container_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock emptyMessage_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::StackPanel itemsPanel_{ nullptr };
    std::vector<Entry> entries_;
};
