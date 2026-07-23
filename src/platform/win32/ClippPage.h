#pragma once

#include "ClipboardActivityStore.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

class ClippPage {
public:
    using NavigateCallback = std::function<void()>;

    ClippPage(ClipboardActivityStore& activityStore, NavigateCallback showNetworkPage);
    ~ClippPage();

    ClippPage(const ClippPage&) = delete;
    ClippPage& operator=(const ClippPage&) = delete;

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

    void OnShown();
    void OnHidden();
    void OnDestroy();
    void OnNetworkKeyChanged();

private:
    void BuildView();
    winrt::Windows::UI::Xaml::Controls::Grid BuildActivitySection();
    winrt::Windows::UI::Xaml::Controls::Grid BuildActivityRow(uint64_t itemID);

    // FLIP-style list animation: capture panel-relative row positions before a
    // mutation, then slide survivors from old to new after it.
    struct RowPosition {
        uint64_t itemID;
        double y;
    };
    std::vector<RowPosition> CaptureRowPositions();
    void AnimateRowsFromPositions(const std::vector<RowPosition>& oldPositions);
    void RefreshActivityItems(const std::vector<ClipboardActivityItemHeader>& items);
    void AddActivityItem(uint64_t itemID);
    void RemoveActivityItem(uint64_t itemID);
    void MoveActivityItem(uint64_t itemID);
    void ClearActivityItems();
    void SetActivityEmptyMessageVisible(bool visible);
    void UpdateActivityEmptyState();
    bool IsActivityNearTop() const;
    void ScrollActivityToTop() const;
    void CopyActivityItem(uint64_t itemID);
    void BeginActivityNotifications();
    void EndActivityNotifications();

    static void ClipboardActivityWatcher(const ClipboardActivityUpdate& update, void* userData);

    ClipboardActivityStore& activityStore_;
    NavigateCallback showNetworkPage_;

    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::ScrollViewer activityScroll_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::StackPanel activityItemsPanel_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::StackPanel activityEmptyState_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock activityEmptyMessage_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::Button activityEmptyNetworkButton_{ nullptr };
    winrt::Windows::System::DispatcherQueue uiDispatcher_{ nullptr };

    std::size_t activityWatcherID_ = 0;
    std::vector<uint64_t> activityItemIDs_;
};
