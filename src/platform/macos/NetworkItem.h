#pragma once

#ifdef __APPLE__

#include "PeerDisplay.h"

#include <chrono>
#include <cstdint>
#include <string>

#import <AppKit/AppKit.h>

@class MacOSNetworkItemTarget;

class MacOSNetworkItemView {
public:
    explicit MacOSNetworkItemView(const PeerDisplayItem& item);

    NSView* View() const;
    NSButton* DisclosureButton() const;

    void ToggleDisclosure();
    void UpdateHostName(const std::wstring& hostName);
    void UpdateHostID(const HostId& hostID);
    void UpdateIncomingConnection(bool connected);
    void UpdateOutgoingConnection(bool connected);
    void UpdateBytesSent(uint64_t bytesSent);
    void UpdateBytesReceived(uint64_t bytesReceived);
    void UpdateConnectedSince(std::chrono::steady_clock::time_point connectedSince);
    void RefreshConnectedFor(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

private:
    NSTextField* AddDetailRow(NSGridView* grid, NSInteger rowIndex, NSString* labelText);
    void BuildView();
    NSStackView* CreateTitleStack();
    NSStackView* CreateStatusStack();
    NSButton* CreateDisclosureButton();
    void UpdateDisclosureImage();

    NSView* card_ = nullptr;
    NSTextField* title_ = nullptr;
    NSTextField* subtitle_ = nullptr;
    NSImageView* incomingIcon_ = nullptr;
    NSImageView* outgoingIcon_ = nullptr;
    NSButton* disclosureButton_ = nullptr;
    NSView* detailsPanel_ = nullptr;
    NSTextField* bytesSentValue_ = nullptr;
    NSTextField* bytesReceivedValue_ = nullptr;
    NSTextField* incomingValue_ = nullptr;
    NSTextField* outgoingValue_ = nullptr;
    MacOSNetworkItemTarget* disclosureTarget_ = nullptr;
    std::chrono::steady_clock::time_point connectedSince_{};
    std::string connectedForText_;
};

#endif
