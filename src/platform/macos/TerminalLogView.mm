#include "TerminalLogView.h"

#ifdef __APPLE__

#include "UiHelpers.h"

#import <AppKit/AppKit.h>

namespace {
constexpr CGFloat kFollowScrollTolerance = 48.0;

NSColor* DefaultTextColor() {
    return [NSColor colorWithCalibratedWhite:0.82 alpha:1.0];
}

NSColor* ColorForRun(TerminalLogBuffer::Color color) {
    if (color == TerminalLogBuffer::Color::Gray) return [NSColor colorWithCalibratedWhite:0.50 alpha:1.0];
    if (color == TerminalLogBuffer::Color::DimCyan) return [NSColor colorWithCalibratedRed:0.24 green:0.58 blue:0.68 alpha:1.0];
    if (color == TerminalLogBuffer::Color::Cyan) return [NSColor colorWithCalibratedRed:0.33 green:0.78 blue:0.92 alpha:1.0];
    if (color == TerminalLogBuffer::Color::Green) return [NSColor colorWithCalibratedRed:0.40 green:0.86 blue:0.45 alpha:1.0];
    if (color == TerminalLogBuffer::Color::Yellow) return [NSColor colorWithCalibratedRed:0.95 green:0.76 blue:0.25 alpha:1.0];
    if (color == TerminalLogBuffer::Color::Red) return [NSColor colorWithCalibratedRed:1.00 green:0.36 blue:0.25 alpha:1.0];
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
    buffer_.SetAnsiLogText(lines);
    [textView_.textStorage setAttributedString:[[NSAttributedString alloc] initWithString:@""]];

    for (const auto& line : buffer_.Lines()) {
        AppendLine(line);
    }

    ScrollToBottom();
}

unsigned int MacOSTerminalLogView::LineCount() const {
    return static_cast<unsigned int>(buffer_.LineCount());
}

NSString* MacOSTerminalLogView::PlainText() const {
    return MacOSToNSString(buffer_.PlainText());
}

void MacOSTerminalLogView::AppendAnsiLogText(const std::wstring& text, bool scrollToBottom) {
    const bool shouldScrollToBottom = scrollToBottom && IsNearBottom();
    const TerminalLogBuffer::AppendResult update = buffer_.AppendAnsiLogText(text);

    if (update.addedLines.empty()) {
        return;
    }

    RemoveOldestLines(update.removedLineCount);
    for (const auto& line : update.addedLines) {
        AppendLine(line);
    }
    if (shouldScrollToBottom) {
        ScrollToBottom();
    }
}

void MacOSTerminalLogView::AppendLine(const TerminalLogBuffer::Line& line) {
    NSMutableAttributedString* attributedLine = [[NSMutableAttributedString alloc] init];

    for (const auto& logRun : line.runs) {
        NSAttributedString* run = [[NSAttributedString alloc] initWithString:MacOSToNSString(logRun.text)
                                                                  attributes:AttributesForColor(ColorForRun(logRun.color))];
        [attributedLine appendAttributedString:run];
    }

    NSAttributedString* newline = [[NSAttributedString alloc] initWithString:@"\n"
                                                                  attributes:AttributesForColor(DefaultTextColor())];
    [attributedLine appendAttributedString:newline];
    [textView_.textStorage appendAttributedString:attributedLine];
}

void MacOSTerminalLogView::RemoveOldestLines(std::size_t lineCount) {
    if (lineCount == 0) {
        return;
    }

    NSString* text = textView_.textStorage.string;
    NSUInteger removeEnd = 0;
    std::size_t removed = 0;

    while (removed < lineCount && removeEnd < text.length) {
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
