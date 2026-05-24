#include "TerminalLogView.h"

#include <cstdint>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>

static constexpr uint32_t kMaxTerminalLogLines = 1000;
static constexpr double kFollowScrollToleranceDips = 48.0;

TerminalLogView::TerminalLogView() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    defaultBrush_ = SolidColorBrush(winrt::Windows::UI::Colors::LightGray());
    grayBrush_    = SolidColorBrush(winrt::Windows::UI::Colors::Gray());
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
    richTextBlock_.Blocks().Clear();
    for (const auto& line : lines) {
        AppendAnsiLogText(line, false);
    }
    TrimOldLines();
    ScrollToBottom();
}

uint32_t TerminalLogView::LineCount() const {
    return richTextBlock_ ? richTextBlock_.Blocks().Size() : 0;
}

std::wstring TerminalLogView::PlainText() const {
    std::wstring text;
    if (!richTextBlock_) {
        return text;
    }

    auto blocks = richTextBlock_.Blocks();
    for (uint32_t blockIndex = 0; blockIndex < blocks.Size(); ++blockIndex) {
        auto paragraph = blocks.GetAt(blockIndex).try_as<winrt::Windows::UI::Xaml::Documents::Paragraph>();
        if (paragraph) {
            auto inlines = paragraph.Inlines();
            for (uint32_t inlineIndex = 0; inlineIndex < inlines.Size(); ++inlineIndex) {
                auto run = inlines.GetAt(inlineIndex).try_as<winrt::Windows::UI::Xaml::Documents::Run>();
                if (run) {
                    const winrt::hstring runText = run.Text();
                    text.append(runText.c_str(), runText.size());
                }
            }
        }
        text.push_back(L'\n');
    }

    return text;
}

void TerminalLogView::AppendAnsiLogText(const std::wstring& text, bool scrollToBottom) {
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

        richTextBlock_.Blocks().Append(CreateParagraphForAnsiLine(line));

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

winrt::Windows::UI::Xaml::Documents::Paragraph TerminalLogView::CreateParagraphForAnsiLine(const std::wstring& line) const {
    using namespace winrt::Windows::UI::Xaml::Documents;

    Paragraph paragraph;
    auto currentBrush = defaultBrush_;
    std::wstring currentText;

    auto flushText = [&]() {
        if (!currentText.empty()) {
            Run run;
            run.Text(currentText);
            run.Foreground(currentBrush);
            paragraph.Inlines().Append(run);
            currentText.clear();
        }
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

            currentBrush = BrushForAnsiCode(code);
        } else {
            currentText += line[i];
            ++i;
        }
    }

    flushText();
    return paragraph;
}

winrt::Windows::UI::Xaml::Media::Brush TerminalLogView::BrushForAnsiCode(const std::wstring& code) const {
    if (code == L"0") return defaultBrush_;
    if (code == L"90") return grayBrush_;
    if (code == L"36") return cyanBrush_;
    if (code == L"1;32") return greenBrush_;
    if (code == L"1;33") return yellowBrush_;
    if (code == L"1;31") return redBrush_;
    return defaultBrush_;
}

void TerminalLogView::TrimOldLines() {
    auto blocks = richTextBlock_.Blocks();
    while (blocks.Size() > kMaxTerminalLogLines) {
        blocks.RemoveAt(0);
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
