#include "TerminalLogView.h"

#ifdef __APPLE__

#include "UiHelpers.h"

#import <AppKit/AppKit.h>

namespace {
constexpr unsigned int kMaxTerminalLogLines = 1000;
constexpr CGFloat kFollowScrollTolerance = 48.0;

NSColor* DefaultTextColor() {
    return [NSColor colorWithCalibratedWhite:0.82 alpha:1.0];
}

NSColor* ColorForAnsiCode(const std::wstring& code) {
    if (code == L"0") return DefaultTextColor();
    if (code == L"90" || code == L"0;90") return [NSColor colorWithCalibratedWhite:0.50 alpha:1.0];
    if (code == L"2;36" || code == L"0;2;36") return [NSColor colorWithCalibratedRed:0.24 green:0.58 blue:0.68 alpha:1.0];
    if (code == L"36" || code == L"0;36") return [NSColor colorWithCalibratedRed:0.33 green:0.78 blue:0.92 alpha:1.0];
    if (code == L"1;32" || code == L"0;1;32") return [NSColor colorWithCalibratedRed:0.40 green:0.86 blue:0.45 alpha:1.0];
    if (code == L"1;33" || code == L"0;1;33") return [NSColor colorWithCalibratedRed:0.95 green:0.76 blue:0.25 alpha:1.0];
    if (code == L"1;31" || code == L"0;1;31") return [NSColor colorWithCalibratedRed:1.00 green:0.36 blue:0.25 alpha:1.0];
    return DefaultTextColor();
}

NSDictionary<NSAttributedStringKey, id>* AttributesForColor(NSColor* color) {
    return @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: color ?: DefaultTextColor(),
    };
}
}

MacOSTerminalLogView::MacOSTerminalLogView() {
    scrollView_ = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scrollView_.translatesAutoresizingMaskIntoConstraints = NO;
    scrollView_.hasVerticalScroller = YES;
    scrollView_.hasHorizontalScroller = NO;
    scrollView_.autohidesScrollers = YES;
    scrollView_.borderType = NSNoBorder;
    scrollView_.drawsBackground = YES;
    scrollView_.backgroundColor = [NSColor colorWithCalibratedWhite:0.06 alpha:1.0];

    textView_ = [[NSTextView alloc] initWithFrame:NSZeroRect];
    textView_.editable = NO;
    textView_.selectable = YES;
    textView_.richText = YES;
    textView_.importsGraphics = NO;
    textView_.usesFindBar = YES;
    textView_.drawsBackground = YES;
    textView_.backgroundColor = scrollView_.backgroundColor;
    textView_.textColor = DefaultTextColor();
    textView_.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    textView_.textContainerInset = NSMakeSize(10, 10);
    textView_.horizontallyResizable = NO;
    textView_.verticallyResizable = YES;
    textView_.autoresizingMask = NSViewWidthSizable;
    textView_.textContainer.widthTracksTextView = YES;
    textView_.textContainer.containerSize = NSMakeSize(scrollView_.contentSize.width, CGFLOAT_MAX);

    scrollView_.documentView = textView_;
}

NSScrollView* MacOSTerminalLogView::View() const {
    return scrollView_;
}

void MacOSTerminalLogView::AppendAnsiLogText(const std::wstring& text) {
    AppendAnsiLogText(text, true);
}

void MacOSTerminalLogView::SetAnsiLogText(const std::vector<std::wstring>& lines) {
    [textView_.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:@""]];
    lineCount_ = 0;

    for (const auto& line : lines) {
        AppendAnsiLogText(line, false);
    }

    TrimOldLines();
    ScrollToBottom();
}

unsigned int MacOSTerminalLogView::LineCount() const {
    return lineCount_;
}

NSString* MacOSTerminalLogView::PlainText() const {
    return textView_ != nil ? textView_.textStorage.string : @"";
}

void MacOSTerminalLogView::AppendAnsiLogText(const std::wstring& text, bool scrollToBottom) {
    std::wstring trimmedText = text;
    while (!trimmedText.empty() && (trimmedText.back() == L'\r' || trimmedText.back() == L'\n')) {
        trimmedText.pop_back();
    }

    if (trimmedText.empty()) {
        return;
    }

    const bool shouldScrollToBottom = scrollToBottom && IsNearBottom();

    std::size_t lineStart = 0;
    while (lineStart <= trimmedText.size()) {
        const std::size_t lineEnd = trimmedText.find_first_of(L"\r\n", lineStart);
        const std::wstring line = lineEnd == std::wstring::npos
            ? trimmedText.substr(lineStart)
            : trimmedText.substr(lineStart, lineEnd - lineStart);

        AppendAnsiLine(line);

        if (lineEnd == std::wstring::npos) {
            break;
        }

        lineStart = lineEnd + 1;
        if (trimmedText[lineEnd] == L'\r' && lineStart < trimmedText.size() && trimmedText[lineStart] == L'\n') {
            ++lineStart;
        }
    }

    TrimOldLines();
    if (shouldScrollToBottom) {
        ScrollToBottom();
    }
}

void MacOSTerminalLogView::AppendAnsiLine(const std::wstring& line) {
    NSMutableAttributedString* attributedLine = [[NSMutableAttributedString alloc] init];
    NSColor* currentColor = DefaultTextColor();
    std::wstring currentText;

    auto flushText = [&]() {
        if (currentText.empty()) {
            return;
        }
        NSAttributedString* run = [[NSAttributedString alloc] initWithString:MacOSToNSString(currentText)
                                                                  attributes:AttributesForColor(currentColor)];
        [attributedLine appendAttributedString:run];
        currentText.clear();
    };

    std::size_t i = 0;
    while (i < line.length()) {
        if (line[i] == L'\x1b' && i + 1 < line.length() && line[i + 1] == L'[') {
            flushText();

            i += 2;
            std::wstring code;
            while (i < line.length() && line[i] != L'm') {
                code += line[i];
                ++i;
            }
            if (i < line.length() && line[i] == L'm') {
                ++i;
            }

            currentColor = ColorForAnsiCode(code);
        } else {
            currentText += line[i];
            ++i;
        }
    }

    flushText();

    NSAttributedString* newline = [[NSAttributedString alloc] initWithString:@"\n"
                                                                  attributes:AttributesForColor(DefaultTextColor())];
    [attributedLine appendAttributedString:newline];
    [textView_.textStorage appendAttributedString:attributedLine];
    ++lineCount_;
}

void MacOSTerminalLogView::TrimOldLines() {
    if (lineCount_ <= kMaxTerminalLogLines) {
        return;
    }

    const unsigned int linesToRemove = lineCount_ - kMaxTerminalLogLines;
    NSString* text = textView_.textStorage.string;
    NSUInteger removeEnd = 0;
    unsigned int removed = 0;

    while (removed < linesToRemove && removeEnd < text.length) {
        NSRange newlineRange = [text rangeOfString:@"\n"
                                           options:0
                                             range:NSMakeRange(removeEnd, text.length - removeEnd)];
        if (newlineRange.location == NSNotFound) {
            break;
        }

        removeEnd = NSMaxRange(newlineRange);
        ++removed;
    }

    if (removeEnd > 0) {
        [textView_.textStorage deleteCharactersInRange:NSMakeRange(0, removeEnd)];
        lineCount_ -= removed;
    }
}

bool MacOSTerminalLogView::IsNearBottom() const {
    if (scrollView_ == nil || textView_ == nil) {
        return true;
    }

    [scrollView_ layoutSubtreeIfNeeded];
    [textView_ layoutSubtreeIfNeeded];

    const NSRect visibleRect = [textView_ visibleRect];
    const NSRect bounds = [textView_ bounds];
    const CGFloat distanceFromBottom = [textView_ isFlipped]
        ? NSMaxY(bounds) - NSMaxY(visibleRect)
        : NSMinY(visibleRect) - NSMinY(bounds);

    return distanceFromBottom <= kFollowScrollTolerance;
}

void MacOSTerminalLogView::ScrollToBottom() {
    const NSUInteger length = textView_.textStorage.length;
    [textView_ scrollRangeToVisible:NSMakeRange(length, 0)];
}

#endif
