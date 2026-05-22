#pragma once

#include <winrt/Windows.UI.Xaml.Controls.h>

class PlaceholderPage {
public:
    PlaceholderPage(const wchar_t* title, const wchar_t* body);

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

private:
    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
};

class AboutPage {
public:
    AboutPage();

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

private:
    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
};
