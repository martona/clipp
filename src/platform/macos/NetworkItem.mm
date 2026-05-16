#include "NetworkItem.h"

#ifdef __APPLE__

#include "platform/uiClippPage.h"
#include "UiHelpers.h"

#include <chrono>
#include <cstdint>
#include <string>

#import <AppKit/AppKit.h>

@interface MacOSNetworkItemTarget : NSObject {
@private
    MacOSNetworkItemView* owner_;
}
- (instancetype)initWithOwner:(MacOSNetworkItemView*)owner;
- (void)toggleDisclosure:(id)sender;
@end

namespace {
NSString* DisplayHostName(const std::wstring& hostName) {
    return MacOSToNSString(uiClippPage::DisplayHostNameOrUnknown(hostName));
}

NSString* FormatByteCounter(uint64_t bytes) {
    return MacOSToNSString(uiClippPage::FormatByteCounter(bytes));
}

NSString* FormatConnectionState(bool connected) {
    return MacOSToNSString(uiClippPage::FormatConnectionState(connected));
}
}

MacOSNetworkItemView::MacOSNetworkItemView(const PeerDisplayItem& item)
    : disclosureTarget_([[MacOSNetworkItemTarget alloc] initWithOwner:this]) {
    BuildView();
    UpdateHostName(item.hostName);
    UpdateIncomingConnection(item.hasIncomingConnection);
    UpdateOutgoingConnection(item.hasOutgoingConnection);
    UpdateBytesSent(item.bytesSent);
    UpdateBytesReceived(item.bytesReceived);
    UpdateConnectedSince(item.connectedSince);
}

NSView* MacOSNetworkItemView::View() const {
    return card_;
}

NSButton* MacOSNetworkItemView::DisclosureButton() const {
    return disclosureButton_;
}

void MacOSNetworkItemView::ToggleDisclosure() {
    detailsPanel_.hidden = !detailsPanel_.hidden;
    UpdateDisclosureImage();
    [card_ layoutSubtreeIfNeeded];
}

void MacOSNetworkItemView::UpdateHostName(const std::wstring& hostName) {
    MacOSSetFieldText(title_, DisplayHostName(hostName));
}

void MacOSNetworkItemView::UpdateHostID(const HostId&) {
}

void MacOSNetworkItemView::UpdateIncomingConnection(bool connected) {
    incomingIcon_.hidden = !connected;
    MacOSSetFieldText(incomingValue_, FormatConnectionState(connected));
}

void MacOSNetworkItemView::UpdateOutgoingConnection(bool connected) {
    outgoingIcon_.hidden = !connected;
    MacOSSetFieldText(outgoingValue_, FormatConnectionState(connected));
}

void MacOSNetworkItemView::UpdateBytesSent(uint64_t bytesSent) {
    MacOSSetFieldText(bytesSentValue_, FormatByteCounter(bytesSent));
}

void MacOSNetworkItemView::UpdateBytesReceived(uint64_t bytesReceived) {
    MacOSSetFieldText(bytesReceivedValue_, FormatByteCounter(bytesReceived));
}

void MacOSNetworkItemView::UpdateConnectedSince(std::chrono::steady_clock::time_point connectedSince) {
    connectedSince_ = connectedSince;
    connectedForText_.clear();
    RefreshConnectedFor();
}

void MacOSNetworkItemView::RefreshConnectedFor(std::chrono::steady_clock::time_point now) {
    const std::string text = uiClippPage::FormatConnectedFor(connectedSince_, now);
    if (connectedForText_ != text) {
        connectedForText_ = text;
        MacOSSetFieldText(subtitle_, MacOSToNSString(text));
    }
}

NSTextField* MacOSNetworkItemView::AddDetailRow(NSGridView* grid, NSInteger rowIndex, NSString* labelText) {
    NSTextField* label = MacOSMakeLabel(labelText);
    label.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];

    NSTextField* value = MacOSMakeLabel(@"");
    value.textColor = [NSColor secondaryLabelColor];

    [grid addRowWithViews:@[label, value]];
    NSGridRow* row = [grid rowAtIndex:rowIndex];
    row.yPlacement = NSGridCellPlacementCenter;
    return value;
}

void MacOSNetworkItemView::BuildView() {
    card_ = MacOSMakeGroupBox();

    NSStackView* cardStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    cardStack.translatesAutoresizingMaskIntoConstraints = NO;
    cardStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    cardStack.alignment = NSLayoutAttributeLeading;
    cardStack.distribution = NSStackViewDistributionFill;
    cardStack.spacing = 0.0;
    cardStack.detachesHiddenViews = YES;

    NSView* headerRow = [[NSView alloc] initWithFrame:NSZeroRect];
    headerRow.translatesAutoresizingMaskIntoConstraints = NO;

    NSImageView* networkIcon = MacOSMakeSymbolImageView(@"network", @"Network", [NSColor secondaryLabelColor]);
    NSStackView* titleStack = CreateTitleStack();
    NSStackView* statusStack = CreateStatusStack();
    NSButton* disclosureButton = CreateDisclosureButton();

    [titleStack setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [titleStack setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [statusStack setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [statusStack setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    [headerRow addSubview:networkIcon];
    [headerRow addSubview:titleStack];
    [headerRow addSubview:statusStack];
    [headerRow addSubview:disclosureButton];

    detailsPanel_ = [[NSView alloc] initWithFrame:NSZeroRect];
    detailsPanel_.translatesAutoresizingMaskIntoConstraints = NO;
    detailsPanel_.hidden = YES;

    NSGridView* detailsGrid = [[NSGridView alloc] initWithFrame:NSZeroRect];
    detailsGrid.translatesAutoresizingMaskIntoConstraints = NO;
    detailsGrid.columnSpacing = 24.0;
    detailsGrid.rowSpacing = 7.0;

    bytesSentValue_ = AddDetailRow(detailsGrid, 0, @"Bytes sent:");
    bytesReceivedValue_ = AddDetailRow(detailsGrid, 1, @"Bytes received:");
    incomingValue_ = AddDetailRow(detailsGrid, 2, @"Incoming:");
    outgoingValue_ = AddDetailRow(detailsGrid, 3, @"Outgoing:");
    [detailsGrid columnAtIndex:0].xPlacement = NSGridCellPlacementLeading;
    [detailsGrid columnAtIndex:1].xPlacement = NSGridCellPlacementLeading;

    [detailsPanel_ addSubview:detailsGrid];
    [cardStack addArrangedSubview:headerRow];
    [cardStack addArrangedSubview:detailsPanel_];
    [card_ addSubview:cardStack];

    [NSLayoutConstraint activateConstraints:@[
        [cardStack.leadingAnchor constraintEqualToAnchor:card_.leadingAnchor],
        [cardStack.trailingAnchor constraintEqualToAnchor:card_.trailingAnchor],
        [cardStack.topAnchor constraintEqualToAnchor:card_.topAnchor],
        [cardStack.bottomAnchor constraintEqualToAnchor:card_.bottomAnchor],

        [headerRow.widthAnchor constraintEqualToAnchor:cardStack.widthAnchor],
        [headerRow.heightAnchor constraintGreaterThanOrEqualToConstant:64.0],

        [networkIcon.leadingAnchor constraintEqualToAnchor:headerRow.leadingAnchor constant:16.0],
        [networkIcon.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],

        [titleStack.leadingAnchor constraintEqualToAnchor:networkIcon.trailingAnchor constant:12.0],
        [titleStack.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],
        [titleStack.topAnchor constraintGreaterThanOrEqualToAnchor:headerRow.topAnchor constant:12.0],
        [titleStack.bottomAnchor constraintLessThanOrEqualToAnchor:headerRow.bottomAnchor constant:-12.0],

        [statusStack.leadingAnchor constraintGreaterThanOrEqualToAnchor:titleStack.trailingAnchor constant:12.0],
        [statusStack.trailingAnchor constraintEqualToAnchor:disclosureButton.leadingAnchor constant:-10.0],
        [statusStack.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],

        [disclosureButton.trailingAnchor constraintEqualToAnchor:headerRow.trailingAnchor constant:-12.0],
        [disclosureButton.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],

        [detailsPanel_.widthAnchor constraintEqualToAnchor:cardStack.widthAnchor],

        [detailsGrid.leadingAnchor constraintEqualToAnchor:detailsPanel_.leadingAnchor constant:56.0],
        [detailsGrid.trailingAnchor constraintLessThanOrEqualToAnchor:detailsPanel_.trailingAnchor constant:-16.0],
        [detailsGrid.topAnchor constraintEqualToAnchor:detailsPanel_.topAnchor constant:6.0],
        [detailsGrid.bottomAnchor constraintEqualToAnchor:detailsPanel_.bottomAnchor constant:-16.0],
    ]];

    UpdateDisclosureImage();
}

NSStackView* MacOSNetworkItemView::CreateTitleStack() {
    NSStackView* textStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    textStack.translatesAutoresizingMaskIntoConstraints = NO;
    textStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    textStack.alignment = NSLayoutAttributeLeading;
    textStack.distribution = NSStackViewDistributionFill;
    textStack.spacing = 2.0;

    title_ = MacOSMakeLabel(@"");
    title_.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];

    subtitle_ = MacOSMakeLabel(@"");
    subtitle_.font = [NSFont systemFontOfSize:12];
    subtitle_.textColor = [NSColor secondaryLabelColor];

    [textStack addArrangedSubview:title_];
    [textStack addArrangedSubview:subtitle_];
    return textStack;
}

NSStackView* MacOSNetworkItemView::CreateStatusStack() {
    NSStackView* statusStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    statusStack.translatesAutoresizingMaskIntoConstraints = NO;
    statusStack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    statusStack.alignment = NSLayoutAttributeCenterY;
    statusStack.distribution = NSStackViewDistributionFill;
    statusStack.spacing = 4.0;

    incomingIcon_ = MacOSMakeSymbolImageView(@"arrow.down.circle.fill", @"Incoming connection", [NSColor systemGreenColor]);
    outgoingIcon_ = MacOSMakeSymbolImageView(@"arrow.up.circle.fill", @"Outgoing connection", [NSColor systemBlueColor]);

    [statusStack addArrangedSubview:incomingIcon_];
    [statusStack addArrangedSubview:outgoingIcon_];
    return statusStack;
}

NSButton* MacOSNetworkItemView::CreateDisclosureButton() {
    disclosureButton_ = MacOSMakeIconButton(@"chevron.right", @"Peer details", disclosureTarget_, @selector(toggleDisclosure:));
    return disclosureButton_;
}

void MacOSNetworkItemView::UpdateDisclosureImage() {
    NSString* symbol = detailsPanel_.hidden ? @"chevron.right" : @"chevron.down";
    disclosureButton_.image = MacOSMakeSymbolImage(symbol, @"Peer details", 13.0, [NSColor secondaryLabelColor]);
}

@implementation MacOSNetworkItemTarget

- (instancetype)initWithOwner:(MacOSNetworkItemView*)owner {
    self = [super init];
    if (self) {
        owner_ = owner;
    }
    return self;
}

- (void)toggleDisclosure:(id)sender {
    (void)sender;
    if (owner_ != nullptr) {
        owner_->ToggleDisclosure();
    }
}

@end

#endif
