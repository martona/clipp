#pragma once

#ifdef __APPLE__

#include <functional>

@class NSObject;
@class NSView;

class MacOSAboutPage {
public:
    explicit MacOSAboutPage(std::function<void()> diagnosticsCallback = {});

    MacOSAboutPage(const MacOSAboutPage&) = delete;
    MacOSAboutPage& operator=(const MacOSAboutPage&) = delete;

    NSView* View() const;
    void ShowDiagnostics();

private:
    void BuildView();

    std::function<void()> diagnosticsCallback_;
    NSObject* diagnosticsButtonTarget_ = nullptr;
    NSView* root_ = nullptr;
};

#endif
