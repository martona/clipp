#pragma once

#include "TerminalLogView.h"

#include <memory>
#include <mutex>
#include <string>

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

class LogsPage {
public:
    LogsPage();
    ~LogsPage();

    LogsPage(const LogsPage&) = delete;
    LogsPage& operator=(const LogsPage&) = delete;

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

    void OnShown();
    void OnHidden();
    void OnDestroy();

private:
    void BuildView();
    void ReflectLogLine(const std::wstring& line);

    static void LogReflectorCallback(const std::wstring& line);

    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
    winrt::Windows::System::DispatcherQueue uiDispatcher_{ nullptr };
    std::unique_ptr<TerminalLogView> terminalLogView_;
    std::mutex terminalLogViewMutex_;
    bool logReflectorRegistered_ = false;
};
