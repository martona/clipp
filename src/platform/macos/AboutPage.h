#pragma once

#ifdef __APPLE__

@class NSView;

class MacOSAboutPage {
public:
    MacOSAboutPage();

    MacOSAboutPage(const MacOSAboutPage&) = delete;
    MacOSAboutPage& operator=(const MacOSAboutPage&) = delete;

    NSView* View() const;

private:
    void BuildView();

    NSView* root_ = nullptr;
};

#endif
