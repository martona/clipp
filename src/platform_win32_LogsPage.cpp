#include "platform_win32_LogsPage.h"

#include "Logger.h"

#include <limits>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

extern Logger g_logger;

namespace {
LogsPage* g_logReflectorTarget = nullptr;
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
    heading.Text(L"Logs");
    heading.FontSize(28);
    heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    heading.TextWrapping(TextWrapping::Wrap);
    header.Children().Append(heading);

    TextBlock intro;
    intro.Text(L"Live diagnostic output from Clipp.");
    intro.FontSize(14);
    intro.TextWrapping(TextWrapping::WrapWholeWords);
    intro.Opacity(0.75);
    header.Children().Append(intro);

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
}

void LogsPage::OnShown() {
    if (!logReflectorRegistered_) {
        g_logReflectorTarget = this;
        g_logger.AddLogReflector(LogReflectorCallback);
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
        std::lock_guard<std::mutex> lock(terminalLogViewMutex_);
        if (terminalLogView_) {
            terminalLogView_->AppendAnsiLogText(line);
        }
    });
}

void LogsPage::LogReflectorCallback(const std::wstring& line) {
    if (g_logReflectorTarget) {
        g_logReflectorTarget->ReflectLogLine(line);
    }
}
