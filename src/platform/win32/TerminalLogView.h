#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../TerminalLogBuffer.h"

#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>

class TerminalLogView {
public:
    TerminalLogView();

    winrt::Windows::UI::Xaml::Controls::ScrollViewer View() const;
    void AppendAnsiLogText(const std::wstring& text);
    void SetAnsiLogText(const std::vector<std::wstring>& lines);
    uint32_t LineCount() const;
    std::wstring PlainText() const;

private:
    void AppendAnsiLogText(const std::wstring& text, bool scrollToBottom);
    winrt::Windows::UI::Xaml::Documents::Paragraph CreateParagraphForLine(const TerminalLogBuffer::Line& line) const;
    winrt::Windows::UI::Xaml::Media::Brush BrushForColor(TerminalLogBuffer::Color color) const;
    void RemoveOldestLines(std::size_t lineCount);
    bool IsNearBottom() const;
    void ScrollToBottom();

    winrt::Windows::UI::Xaml::Controls::ScrollViewer scrollViewer_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::RichTextBlock richTextBlock_{ nullptr };

    winrt::Windows::UI::Xaml::Media::Brush defaultBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush grayBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush dimCyanBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush cyanBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush greenBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush yellowBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush redBrush_{ nullptr };
    TerminalLogBuffer buffer_;
};
