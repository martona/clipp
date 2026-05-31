#include "platform.h"

#ifdef __APPLE__

#import "CliPathBanner.h"

#include "UiHelpers.h"
#include "platform/uistrings.h"

namespace {

// NSUserDefaults keys. macOS-only UI state, so they live in defaults rather than the
// cross-platform Settings store.
//   ...Dismissed:      the user permanently dismissed the banner.
//   ...EverConnected:  the sticky "user has connected to a peer at least once" latch
//                      that proves they're past initial setup.
NSString* const kCliBannerDismissedDefaultsKey = @"CliPathBannerDismissed";
NSString* const kCliBannerEverConnectedDefaultsKey = @"CliPathBannerEverConnected";

// Common PATH locations we'd symlink into. If `clipp` already resolves at one of
// these, the user has done the deed (or Homebrew-style tooling has) and the banner
// is moot. /usr/local/bin is the Intel/most-common target; /opt/homebrew/bin is the
// Apple-silicon Homebrew prefix. We check for our symlink specifically rather than
// scanning the whole PATH -- this is what the banner offers to create.
NSArray<NSString*>* CliSymlinkTargets() {
    return @[ @"/usr/local/bin/clipp", @"/opt/homebrew/bin/clipp" ];
}

// Absolute path to the clipp executable inside this .app bundle. There is exactly
// one binary -- Clipp.app/Contents/MacOS/clipp -- and it doubles as the CLI.
NSString* BundledClippBinaryPath() {
    // executablePath is the running binary itself; that's precisely what we want to
    // link to, and it's correct regardless of where the .app was installed.
    NSString* path = [[NSBundle mainBundle] executablePath];
    return path != nil ? path : @"/Applications/Clipp.app/Contents/MacOS/clipp";
}

// Returns YES if any known symlink target already exists (symlink, file, or dir entry
// -- existence is enough to consider the CLI "installed" and stop nagging).
BOOL CliAlreadyInstalled() {
    NSFileManager* fileManager = [NSFileManager defaultManager];
    for (NSString* target in CliSymlinkTargets()) {
        if ([fileManager fileExistsAtPath:target]) {
            return YES;
        }
    }
    return NO;
}

// Builds the Terminal command the user runs to install the CLI. We assume the target
// directory needs sudo (it usually does for /usr/local/bin, and a needless sudo on a
// dir the user could write is harmless). Two forms: if /usr/local/bin already exists
// we skip the mkdir; otherwise we create it first (fresh Apple-silicon machines have
// no /usr/local/bin). Single-quote the bundle path so spaces in it survive the shell.
NSString* CliInstallCommand() {
    NSString* binaryPath = BundledClippBinaryPath();
    // Escape any single quotes in the path for safe single-quoting: ' -> '\''.
    NSString* escaped = [binaryPath stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"];
    NSString* quoted = [NSString stringWithFormat:@"'%@'", escaped];

    const BOOL binDirExists = [[NSFileManager defaultManager] fileExistsAtPath:@"/usr/local/bin"];
    if (binDirExists) {
        return [NSString stringWithFormat:@"sudo ln -sf %@ /usr/local/bin/clipp", quoted];
    }
    return [NSString stringWithFormat:@"sudo mkdir -p /usr/local/bin && sudo ln -sf %@ /usr/local/bin/clipp", quoted];
}

NSColor* BannerBoxFillColor() {
    // The clipp navy accent (a deeper, more opaque cousin of the outgoing-clip bubble
    // color in ClippPage) so the banner reads as "Clipp talking about itself" and is
    // visually distinct from page content in both light and dark mode.
    return [NSColor colorWithCalibratedRed:0.0 green:0.32 blue:0.58 alpha:0.92];
}

NSColor* CommandStripFillColor() {
    // Near-black "terminal" strip for the monospaced command. Explicitly dark (not a
    // semantic system color) so it reads terminal-like in both appearances without
    // going pure #000 (which clashes against the dialog black in dark mode).
    return [NSColor colorWithCalibratedWhite:0.11 alpha:1.0];
}

}  // namespace

@implementation ClippCliPathBanner {
    void (^dismissHandler_)(void);
    NSButton* copyButton_;
    NSString* command_;
    NSUInteger copyFeedbackGeneration_;
}

+ (BOOL)shouldShow {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if ([defaults boolForKey:kCliBannerDismissedDefaultsKey]) {
        return NO;
    }
    if (![defaults boolForKey:kCliBannerEverConnectedDefaultsKey]) {
        return NO;  // not yet proven past initial setup -- wait for a real peer connection
    }
    if (CliAlreadyInstalled()) {
        return NO;
    }
    return YES;
}

+ (void)noteConnectedPeers {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if (![defaults boolForKey:kCliBannerEverConnectedDefaultsKey]) {
        [defaults setBool:YES forKey:kCliBannerEverConnectedDefaultsKey];
    }
}

+ (BOOL)isResolved {
    // Decided-for-good: dismissed (permanent) or already installed. The everConnected
    // latch is deliberately NOT consulted here -- not-yet-connected is exactly the
    // pending state the poll timer exists to watch.
    if ([[NSUserDefaults standardUserDefaults] boolForKey:kCliBannerDismissedDefaultsKey]) {
        return YES;
    }
    return CliAlreadyInstalled();
}

+ (void)markDismissed {
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:kCliBannerDismissedDefaultsKey];
}

- (instancetype)initWithDismissHandler:(void (^)(void))dismissHandler {
    self = [super initWithFrame:NSZeroRect];
    if (self) {
        dismissHandler_ = [dismissHandler copy];
        command_ = [CliInstallCommand() copy];
        [self buildView];
    }
    return self;
}

- (void)buildView {
    self.translatesAutoresizingMaskIntoConstraints = NO;

    NSBox* box = [[NSBox alloc] initWithFrame:NSZeroRect];
    box.translatesAutoresizingMaskIntoConstraints = NO;
    box.boxType = NSBoxCustom;
    box.titlePosition = NSNoTitle;
    box.borderType = NSNoBorder;
    box.cornerRadius = 10.0;
    box.fillColor = BannerBoxFillColor();
    [self addSubview:box];

    // Title + body, light text on the navy fill.
    NSTextField* title = [NSTextField labelWithString:CLP_NS(CLP_UI_CLI_BANNER_TITLE)];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    title.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold];
    title.textColor = [NSColor whiteColor];

    NSTextField* body = MacOSMakeWrappingLabel(CLP_NS(CLP_UI_CLI_BANNER_BODY), 12.0,
                                               [NSColor colorWithCalibratedWhite:1.0 alpha:0.85]);

    // Dismiss (x) in the top-right corner of the box.
    NSButton* dismiss = [NSButton buttonWithImage:MacOSMakeSymbolImage(@"xmark", CLP_NS(CLP_UI_CLI_BANNER_DISMISS), 11.0, [NSColor whiteColor])
                                           target:self
                                           action:@selector(dismissClicked:)];
    dismiss.translatesAutoresizingMaskIntoConstraints = NO;
    dismiss.bezelStyle = NSBezelStyleRegularSquare;
    dismiss.bordered = NO;
    dismiss.imagePosition = NSImageOnly;
    dismiss.contentTintColor = [NSColor colorWithCalibratedWhite:1.0 alpha:0.85];
    dismiss.toolTip = CLP_NS(CLP_UI_CLI_BANNER_DISMISS);

    // Command strip: dark "terminal" box holding the monospaced command + a copy glyph.
    NSBox* strip = [[NSBox alloc] initWithFrame:NSZeroRect];
    strip.translatesAutoresizingMaskIntoConstraints = NO;
    strip.boxType = NSBoxCustom;
    strip.titlePosition = NSNoTitle;
    strip.borderType = NSNoBorder;
    strip.cornerRadius = 6.0;
    strip.fillColor = CommandStripFillColor();

    NSTextField* commandLabel = [NSTextField labelWithString:command_];
    commandLabel.translatesAutoresizingMaskIntoConstraints = NO;
    commandLabel.font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular];
    commandLabel.textColor = [NSColor colorWithCalibratedWhite:0.92 alpha:1.0];
    commandLabel.selectable = YES;  // let the user select/copy manually too
    commandLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    commandLabel.allowsDefaultTighteningForTruncation = YES;
    [commandLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];

    copyButton_ = [NSButton buttonWithImage:MacOSMakeSymbolImage(@"doc.on.doc", CLP_NS(CLP_UI_CLI_BANNER_COPY), 13.0, [NSColor whiteColor])
                                     target:self
                                     action:@selector(copyClicked:)];
    copyButton_.translatesAutoresizingMaskIntoConstraints = NO;
    copyButton_.bezelStyle = NSBezelStyleRegularSquare;
    copyButton_.bordered = NO;
    copyButton_.imagePosition = NSImageOnly;
    copyButton_.contentTintColor = [NSColor colorWithCalibratedWhite:0.92 alpha:1.0];
    copyButton_.toolTip = CLP_NS(CLP_UI_CLI_BANNER_COPY);
    [copyButton_ setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    [strip addSubview:commandLabel];
    [strip addSubview:copyButton_];
    [box addSubview:title];
    [box addSubview:body];
    [box addSubview:dismiss];
    [box addSubview:strip];

    [NSLayoutConstraint activateConstraints:@[
        // Box fills self with a little vertical breathing room (horizontal insets are
        // applied by the shell when it positions the banner).
        [box.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [box.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [box.topAnchor constraintEqualToAnchor:self.topAnchor constant:8.0],
        [box.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-4.0],

        [title.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:14.0],
        [title.topAnchor constraintEqualToAnchor:box.topAnchor constant:12.0],
        [title.trailingAnchor constraintLessThanOrEqualToAnchor:dismiss.leadingAnchor constant:-8.0],

        [dismiss.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-10.0],
        [dismiss.topAnchor constraintEqualToAnchor:box.topAnchor constant:10.0],
        [dismiss.widthAnchor constraintEqualToConstant:20.0],
        [dismiss.heightAnchor constraintEqualToConstant:20.0],

        [body.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:14.0],
        [body.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-14.0],
        [body.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:4.0],

        [strip.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:14.0],
        [strip.trailingAnchor constraintEqualToAnchor:box.trailingAnchor constant:-14.0],
        [strip.topAnchor constraintEqualToAnchor:body.bottomAnchor constant:10.0],
        [strip.bottomAnchor constraintEqualToAnchor:box.bottomAnchor constant:-12.0],

        [commandLabel.leadingAnchor constraintEqualToAnchor:strip.leadingAnchor constant:10.0],
        [commandLabel.centerYAnchor constraintEqualToAnchor:strip.centerYAnchor],
        [commandLabel.topAnchor constraintEqualToAnchor:strip.topAnchor constant:8.0],
        [commandLabel.bottomAnchor constraintEqualToAnchor:strip.bottomAnchor constant:-8.0],

        [copyButton_.leadingAnchor constraintGreaterThanOrEqualToAnchor:commandLabel.trailingAnchor constant:8.0],
        [copyButton_.trailingAnchor constraintEqualToAnchor:strip.trailingAnchor constant:-8.0],
        [copyButton_.centerYAnchor constraintEqualToAnchor:strip.centerYAnchor],
        [copyButton_.widthAnchor constraintEqualToConstant:22.0],
        [copyButton_.heightAnchor constraintEqualToConstant:22.0],
    ]];
}

- (void)copyClicked:(id)sender {
    (void)sender;
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:command_ forType:NSPasteboardTypeString];
    [self showCopiedFeedback];
}

- (void)showCopiedFeedback {
    ++copyFeedbackGeneration_;
    const NSUInteger generation = copyFeedbackGeneration_;

    copyButton_.image = MacOSMakeSymbolImage(@"checkmark", CLP_NS(CLP_UI_CLI_BANNER_COPIED), 13.0, [NSColor whiteColor]);
    copyButton_.contentTintColor = [NSColor systemGreenColor];
    copyButton_.toolTip = CLP_NS(CLP_UI_CLI_BANNER_COPIED);

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.9 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (generation != self->copyFeedbackGeneration_) {
            return;
        }
        self->copyButton_.image = MacOSMakeSymbolImage(@"doc.on.doc", CLP_NS(CLP_UI_CLI_BANNER_COPY), 13.0, [NSColor whiteColor]);
        self->copyButton_.contentTintColor = [NSColor colorWithCalibratedWhite:0.92 alpha:1.0];
        self->copyButton_.toolTip = CLP_NS(CLP_UI_CLI_BANNER_COPY);
    });
}

- (void)dismissClicked:(id)sender {
    (void)sender;
    [ClippCliPathBanner markDismissed];
    if (dismissHandler_) {
        dismissHandler_();
    }
}

@end

#endif  // __APPLE__
