#include "NetworkItem.h"

#ifdef __APPLE__

#include "OsGlyphs.h"
#include "platform/uiClippPage.h"
#include "UiHelpers.h"

#include <chrono>
#include <cstdint>
#include <string>

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

@interface MacOSNetworkItemTarget : NSObject {
@private
    MacOSNetworkItemView* owner_;
}
- (instancetype)initWithOwner:(MacOSNetworkItemView*)owner;
- (void)toggleDisclosure:(id)sender;
@end

namespace {
constexpr CGFloat kBadgeViewPx = 27.0;     // family glyph box / badge view size
constexpr CGFloat kBadgeDevicePx = 13.0;   // device badge box
constexpr CGFloat kBadgeGlyphFill = 0.85;  // font size as a fraction of its box
                                           // (CoreText line height > point size, so
                                           // < 1.0 avoids vertical clipping; raise to
                                           // fill more)
constexpr CGFloat kBadgeHaloPct = 9.0;     // halo as % of device font (NSStrokeWidth)

void EnsureSymbolsFontRegistered() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURL* url = [[NSBundle mainBundle] URLForResource:@"ClippSymbols" withExtension:@"ttf"];
        if (url != nil) {
            CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url, kCTFontManagerScopeProcess, NULL);
        }
    });
}

NSString* GlyphString(char32_t codepoint) {
    return [[NSString alloc] initWithBytes:&codepoint
                                    length:sizeof(codepoint)
                                  encoding:NSUTF32LittleEndianStringEncoding];
}

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

// Compound device mark: the OS-family glyph (primary) with the device-type glyph
// overlaid as a smaller badge over its bottom-right quarter, with a background-color
// halo knocked out of the family glyph (mirrors the Windows treatment). Drawn in
// drawRect: so AppKit re-renders on light/dark switches automatically.
@interface ClippGlyphBadgeView : NSView
- (void)setOsType:(OsType)osType;
@end

@implementation ClippGlyphBadgeView {
    OsType _osType;
}

- (void)setOsType:(OsType)osType {
    _osType = osType;
    [self setNeedsDisplay:YES];
}

- (NSSize)intrinsicContentSize {
    return NSMakeSize(kBadgeViewPx, kBadgeViewPx);
}

- (void)drawGlyph:(char32_t)codepoint
             fill:(NSColor*)fill
           stroke:(NSColor*)stroke
        strokePct:(CGFloat)strokePct
           inRect:(NSRect)rect {
    if (codepoint == 0) {
        return;
    }
    NSString* str = GlyphString(codepoint);
    // Reference size only -- the glyph is scaled to fit `rect` below, so the absolute
    // point size doesn't matter (it just keeps the metrics precise).
    const CGFloat refSize = 128.0;
    CTFontRef font = CTFontCreateWithName(CFSTR("Clipp Symbols"), refSize, NULL);
    if (str == nil || font == NULL) {
        if (font != NULL) {
            CFRelease(font);
        }
        return;
    }

    // Shape into a single glyph (this is how the astral md-* codepoints survive the
    // surrogate pair) and capture the glyph id + the font CoreText actually used.
    NSDictionary* attrs = @{ (__bridge NSString*)kCTFontAttributeName: (__bridge id)font };
    NSAttributedString* attributed = [[NSAttributedString alloc] initWithString:str attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CGGlyph glyph = 0;
    CTFontRef glyphFont = NULL;
    if (CFArrayGetCount(runs) > 0) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, 0);
        if (CTRunGetGlyphCount(run) > 0) {
            CTRunGetGlyphs(run, CFRangeMake(0, 1), &glyph);
            CTFontRef runFont = (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
            if (runFont != NULL) {
                glyphFont = (CTFontRef)CFRetain(runFont);
            }
        }
    }
    CFRelease(line);
    if (glyphFont == NULL || glyph == 0) {
        CFRelease(font);
        return;
    }

    // Fit the glyph's TRUE ink bounds into rect (times the fill fraction) and center
    // it. Icon glyphs have wildly different advances/metrics, so measuring the actual
    // path bbox and scaling to fit is the only reliable way to size + place them --
    // and it can't overflow the box, so nothing clips at the view edge.
    const CGRect ink = CTFontGetBoundingRectsForGlyphs(glyphFont, kCTFontOrientationHorizontal, &glyph, NULL, 1);
    if (ink.size.width > 0.0 && ink.size.height > 0.0) {
        const CGFloat scale = kBadgeGlyphFill * MIN(rect.size.width / ink.size.width,
                                                    rect.size.height / ink.size.height);
        CGContextRef ctx = [NSGraphicsContext currentContext].CGContext;
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, NSMidX(rect), NSMidY(rect));
        CGContextScaleCTM(ctx, scale, scale);

        const CGPoint pos = CGPointMake(-(ink.origin.x + ink.size.width / 2.0),
                                        -(ink.origin.y + ink.size.height / 2.0));

        // Halo FIRST (stroke only, bg color), then the green fill ON TOP. Order
        // matters: a single fill+stroke pass strokes over the fill, so the bg halo
        // eats into the green (erasing thin glyphs entirely). Drawing the fill last
        // keeps the glyph fully visible and leaves the stroke as an outward halo only.
        // Width is doubled because the fill covers the stroke's inner half.
        if (stroke != nil && strokePct > 0.0) {
            CGContextSetTextDrawingMode(ctx, kCGTextStroke);
            CGContextSetStrokeColorWithColor(ctx, stroke.CGColor);
            CGContextSetLineWidth(ctx, refSize * strokePct / 100.0 * 2.0);
            CGContextSetLineJoin(ctx, kCGLineJoinRound);
            CTFontDrawGlyphs(glyphFont, &glyph, &pos, 1, ctx);
        }
        CGContextSetTextDrawingMode(ctx, kCGTextFill);
        CGContextSetFillColorWithColor(ctx, fill.CGColor);
        CTFontDrawGlyphs(glyphFont, &glyph, &pos, 1, ctx);
        CGContextRestoreGState(ctx);
    }

    CFRelease(glyphFont);
    CFRelease(font);
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    EnsureSymbolsFontRegistered();
    const clipp::OsGlyphs glyphs = clipp::OsGlyphsFor(_osType);
    const CGFloat boxW = self.bounds.size.width;

    // Family glyph: primary, blue, fills the box (macOS arrow palette: outgoing=blue).
    [self drawGlyph:glyphs.family
               fill:[NSColor systemBlueColor]
             stroke:nil
          strokePct:0.0
             inRect:self.bounds];

    // Device glyph: smaller green badge over the bottom-right quarter, with a
    // window-background halo so it knocks cleanly out of the family glyph.
    if (glyphs.device != 0) {
        const NSRect devRect = NSMakeRect(boxW - kBadgeDevicePx, 0.0, kBadgeDevicePx, kBadgeDevicePx);
        [self drawGlyph:glyphs.device
                   fill:[NSColor systemGreenColor]
                 stroke:[NSColor windowBackgroundColor]
              strokePct:kBadgeHaloPct
                 inRect:devRect];
    }
}

@end

MacOSNetworkItemView::MacOSNetworkItemView(const PeerDisplayItem& item)
    : disclosureTarget_([[MacOSNetworkItemTarget alloc] initWithOwner:this]) {
    BuildView();
    UpdateOsType(item.osType);
    UpdateHostName(item.hostName);
    UpdateIncomingConnection(item.hasIncomingConnection);
    UpdateOutgoingConnection(item.hasOutgoingConnection);
    UpdateOutgoingConnState(item.outgoingConnState);
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

void MacOSNetworkItemView::UpdateOsType(OsType osType) {
    osType_ = osType;
    [glyphBadge_ setOsType:osType];
}

void MacOSNetworkItemView::UpdateIncomingConnection(bool connected) {
    hasIncoming_ = connected;
    incomingIcon_.hidden = !connected;
    MacOSSetFieldText(incomingValue_, FormatConnectionState(connected));
    RefreshConnectedFor();
}

void MacOSNetworkItemView::UpdateOutgoingConnection(bool connected) {
    outgoingIcon_.hidden = !connected;
    MacOSSetFieldText(outgoingValue_, FormatConnectionState(connected));
}

void MacOSNetworkItemView::UpdateOutgoingConnState(PeerConnState state) {
    outgoingState_ = state;
    NSColor* tint = nil;
    switch (state) {
    case PeerConnState::Connecting:
        tint = [NSColor secondaryLabelColor];
        break;
    case PeerConnState::Connected:
        tint = [NSColor systemBlueColor];
        break;
    case PeerConnState::Backoff:
    case PeerConnState::Failed:
        tint = [NSColor systemRedColor];
        break;
    }
    outgoingIcon_.contentTintColor = tint;
    RefreshConnectedFor();
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
    std::string text;
    switch (outgoingState_) {
    case PeerConnState::Connecting:
        text = "Connecting\xE2\x80\xA6"; // ellipsis
        break;
    case PeerConnState::Backoff:
        text = "Reconnecting\xE2\x80\xA6";
        break;
    case PeerConnState::Failed:
        text = "Connection failed";
        break;
    case PeerConnState::Connected:
        text = uiClippPage::FormatConnectedFor(connectedSince_, now);
        if (!hasIncoming_) {
            text += "  \xC2\xB7  no inbound";
        }
        break;
    }
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

    glyphBadge_ = [[ClippGlyphBadgeView alloc] initWithFrame:NSZeroRect];
    glyphBadge_.translatesAutoresizingMaskIntoConstraints = NO;
    NSStackView* titleStack = CreateTitleStack();
    NSStackView* statusStack = CreateStatusStack();
    NSButton* disclosureButton = CreateDisclosureButton();

    [titleStack setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [titleStack setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [statusStack setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [statusStack setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    [headerRow addSubview:glyphBadge_];
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

    bytesSentValue_ = AddDetailRow(detailsGrid, 0, CLP_NS(CLP_UI_BYTES_SENT));
    bytesReceivedValue_ = AddDetailRow(detailsGrid, 1, CLP_NS(CLP_UI_BYTES_RECEIVED));
    incomingValue_ = AddDetailRow(detailsGrid, 2, CLP_NS(CLP_UI_INCOMING));
    outgoingValue_ = AddDetailRow(detailsGrid, 3, CLP_NS(CLP_UI_OUTGOING));
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

        [glyphBadge_.leadingAnchor constraintEqualToAnchor:headerRow.leadingAnchor constant:16.0],
        [glyphBadge_.centerYAnchor constraintEqualToAnchor:headerRow.centerYAnchor],

        [titleStack.leadingAnchor constraintEqualToAnchor:glyphBadge_.trailingAnchor constant:12.0],
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
