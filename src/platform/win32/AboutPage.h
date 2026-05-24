#pragma once

#include <functional>

#include <winrt/Windows.UI.Xaml.Controls.h>

class AboutPage {
public:
    explicit AboutPage(std::function<void()> diagnosticsCallback = {});

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

private:
    std::function<void()> diagnosticsCallback_;
    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
};
