#pragma once

#ifdef __APPLE__

#include "ClipboardActivityStore.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

@class NSMutableArray;
@class NSButton;
@class NSScrollView;
@class NSStackView;
@class NSTextField;
@class NSView;

class MacOSClippPageState;
@class MacOSClippPageTarget;

class MacOSClippPage {
public:
    using NavigateCallback = std::function<void()>;

    MacOSClippPage(ClipboardActivityStore& activityStore, NavigateCallback showNetworkPage = {});
    ~MacOSClippPage();

    MacOSClippPage(const MacOSClippPage&) = delete;
    MacOSClippPage& operator=(const MacOSClippPage&) = delete;

    NSView* View() const;

    void OnShown();
    void OnHidden();
    void OnDestroy();
    void OnNetworkKeyChanged();
    NSView* FirstKeyView() const;
    void ConnectKeyViewLoop(NSView* nextKeyView);

    void CopyActivityItem(uint64_t itemID);
    void DeleteActivityItem(uint64_t itemID);
    void ShowNetworkPage();

private:
    void BuildView();
    NSView* BuildActivityRow(uint64_t itemID);
    void RefreshActivityItems(const std::vector<ClipboardActivityItemHeader>& items);
    void AddActivityItem(uint64_t itemID);
    void RemoveActivityItem(uint64_t itemID);
    void MoveActivityItem(uint64_t itemID);
    void ClearActivityItems();
    void SetActivityEmptyMessageVisible(bool visible);
    void UpdateActivityEmptyState();
    bool IsActivityNearTop() const;
    void ScrollActivityToTop() const;
    void BeginActivityNotifications();
    void EndActivityNotifications();

    static void ClipboardActivityWatcher(const ClipboardActivityUpdate& update, void* userData);

    ClipboardActivityStore& activityStore_;
    NavigateCallback showNetworkPage_;

    NSView* root_ = nullptr;
    NSScrollView* activityScroll_ = nullptr;
    NSStackView* activityItemsPanel_ = nullptr;
    NSStackView* activityEmptyState_ = nullptr;
    NSTextField* activityEmptyMessage_ = nullptr;
    NSButton* activityEmptyNetworkButton_ = nullptr;
    NSView* nextKeyViewAfterPage_ = nullptr;
    MacOSClippPageTarget* actionTarget_ = nullptr;
    NSMutableArray* activityItemTargets_ = nil;

    std::shared_ptr<MacOSClippPageState> pageState_;
    std::size_t activityWatcherID_ = 0;
    std::vector<uint64_t> activityItemIDs_;
    bool destroyed_ = false;
};

#endif
