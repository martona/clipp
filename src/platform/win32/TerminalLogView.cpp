#include "TerminalLogView.h"

#include <cstdint>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>

static constexpr double kFollowScrollToleranceDips = 48.0;

TerminalLogView::TerminalLogView() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    defaultBrush_ = SolidColorBrush(winrt::Windows::UI::Colors::LightGray());
    grayBrush_    = SolidColorBrush(winrt::Windows::UI::Colors::Gray());
    dimCyanBrush_ = SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(255, 80, 150, 165));
    cyanBrush_    = SolidColorBrush(winrt::Windows::UI::Colors::Cyan());
    greenBrush_   = SolidColorBrush(winrt::Windows::UI::Colors::LimeGreen());
    yellowBrush_  = SolidColorBrush(winrt::Windows::UI::Colors::Gold());
    redBrush_     = SolidColorBrush(winrt::Windows::UI::Colors::OrangeRed());

    scrollViewer_ = ScrollViewer{};
    scrollViewer_.Background(SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(255, 12, 12, 12)));
    scrollViewer_.Padding(ThicknessHelper::FromUniformLength(10));
    scrollViewer_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    scrollViewer_.MinHeight(150);
    scrollViewer_.MaxHeight(450);

    richTextBlock_ = RichTextBlock{};
    richTextBlock_.FontFamily(FontFamily(L"Consolas"));
    richTextBlock_.FontSize(13);
    richTextBlock_.TextWrapping(TextWrapping::Wrap);

    scrollViewer_.Content(richTextBlock_);
}

winrt::Windows::UI::Xaml::Controls::ScrollViewer TerminalLogView::View() const {
    return scrollViewer_;
}

void TerminalLogView::AppendAnsiLogText(const std::wstring& text) {
    AppendAnsiLogText(text, true);
}

void TerminalLogView::SetAnsiLogText(const std::vector<std::wstring>& lines) {
    buffer_.SetAnsiLogText(lines);
    richTextBlock_.Blocks().Clear();

    for (const auto& line : buffer_.Lines()) {
        richTextBlock_.Blocks().Append(CreateParagraphForLine(line));
    }
    ScrollToBottom();
}

uint32_t TerminalLogView::LineCount() const {
    return static_cast<uint32_t>(buffer_.LineCount());
}

std::wstring TerminalLogView::PlainText() const {
    return buffer_.PlainText();
}

void TerminalLogView::AppendAnsiLogText(const std::wstring& text, bool scrollToBottom) {
    const bool shouldScrollToBottom = scrollToBottom && IsNearBottom();
    const TerminalLogBuffer::AppendResult update = buffer_.AppendAnsiLogText(text);

    if (update.addedLines.empty()) {
        return;
    }

    RemoveOldestLines(update.removedLineCount);
    for (const auto& line : update.addedLines) {
        richTextBlock_.Blocks().Append(CreateParagraphForLine(line));
    }
    if (shouldScrollToBottom) {
        ScrollToBottom();
    }
}

winrt::Windows::UI::Xaml::Documents::Paragraph TerminalLogView::CreateParagraphForLine(const TerminalLogBuffer::Line& line) const {
    using namespace winrt::Windows::UI::Xaml::Documents;

    Paragraph paragraph;
    for (const auto& logRun : line.runs) {
        Run run;
        run.Text(logRun.text);
        run.Foreground(BrushForColor(logRun.color));
        paragraph.Inlines().Append(run);
    }
    return paragraph;
}

winrt::Windows::UI::Xaml::Media::Brush TerminalLogView::BrushForColor(TerminalLogBuffer::Color color) const {
    switch (color) {
    case TerminalLogBuffer::Color::Gray:
        return grayBrush_;
    case TerminalLogBuffer::Color::DimCyan:
        return dimCyanBrush_;
    case TerminalLogBuffer::Color::Cyan:
        return cyanBrush_;
    case TerminalLogBuffer::Color::Green:
        return greenBrush_;
    case TerminalLogBuffer::Color::Yellow:
        return yellowBrush_;
    case TerminalLogBuffer::Color::Red:
        return redBrush_;
    case TerminalLogBuffer::Color::Default:
    default:
        return defaultBrush_;
    }
}

void TerminalLogView::RemoveOldestLines(std::size_t lineCount) {
    auto blocks = richTextBlock_.Blocks();
    while (lineCount > 0 && blocks.Size() > 0) {
        blocks.RemoveAt(0);
        --lineCount;
    }
}

bool TerminalLogView::IsNearBottom() const {
    if (!scrollViewer_) {
        return true;
    }

    scrollViewer_.UpdateLayout();
    return scrollViewer_.ScrollableHeight() - scrollViewer_.VerticalOffset() <= kFollowScrollToleranceDips;
}

void TerminalLogView::ScrollToBottom() {
    scrollViewer_.UpdateLayout();
    scrollViewer_.ChangeView(nullptr, scrollViewer_.ScrollableHeight(), nullptr);
}
