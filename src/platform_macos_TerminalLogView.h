#pragma once

#ifdef __APPLE__

#include <string>
#include <vector>

@class NSScrollView;
@class NSTextView;

class MacOSTerminalLogView {
public:
    MacOSTerminalLogView();

    NSScrollView* View() const;
    void AppendAnsiLogText(const std::wstring& text);
    void SetAnsiLogText(const std::vector<std::wstring>& lines);

private:
    void AppendAnsiLogText(const std::wstring& text, bool scrollToBottom);
    void AppendAnsiLine(const std::wstring& line);
    void TrimOldLines();
    void ScrollToBottom();

    NSScrollView* scrollView_ = nullptr;
    NSTextView* textView_ = nullptr;
    unsigned int lineCount_ = 0;
};

#endif
