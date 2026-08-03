#include "TextTyper.h"

#include "Logger.h"
#include "platform/uistrings.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>  // kVK_* modifier key codes

#include <algorithm>
#include <string>
#include <vector>

// Progress pill under the menu bar, beside the status item. Borderless and
// non-activating: it must never take focus, or the app being typed into stops
// receiving the keystrokes.
@interface ClippTypingProgressPanel : NSPanel
@end

@implementation ClippTypingProgressPanel
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

namespace clipp {

namespace {

// Stamped on every event we post so the abort monitor can tell our own
// keystrokes from the user's. Without it the first synthesized key would look
// like user input and cancel the run instantly.
constexpr int64_t kClippTypingUserData = 0x434C5054;  // 'CLPT'

CGEventFlags FlagsForHeldModifiers(const std::vector<TypeKeyCode>& held) {
    CGEventFlags flags = 0;
    for (const TypeKeyCode key : held) {
        switch (key) {
        case kVK_Shift:
        case kVK_RightShift:
            flags |= kCGEventFlagMaskShift;
            break;
        case kVK_Option:
        case kVK_RightOption:
            flags |= kCGEventFlagMaskAlternate;
            break;
        case kVK_Control:
        case kVK_RightControl:
            flags |= kCGEventFlagMaskControl;
            break;
        case kVK_Command:
            flags |= kCGEventFlagMaskCommand;
            break;
        default:
            break;
        }
    }
    return flags;
}

bool IsModifierKey(TypeKeyCode key) {
    return FlagsForHeldModifiers({ key }) != 0;
}

class ProgressPill {
public:
    void Show(const std::wstring& text) {
        EnsureCreated();
        SetText(text);
        Reposition();
        [panel_ orderFront:nil];
    }

    void Update(const std::wstring& text) {
        if (panel_ == nil) {
            return;
        }
        // ~10 Hz: the counter moves once per keystroke, far faster than anyone
        // reads, and relayout on every tick is pure waste.
        const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
        if (now - lastUpdate_ < 0.09) {
            return;
        }
        lastUpdate_ = now;
        SetText(text);
        Reposition();
    }

    void Hide() {
        [panel_ orderOut:nil];
    }

    void Destroy() {
        [panel_ orderOut:nil];
        panel_ = nil;
        label_ = nil;
    }

private:
    void EnsureCreated() {
        if (panel_ != nil) {
            return;
        }
        panel_ = [[ClippTypingProgressPanel alloc]
            initWithContentRect:NSMakeRect(0, 0, 240, 30)
                      styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                        backing:NSBackingStoreBuffered
                          defer:YES];
        panel_.level = NSStatusWindowLevel;
        panel_.opaque = NO;
        panel_.backgroundColor = [NSColor clearColor];
        panel_.hasShadow = YES;
        panel_.ignoresMouseEvents = YES;  // a click near it belongs to the app underneath
        panel_.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces
            | NSWindowCollectionBehaviorFullScreenAuxiliary
            | NSWindowCollectionBehaviorIgnoresCycle;

        NSVisualEffectView* background = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
        background.material = NSVisualEffectMaterialHUDWindow;
        background.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        background.state = NSVisualEffectStateActive;
        background.wantsLayer = YES;
        background.layer.cornerRadius = 8.0;
        background.layer.masksToBounds = YES;
        background.translatesAutoresizingMaskIntoConstraints = NO;

        label_ = [NSTextField labelWithString:@""];
        label_.font = [NSFont systemFontOfSize:12.0];
        label_.textColor = [NSColor labelColor];
        label_.alignment = NSTextAlignmentCenter;
        label_.translatesAutoresizingMaskIntoConstraints = NO;
        [background addSubview:label_];

        NSView* root = [[NSView alloc] initWithFrame:NSZeroRect];
        [root addSubview:background];
        panel_.contentView = root;
        background.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [NSLayoutConstraint activateConstraints:@[
            [background.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
            [background.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
            [background.topAnchor constraintEqualToAnchor:root.topAnchor],
            [background.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
            [label_.leadingAnchor constraintEqualToAnchor:background.leadingAnchor constant:14.0],
            [label_.trailingAnchor constraintEqualToAnchor:background.trailingAnchor constant:-14.0],
            [label_.topAnchor constraintEqualToAnchor:background.topAnchor constant:7.0],
            [label_.bottomAnchor constraintEqualToAnchor:background.bottomAnchor constant:-7.0],
        ]];
    }

    void SetText(const std::wstring& text) {
        std::string utf8;
        utf8.reserve(text.size());
        for (const wchar_t ch : text) {
            const auto codepoint = static_cast<uint32_t>(ch);
            if (codepoint < 0x80) {
                utf8.push_back(static_cast<char>(codepoint));
            } else if (codepoint < 0x800) {
                utf8.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            } else if (codepoint < 0x10000) {
                utf8.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                utf8.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
        }
        label_.stringValue = [NSString stringWithUTF8String:utf8.c_str()] ?: @"";
    }

    // Top-right of the screen holding the menu bar, just below it — beside the
    // status item the user summoned this from.
    void Reposition() {
        NSScreen* screen = [NSScreen screens].firstObject;
        if (screen == nil) {
            return;
        }
        [panel_.contentView layoutSubtreeIfNeeded];
        const NSSize fit = [panel_.contentView fittingSize];
        const CGFloat width = (std::max)(fit.width, static_cast<CGFloat>(120.0));
        const CGFloat height = (std::max)(fit.height, static_cast<CGFloat>(28.0));
        const NSRect visible = screen.visibleFrame;
        const NSRect frame = NSMakeRect(NSMaxX(visible) - width - 12.0,
                                        NSMaxY(visible) - height - 8.0,
                                        width, height);
        [panel_ setFrame:frame display:YES];
    }

    ClippTypingProgressPanel* panel_ = nil;
    NSTextField* label_ = nil;
    NSTimeInterval lastUpdate_ = 0;
};

class TypingSession {
public:
    static TypingSession& Instance() {
        static TypingSession session;
        return session;
    }

    bool Active() const { return active_; }

    bool Start(TypeSchedule schedule) {
        if (active_ || schedule.events.empty()) {
            return false;
        }
        // CGEventPost is gated on Accessibility, the same grant the popup's
        // synthetic Cmd+V needs; its onboarding toast already handles asking.
        if (!AXIsProcessTrusted()) {
            g_logger.log(__FUNCTION__, Logger::Level::Info,
                         "Typing skipped: accessibility permission not granted.");
            return false;
        }
        source_ = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
        if (source_ == nullptr) {
            return false;
        }
        CGEventSourceSetUserData(source_, kClippTypingUserData);

        schedule_ = std::move(schedule);
        next_ = 0;
        held_.clear();
        active_ = true;

        InstallAbortMonitor();
        pill_.Show(ProgressText());
        timer_ = [NSTimer scheduledTimerWithTimeInterval:(kTypeEventIntervalMs / 1000.0)
                                                 repeats:YES
                                                   block:^(NSTimer* timer) {
            (void)timer;
            TypingSession::Instance().Tick();
        }];
        g_logger.log(__FUNCTION__, Logger::Level::Info,
                     "Typing %d character(s) as %zu key event(s).",
                     schedule_.characterCount, schedule_.events.size());
        return true;
    }

    void Cancel() {
        if (active_) {
            Stop(/*completed=*/false);
        }
    }

    void Tick() {
        if (!active_) {
            return;
        }
        const TypeKeyEvent& event = schedule_.events[next_++];
        // Modifiers are NOT posted as their own events here. On macOS the
        // modifier state rides in each event's flags, which is what apps read;
        // posting bare modifier key events instead would leave the flags empty
        // and produce unshifted text. A happy consequence: nothing is ever
        // "held down" system-wide, so a cancelled run cannot strand a modifier
        // the way the Windows backend must guard against.
        if (IsModifierKey(event.key)) {
            if (event.down) {
                held_.push_back(event.key);
            } else {
                const auto it = std::find(held_.rbegin(), held_.rend(), event.key);
                if (it != held_.rend()) {
                    held_.erase(std::next(it).base());
                }
            }
        } else {
            PostKey(event.key, event.down, FlagsForHeldModifiers(held_));
        }

        if (next_ >= schedule_.events.size()) {
            Stop(/*completed=*/true);
            return;
        }
        pill_.Update(ProgressText());
    }

    void Shutdown() {
        Cancel();
        pill_.Destroy();
    }

private:
    void PostKey(TypeKeyCode key, bool down, CGEventFlags flags) {
        CGEventRef event = CGEventCreateKeyboardEvent(source_, static_cast<CGKeyCode>(key), down);
        if (event == nullptr) {
            return;
        }
        CGEventSetFlags(event, flags);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    std::wstring ProgressText() const {
        const std::size_t remaining = schedule_.events.size() - next_;
        const int seconds =
            static_cast<int>((remaining * kTypeEventIntervalMs + 999) / 1000);
        std::wstring text = CLP_W(CLP_UI_TYPING_PROGRESS_PREFIX);
        text += std::to_wstring(remaining);
        text += CLP_W(CLP_UI_TYPING_PROGRESS_MIDDLE);
        text += std::to_wstring(seconds);
        text += CLP_W(CLP_UI_TYPING_PROGRESS_SUFFIX);
        return text;
    }

    // Any physical keystroke or mouse click stops the run. A PASSIVE global
    // monitor, deliberately not a CGEventTap: this is a public AppKit observer
    // that consumes nothing (so the user's key still reaches the app they are
    // looking at, matching the Windows backend), and it keeps the App Store
    // build clear of input-interception APIs. Key events need the same
    // Accessibility grant typing already requires; mouse events need none.
    void InstallAbortMonitor() {
        monitor_ = [NSEvent addGlobalMonitorForEventsMatchingMask:
            (NSEventMaskKeyDown | NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown
             | NSEventMaskOtherMouseDown)
                                                          handler:^(NSEvent* event) {
            CGEventRef cgEvent = [event CGEvent];
            if (cgEvent != nullptr
                && CGEventGetIntegerValueField(cgEvent, kCGEventSourceUserData)
                    == kClippTypingUserData) {
                return;  // our own synthesized keystroke
            }
            TypingSession::Instance().Cancel();
        }];
    }

    void RemoveAbortMonitor() {
        if (monitor_ != nil) {
            [NSEvent removeMonitor:monitor_];
            monitor_ = nil;
        }
    }

    void Stop(bool completed) {
        [timer_ invalidate];
        timer_ = nil;
        RemoveAbortMonitor();
        held_.clear();
        active_ = false;
        pill_.Hide();
        if (source_ != nullptr) {
            CFRelease(source_);
            source_ = nullptr;
        }
        const std::size_t done = next_;
        const std::size_t total = schedule_.events.size();
        schedule_ = TypeSchedule{};
        next_ = 0;
        g_logger.log(__FUNCTION__, Logger::Level::Info,
                     completed ? "Typing finished (%zu/%zu events)."
                               : "Typing canceled (%zu/%zu events).",
                     done, total);
    }

    ProgressPill pill_;
    TypeSchedule schedule_;
    std::size_t next_ = 0;
    std::vector<TypeKeyCode> held_;
    CGEventSourceRef source_ = nullptr;
    NSTimer* timer_ = nil;
    id monitor_ = nil;
    bool active_ = false;
};

}  // namespace

bool StartTyping(TypeSchedule schedule) {
    return TypingSession::Instance().Start(std::move(schedule));
}

bool IsTyping() {
    return TypingSession::Instance().Active();
}

void CancelTyping() {
    TypingSession::Instance().Cancel();
}

void ShutdownTyping() {
    TypingSession::Instance().Shutdown();
}

}  // namespace clipp
