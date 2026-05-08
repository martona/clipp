#pragma once

#include <string>

#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>

class TerminalLogView {
public:
    TerminalLogView();

    winrt::Windows::UI::Xaml::Controls::ScrollViewer View() const;
    void AppendAnsiLogText(const std::wstring& text);

private:
    winrt::Windows::UI::Xaml::Documents::Paragraph CreateParagraphForAnsiLine(const std::wstring& line) const;
    winrt::Windows::UI::Xaml::Media::Brush BrushForAnsiCode(const std::wstring& code) const;
    void TrimOldLines();
    void ScrollToBottom();

    winrt::Windows::UI::Xaml::Controls::ScrollViewer scrollViewer_{ nullptr };
    winrt::Windows::UI::Xaml::Controls::RichTextBlock richTextBlock_{ nullptr };

    winrt::Windows::UI::Xaml::Media::Brush defaultBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush grayBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush cyanBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush greenBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush yellowBrush_{ nullptr };
    winrt::Windows::UI::Xaml::Media::Brush redBrush_{ nullptr };
};
