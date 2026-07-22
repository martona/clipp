#pragma once

#include <cstdint>
#include <string>

// Ambient send/receive feedback for the GUI: the core clipboard paths report
// flow events here, and the platform shell (Win32 tray / macOS status item)
// registers a handler that nudges its icon and remembers the event for the
// hover tooltip / menu header. Builds that never register a handler (headless,
// iOS, the CLI verbs) no-op. Handlers are invoked on the REPORTING thread
// (network or clipboard-watcher) and must marshal to their UI thread.
namespace clipp {

enum class ClipboardFlowDirection { Sent, Received };

using ClipboardFlowHandler = void (*)(ClipboardFlowDirection direction,
                                      const std::string& peerNameUtf8);

void SetClipboardFlowHandler(ClipboardFlowHandler handler);

// peerNameUtf8: the origin device for Received; empty for Sent (a broadcast
// has no single counterparty).
void NotifyClipboardFlow(ClipboardFlowDirection direction, const std::string& peerNameUtf8);

// "just now" / "12s ago" / "3m ago" / "2h ago" / "5d ago". Computed by the
// tooltip/menu code at hover/open time, so the text never goes stale.
std::string FormatRelativeAgeUtf8(uint64_t ageSeconds);

}  // namespace clipp
