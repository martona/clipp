#pragma once

#ifdef __APPLE__

#include "KeyManager.h"
#include "PeerDisplay.h"

#include <cstddef>
#include <functional>
#include <memory>

@class MacOSClippPageFieldDelegate;
@class NSView;
@class NSSecureTextField;
@class NSTextField;
@class NSTimer;
@class NSScrollView;

class MacOSClippPageState;
class MacOSKeyDerivationWorker;
class MacOSNetworkView;

class MacOSClippPage {
public:
    explicit MacOSClippPage(std::function<void()> keyViewChangedHandler = {});
    ~MacOSClippPage();

    MacOSClippPage(const MacOSClippPage&) = delete;
    MacOSClippPage& operator=(const MacOSClippPage&) = delete;

    NSView* View() const;

    void OnShown();
    void OnHidden();
    void OnDestroy();
    NSView* FirstKeyView() const;
    void ConnectKeyViewLoop(NSView* nextKeyView);

    void OnFieldEditingBegan(NSTextField* field);
    void OnFieldEditingChanged(NSTextField* field);
    void OnFieldEditingEnded(NSTextField* field);
    void SchedulePeerDisplayUpdate();

private:
    void BuildView();
    void SetupPasswordFields();
    void NewPasswordHashReceived();
    void ApplyNetworkNameChange();
    void StartPasswordDebounceTimer();
    void StopPasswordDebounceTimer();
    void DerivePasswordFromCurrentField();
    void OnDerivedKey(const KeyManager::NetworkKey& key);
    void PollNetworkView();
    void ScrollToTop();
    void StartNetworkPollTimer();
    void StopNetworkPollTimer();
    void BeginPeerNotifications();
    void EndPeerNotifications();
    void NotifyKeyViewChanged();

    static void PeerDisplayWatcher(const PeerDisplayUpdate& update, void* userData);

    NSView* root_ = nullptr;
    NSTextField* networkNameField_ = nullptr;
    NSSecureTextField* passwordField_ = nullptr;
    NSView* passwordStatusPanel_ = nullptr;
    NSTextField* passwordHashText_ = nullptr;
    NSView* passwordInfoPanel_ = nullptr;
    NSTextField* passwordInfoText_ = nullptr;
    MacOSClippPageFieldDelegate* fieldDelegate_ = nullptr;
    NSTimer* networkPollTimer_ = nullptr;
    NSView* nextKeyViewAfterPage_ = nullptr;

    std::function<void()> keyViewChangedHandler_;
    std::shared_ptr<MacOSClippPageState> pageState_;
    std::unique_ptr<MacOSKeyDerivationWorker> keyDerivationWorker_;
    std::unique_ptr<MacOSNetworkView> networkView_;
    std::size_t peerDisplayWatcherID_ = 0;
    uint64_t passwordDebounceGeneration_ = 0;
    bool destroyed_ = false;
    bool suppressPasswordChange_ = false;
    NSScrollView* scrollView_ = nullptr;
};

#endif
