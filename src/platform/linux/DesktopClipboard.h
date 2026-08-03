#pragma once
#ifdef __linux__

#include <cstdint>
#include <string>
#include <vector>

namespace clipp {

// One-shot read of the desktop CLIPBOARD selection over the X11 protocol. This
// covers real X11 sessions AND every mainstream Wayland desktop via XWayland
// (compositors bridge the clipboard to their XWayland server — notably GNOME,
// which refuses the wlr/ext-data-control protocols, is reachable exactly this
// way). libxcb is loaded lazily with dlopen, so the shipped binary keeps its
// avahi-only dependency list and headless installs never need X libraries.
//
// Read-only by design: reading a selection is request/response. WRITING the
// desktop clipboard is a different animal (the setter must own the selection
// and stay resident) and is deliberately out of scope here.

enum class DesktopClipboardStatus {
    Ok,           // out filled
    NoSession,    // DISPLAY unset or the X server is unreachable
    Empty,        // nobody owns the clipboard, or the owner has nothing for us
    Unsupported,  // owner offers neither text nor image/png
    Error,        // transfer/protocol failure; detail explains
};

struct DesktopClipboardData {
    // UTF-8 text withOUT the trailing NUL (the caller applies the capture
    // convention), or a raw PNG stream.
    std::vector<unsigned char> bytes;
    uint32_t formatId = 0;  // CLIPP_FORMAT_UTF8 or CLIPP_FORMAT_PNG
};

DesktopClipboardStatus ReadDesktopClipboard(DesktopClipboardData& out, std::string& detail);

}  // namespace clipp

#endif  // __linux__
