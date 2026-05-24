#include "LogsPage.h"

#include "Clipboard.h"
#include "Logger.h"
#include "platform/uistrings.h"

#include <cwchar>
#include <limits>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>

extern Logger g_logger;

namespace {
LogsPage* g_logReflectorTarget = nullptr;

ClipboardPayload MakeTextClipboardPayload(const std::wstring& text) {
    ClipboardPayload payload{};
    payload.formatId = CF_UNICODETEXT;

    const size_t utf8Bytes = utf16_to_utf8(text.c_str(), text.size(), nullptr, 0);
    payload.rawData.resize(utf8Bytes + 1);
    if (utf8Bytes > 0) {
        utf16_to_utf8(text.c_str(), text.size(), reinterpret_cast<char*>(payload.rawData.data()), utf8Bytes);
    }
    payload.rawData[utf8Bytes] = '\0';

    return payload;
}
}

LogsPage::LogsPage() {
    BuildView();
}

LogsPage::~LogsPage() {
    OnDestroy();
}

winrt::Windows::UI::Xaml::Controls::Grid LogsPage::View() const {
    return root_;
}

void LogsPage::BuildView() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;

    root_ = Grid();
    root_.HorizontalAlignment(HorizontalAlignment::Stretch);
    root_.VerticalAlignment(VerticalAlignment::Stretch);

    RowDefinition headerRow;
    headerRow.Height(GridLength{ 1, GridUnitType::Auto });
    RowDefinition logRow;
    logRow.Height(GridLength{ 1, GridUnitType::Star });
    root_.RowDefinitions().Append(headerRow);
    root_.RowDefinitions().Append(logRow);

    StackPanel header;
    header.Orientation(Orientation::Vertical);
    header.Padding(ThicknessHelper::FromLengths(24, 24, 24, 16));
    header.Spacing(8);

    TextBlock heading;
    heading.Text(CLP_W(CLP_UI_LOGS));
    heading.FontSize(28);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    heading.TextWrapping(TextWrapping::Wrap);
    header.Children().Append(heading);

    Grid introRow;
    ColumnDefinition introColumn;
    introColumn.Width(GridLength{ 1, GridUnitType::Star });
    ColumnDefinition copyButtonColumn;
    copyButtonColumn.Width(GridLength{ 1, GridUnitType::Auto });
    introRow.ColumnDefinitions().Append(introColumn);
    introRow.ColumnDefinitions().Append(copyButtonColumn);

    TextBlock intro;
    intro.Text(CLP_W(CLP_UI_LIVE_DIAGNOSTIC_OUTPUT));
    intro.FontSize(14);
    intro.TextWrapping(TextWrapping::WrapWholeWords);
    intro.Opacity(0.75);
    intro.VerticalAlignment(VerticalAlignment::Center);

    copyLogsButton_ = Button();
    copyLogsButton_.HorizontalAlignment(HorizontalAlignment::Right);
    copyLogsButton_.VerticalAlignment(VerticalAlignment::Center);
    copyLogsButton_.Margin(ThicknessHelper::FromLengths(12, 0, 0, 0));
    copyLogsButton_.Padding(ThicknessHelper::FromLengths(12, 6, 12, 6));
    copyLogsButton_.Click([this](auto const&, auto const&) {
        CopyLogsToClipboard();
    });

    Grid::SetColumn(intro, 0);
    Grid::SetColumn(copyLogsButton_, 1);
    introRow.Children().Append(intro);
    introRow.Children().Append(copyLogsButton_);
    header.Children().Append(introRow);

    terminalLogView_ = std::make_unique<TerminalLogView>();
    auto logView = terminalLogView_->View();
    logView.HorizontalAlignment(HorizontalAlignment::Stretch);
    logView.VerticalAlignment(VerticalAlignment::Stretch);
    logView.Margin(ThicknessHelper::FromLengths(24, 0, 24, 24));
    logView.MinHeight(0);
    logView.MaxHeight(std::numeric_limits<double>::infinity());

    Grid::SetRow(header, 0);
    Grid::SetRow(logView, 1);
    root_.Children().Append(header);
    root_.Children().Append(logView);

    uiDispatcher_ = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    UpdateCopyLogsButtonLabel();
}

void LogsPage::OnShown() {
    if (!logReflectorRegistered_) {
        g_logReflectorTarget = this;
        const auto logHistory = g_logger.AddLogReflector(LogReflectorCallback);
        {
            std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
            if (terminalLogView_) {
                terminalLogView_->SetAnsiLogText(logHistory);
            }
        }
        UpdateCopyLogsButtonLabel();
        logReflectorRegistered_ = true;
    }
}

void LogsPage::OnHidden() {
    if (logReflectorRegistered_) {
        g_logger.RemoveLogReflector(LogReflectorCallback);
        if (g_logReflectorTarget == this) {
            g_logReflectorTarget = nullptr;
        }
        logReflectorRegistered_ = false;
    }
}

void LogsPage::OnDestroy() {
    OnHidden();
    {
        std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
        terminalLogView_.reset();
    }
}

void LogsPage::ReflectLogLine(const std::wstring& line) {
    if (!uiDispatcher_) {
        return;
    }

    uiDispatcher_.TryEnqueue([this, line]() {
        {
            std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
            if (terminalLogView_) {
                terminalLogView_->AppendAnsiLogText(line);
            }
        }
        UpdateCopyLogsButtonLabel();
    });
}

void LogsPage::CopyLogsToClipboard() {
    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
        if (terminalLogView_) {
            text = terminalLogView_->PlainText();
        }
    }

    ClipboardPayload payload = MakeTextClipboardPayload(text);
    SetClipboardData(payload, false);
}

void LogsPage::UpdateCopyLogsButtonLabel() {
    if (!copyLogsButton_) {
        return;
    }

    uint32_t lineCount = 0;
    {
        std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
        if (terminalLogView_) {
            lineCount = terminalLogView_->LineCount();
        }
    }

    wchar_t label[64]{};
    swprintf_s(label, CLP_W(CLP_UI_COPY_LOG_LINES_FORMAT), static_cast<int>(lineCount));
    copyLogsButton_.Content(winrt::box_value(winrt::hstring{ label }));
}

void LogsPage::LogReflectorCallback(const std::wstring& line) {
    if (g_logReflectorTarget) {
        g_logReflectorTarget->ReflectLogLine(line);
    }
}
