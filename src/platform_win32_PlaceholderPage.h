#pragma once

#include <winrt/Windows.UI.Xaml.Controls.h>

class PlaceholderPage {
public:
    PlaceholderPage(const wchar_t* title, const wchar_t* body);

    winrt::Windows::UI::Xaml::Controls::Grid View() const;

private:
    winrt::Windows::UI::Xaml::Controls::Grid root_{ nullptr };
};

class SettingsPage : public PlaceholderPage {
public:
    SettingsPage();
};

class LogsPage : public PlaceholderPage {
public:
    LogsPage();
};

class AboutPage : public PlaceholderPage {
public:
    AboutPage();
};
