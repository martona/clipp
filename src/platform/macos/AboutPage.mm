#include "AboutPage.h"

#ifdef __APPLE__

#include "UiHelpers.h"
#include "platform/uistrings.h"

#import <AppKit/AppKit.h>

@interface MacOSAboutFlippedView : NSView
@end

@implementation MacOSAboutFlippedView

- (BOOL)isFlipped {
    return YES;
}

@end

namespace {
constexpr CGFloat kPageInset = 28.0;

NSTextField* MakeAboutText(NSString* text, CGFloat fontSize, NSColor* color, NSFontWeight weight = NSFontWeightRegular) {
    NSTextField* label = MacOSMakeWrappingLabel(text, fontSize, color);
    label.font = [NSFont systemFontOfSize:fontSize weight:weight];
    return label;
}

NSTextField* MakeSectionHeading(NSString* text) {
    NSTextField* label = [NSTextField labelWithString:text];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.font = [NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    label.textColor = [NSColor labelColor];
    return label;
}

NSTextField* MakeRepositoryLink() {
    NSString* title = CLP_NS(CLP_UI_REPOSITORY_LABEL);
    NSURL* url = [NSURL URLWithString:CLP_NS(CLP_UI_REPOSITORY_URL)];
    NSMutableAttributedString* linkText = [[NSMutableAttributedString alloc] initWithString:title attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName: [NSColor linkColor],
        NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle),
        NSLinkAttributeName: url,
    }];

    NSTextField* label = [NSTextField labelWithAttributedString:linkText];
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.allowsEditingTextAttributes = YES;
    label.selectable = YES;
    return label;
}

NSImage* LoadAboutArtwork() {
    NSBundle* bundle = [NSBundle mainBundle];
    NSString* path = [bundle pathForResource:@"Clipboard4DevAspect-macos-1024-transparent" ofType:@"png"];
    if (path.length > 0) {
        NSImage* image = [[NSImage alloc] initWithContentsOfFile:path];
        if (image != nil) {
            return image;
        }
    }

    return [NSImage imageNamed:@"Clipp"];
}

NSImageView* MakeArtworkView() {
    NSImageView* imageView = [[NSImageView alloc] initWithFrame:NSZeroRect];
    imageView.translatesAutoresizingMaskIntoConstraints = NO;
    imageView.image = LoadAboutArtwork();
    imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
    imageView.imageAlignment = NSImageAlignTop;
    return imageView;
}

void AddWrappedArrangedSubview(NSStackView* stack, NSTextField* label) {
    [stack addArrangedSubview:label];
    [label.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}
}

MacOSAboutPage::MacOSAboutPage() {
    BuildView();
}

NSView* MacOSAboutPage::View() const {
    return root_;
}

void MacOSAboutPage::BuildView() {
    root_ = [[NSView alloc] initWithFrame:NSZeroRect];
    root_.translatesAutoresizingMaskIntoConstraints = NO;

    NSScrollView* scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.hasVerticalScroller = YES;
    scrollView.hasHorizontalScroller = NO;
    scrollView.autohidesScrollers = YES;
    scrollView.borderType = NSNoBorder;
    scrollView.drawsBackground = NO;

    NSView* documentView = [[MacOSAboutFlippedView alloc] initWithFrame:NSZeroRect];
    documentView.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView.documentView = documentView;

    NSStackView* contentStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    contentStack.translatesAutoresizingMaskIntoConstraints = NO;
    contentStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    contentStack.alignment = NSLayoutAttributeLeading;
    contentStack.distribution = NSStackViewDistributionFill;
    contentStack.spacing = 14.0;

    NSTextField* heading = [NSTextField labelWithString:CLP_NS(CLP_UI_ABOUT_TITLE)];
    heading.translatesAutoresizingMaskIntoConstraints = NO;
    heading.font = [NSFont systemFontOfSize:28 weight:NSFontWeightSemibold];
    heading.textColor = [NSColor labelColor];

    NSTextField* intro = MakeAboutText(CLP_NS(CLP_UI_TAGLINE),
                                       14.0,
                                       [NSColor secondaryLabelColor]);

    NSView* projectBand = [[NSView alloc] initWithFrame:NSZeroRect];
    projectBand.translatesAutoresizingMaskIntoConstraints = NO;

    NSImageView* artworkView = MakeArtworkView();

    NSStackView* projectStack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    projectStack.translatesAutoresizingMaskIntoConstraints = NO;
    projectStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    projectStack.alignment = NSLayoutAttributeLeading;
    projectStack.distribution = NSStackViewDistributionFill;
    projectStack.spacing = 6.0;

    NSTextField* projectHeading = MakeSectionHeading(CLP_NS(CLP_UI_PROJECT));
    AddWrappedArrangedSubview(projectStack, projectHeading);
    [projectStack setCustomSpacing:10.0 afterView:projectHeading];
    AddWrappedArrangedSubview(projectStack, MakeAboutText(CLP_NS(CLP_UI_COPYRIGHT), 13.0, [NSColor secondaryLabelColor]));
    AddWrappedArrangedSubview(projectStack, MakeAboutText(CLP_NS(CLP_UI_MIT_LICENSE), 13.0, [NSColor secondaryLabelColor]));
    [projectStack addArrangedSubview:MakeRepositoryLink()];

    [projectBand addSubview:artworkView];
    [projectBand addSubview:projectStack];

    NSStackView* acknowledgements = [[NSStackView alloc] initWithFrame:NSZeroRect];
    acknowledgements.translatesAutoresizingMaskIntoConstraints = NO;
    acknowledgements.orientation = NSUserInterfaceLayoutOrientationVertical;
    acknowledgements.alignment = NSLayoutAttributeLeading;
    acknowledgements.distribution = NSStackViewDistributionFill;
    acknowledgements.spacing = 6.0;

    AddWrappedArrangedSubview(acknowledgements, MakeSectionHeading(CLP_NS(CLP_UI_OPEN_SOURCE_ACKNOWLEDGEMENTS)));
    AddWrappedArrangedSubview(acknowledgements, MakeAboutText(CLP_NS(CLP_UI_ACK_LIBSODIUM), 13.0, [NSColor secondaryLabelColor]));
    AddWrappedArrangedSubview(acknowledgements, MakeAboutText(CLP_NS(CLP_UI_ACK_LODEPNG), 13.0, [NSColor secondaryLabelColor]));
    AddWrappedArrangedSubview(acknowledgements, MakeAboutText(CLP_NS(CLP_UI_ACK_XXHASH), 13.0, [NSColor secondaryLabelColor]));
    AddWrappedArrangedSubview(acknowledgements, MakeAboutText(CLP_NS(CLP_UI_ACK_ZSTD), 13.0, [NSColor secondaryLabelColor]));

    NSTextField* note = MakeAboutText(CLP_NS(CLP_UI_THIRD_PARTY_LICENSE_NOTE),
                                      12.0,
                                      [NSColor tertiaryLabelColor]);

    [contentStack addArrangedSubview:heading];
    [contentStack addArrangedSubview:intro];
    [contentStack addArrangedSubview:projectBand];
    [contentStack addArrangedSubview:acknowledgements];
    [contentStack addArrangedSubview:note];

    [documentView addSubview:contentStack];
    [root_ addSubview:scrollView];

    [NSLayoutConstraint activateConstraints:@[
        [scrollView.leadingAnchor constraintEqualToAnchor:root_.leadingAnchor],
        [scrollView.trailingAnchor constraintEqualToAnchor:root_.trailingAnchor],
        [scrollView.topAnchor constraintEqualToAnchor:root_.topAnchor],
        [scrollView.bottomAnchor constraintEqualToAnchor:root_.bottomAnchor],

        [documentView.widthAnchor constraintEqualToAnchor:scrollView.contentView.widthAnchor],

        [contentStack.leadingAnchor constraintEqualToAnchor:documentView.leadingAnchor constant:kPageInset],
        [contentStack.trailingAnchor constraintEqualToAnchor:documentView.trailingAnchor constant:-kPageInset],
        [contentStack.topAnchor constraintEqualToAnchor:documentView.topAnchor constant:kPageInset],
        [contentStack.bottomAnchor constraintEqualToAnchor:documentView.bottomAnchor constant:-kPageInset],

        [heading.widthAnchor constraintLessThanOrEqualToAnchor:contentStack.widthAnchor],
        [intro.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [projectBand.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [acknowledgements.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],
        [note.widthAnchor constraintEqualToAnchor:contentStack.widthAnchor],

        [artworkView.leadingAnchor constraintEqualToAnchor:projectBand.leadingAnchor],
        [artworkView.topAnchor constraintEqualToAnchor:projectBand.topAnchor],
        [artworkView.widthAnchor constraintEqualToConstant:112.0],
        [artworkView.heightAnchor constraintEqualToConstant:112.0],
        [artworkView.bottomAnchor constraintLessThanOrEqualToAnchor:projectBand.bottomAnchor],

        [projectStack.leadingAnchor constraintEqualToAnchor:artworkView.trailingAnchor constant:24.0],
        [projectStack.trailingAnchor constraintEqualToAnchor:projectBand.trailingAnchor],
        [projectStack.topAnchor constraintEqualToAnchor:projectBand.topAnchor],
        [projectStack.bottomAnchor constraintLessThanOrEqualToAnchor:projectBand.bottomAnchor],
        [projectBand.heightAnchor constraintGreaterThanOrEqualToConstant:112.0],
    ]];
}

#endif
