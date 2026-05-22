#pragma once

#include <winrt/Windows.UI.Xaml.Controls.h>

class AboutPage {
public:
    AboutPage();

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

private:
    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
};
