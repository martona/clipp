#pragma once

#ifdef __APPLE__

#include <string>
#include <vector>

#include "../../TerminalLogBuffer.h"

@class NSScrollView;
@class NSTextView;
@class NSString;

class MacOSTerminalLogView {
public:
    MacOSTerminalLogView();

    NSScrollView* View() const;
    void AppendAnsiLogText(const std::wstring& text);
    void SetAnsiLogText(const std::vector<std::wstring>& lines);
    unsigned int LineCount() const;
    NSString* PlainText() const;

private:
    void AppendAnsiLogText(const std::wstring& text, bool scrollToBottom);
    void AppendLine(const TerminalLogBuffer::Line& line);
    void RemoveOldestLines(std::size_t lineCount);
    bool IsNearBottom() const;
    void ScrollToBottom();

    NSScrollView* scrollView_ = nullptr;
    NSTextView* textView_ = nullptr;
    TerminalLogBuffer buffer_;
};

#endif
