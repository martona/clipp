#ifdef __linux__

#include "platform/linux/DesktopClipboard.h"

#include "ClipboardFormat.h"
#include "ClipboardLimits.h"

#include <dlfcn.h>
#include <poll.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace clipp {
namespace {

// ---------------------------------------------------------------------------
// dlopen'd libxcb surface. Headers give us the types (stable wire ABI); the
// symbols are resolved at runtime so libxcb.so.1 is a desktop-only, on-demand
// dependency — a headless server never touches it.
// ---------------------------------------------------------------------------
struct XcbApi {
    void* lib = nullptr;

    xcb_connection_t* (*connect)(const char*, int*) = nullptr;
    int (*connection_has_error)(xcb_connection_t*) = nullptr;
    void (*disconnect)(xcb_connection_t*) = nullptr;
    int (*get_file_descriptor)(xcb_connection_t*) = nullptr;
    const xcb_setup_t* (*get_setup)(xcb_connection_t*) = nullptr;
    xcb_screen_iterator_t (*setup_roots_iterator)(const xcb_setup_t*) = nullptr;
    uint32_t (*generate_id)(xcb_connection_t*) = nullptr;
    xcb_void_cookie_t (*create_window)(xcb_connection_t*, uint8_t, xcb_window_t, xcb_window_t,
                                       int16_t, int16_t, uint16_t, uint16_t, uint16_t, uint16_t,
                                       xcb_visualid_t, uint32_t, const void*) = nullptr;
    xcb_void_cookie_t (*destroy_window)(xcb_connection_t*, xcb_window_t) = nullptr;
    xcb_intern_atom_cookie_t (*intern_atom)(xcb_connection_t*, uint8_t, uint16_t, const char*) = nullptr;
    xcb_intern_atom_reply_t* (*intern_atom_reply)(xcb_connection_t*, xcb_intern_atom_cookie_t,
                                                  xcb_generic_error_t**) = nullptr;
    xcb_get_selection_owner_cookie_t (*get_selection_owner)(xcb_connection_t*, xcb_atom_t) = nullptr;
    xcb_get_selection_owner_reply_t* (*get_selection_owner_reply)(xcb_connection_t*,
                                                                  xcb_get_selection_owner_cookie_t,
                                                                  xcb_generic_error_t**) = nullptr;
    xcb_void_cookie_t (*convert_selection)(xcb_connection_t*, xcb_window_t, xcb_atom_t, xcb_atom_t,
                                           xcb_atom_t, xcb_timestamp_t) = nullptr;
    xcb_void_cookie_t (*delete_property)(xcb_connection_t*, xcb_window_t, xcb_atom_t) = nullptr;
    int (*flush)(xcb_connection_t*) = nullptr;
    xcb_generic_event_t* (*poll_for_event)(xcb_connection_t*) = nullptr;
    xcb_get_property_cookie_t (*get_property)(xcb_connection_t*, uint8_t, xcb_window_t, xcb_atom_t,
                                              xcb_atom_t, uint32_t, uint32_t) = nullptr;
    xcb_get_property_reply_t* (*get_property_reply)(xcb_connection_t*, xcb_get_property_cookie_t,
                                                    xcb_generic_error_t**) = nullptr;
    void* (*get_property_value)(const xcb_get_property_reply_t*) = nullptr;
    int (*get_property_value_length)(const xcb_get_property_reply_t*) = nullptr;

    bool Load() {
        lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            return false;
        }
        const auto sym = [this](const char* name) { return dlsym(lib, name); };
        connect = reinterpret_cast<decltype(connect)>(sym("xcb_connect"));
        connection_has_error = reinterpret_cast<decltype(connection_has_error)>(sym("xcb_connection_has_error"));
        disconnect = reinterpret_cast<decltype(disconnect)>(sym("xcb_disconnect"));
        get_file_descriptor = reinterpret_cast<decltype(get_file_descriptor)>(sym("xcb_get_file_descriptor"));
        get_setup = reinterpret_cast<decltype(get_setup)>(sym("xcb_get_setup"));
        setup_roots_iterator = reinterpret_cast<decltype(setup_roots_iterator)>(sym("xcb_setup_roots_iterator"));
        generate_id = reinterpret_cast<decltype(generate_id)>(sym("xcb_generate_id"));
        create_window = reinterpret_cast<decltype(create_window)>(sym("xcb_create_window"));
        destroy_window = reinterpret_cast<decltype(destroy_window)>(sym("xcb_destroy_window"));
        intern_atom = reinterpret_cast<decltype(intern_atom)>(sym("xcb_intern_atom"));
        intern_atom_reply = reinterpret_cast<decltype(intern_atom_reply)>(sym("xcb_intern_atom_reply"));
        get_selection_owner = reinterpret_cast<decltype(get_selection_owner)>(sym("xcb_get_selection_owner"));
        get_selection_owner_reply = reinterpret_cast<decltype(get_selection_owner_reply)>(sym("xcb_get_selection_owner_reply"));
        convert_selection = reinterpret_cast<decltype(convert_selection)>(sym("xcb_convert_selection"));
        delete_property = reinterpret_cast<decltype(delete_property)>(sym("xcb_delete_property"));
        flush = reinterpret_cast<decltype(flush)>(sym("xcb_flush"));
        poll_for_event = reinterpret_cast<decltype(poll_for_event)>(sym("xcb_poll_for_event"));
        get_property = reinterpret_cast<decltype(get_property)>(sym("xcb_get_property"));
        get_property_reply = reinterpret_cast<decltype(get_property_reply)>(sym("xcb_get_property_reply"));
        get_property_value = reinterpret_cast<decltype(get_property_value)>(sym("xcb_get_property_value"));
        get_property_value_length = reinterpret_cast<decltype(get_property_value_length)>(sym("xcb_get_property_value_length"));
        return connect && connection_has_error && disconnect && get_file_descriptor && get_setup
            && setup_roots_iterator && generate_id && create_window && destroy_window && intern_atom
            && intern_atom_reply && get_selection_owner && get_selection_owner_reply
            && convert_selection && delete_property && flush && poll_for_event && get_property
            && get_property_reply && get_property_value && get_property_value_length;
    }

    ~XcbApi() {
        // One-shot process; keep the library resident until exit. dlclose here
        // would only add an unload/reload hazard for zero benefit.
    }
};

using Clock = std::chrono::steady_clock;

// Whole-read deadline. A healthy owner answers in milliseconds; INCR transfers
// of tens of MB still fit comfortably. A dead owner (crashed app that still
// holds the selection) is the case this bounds.
constexpr std::chrono::milliseconds kDeadline{ 3000 };

struct Reader {
    XcbApi& x;
    xcb_connection_t* conn = nullptr;
    xcb_window_t window = 0;
    Clock::time_point deadline;

    xcb_atom_t atomClipboard = XCB_ATOM_NONE;
    xcb_atom_t atomTargets = XCB_ATOM_NONE;
    xcb_atom_t atomUtf8 = XCB_ATOM_NONE;
    xcb_atom_t atomPng = XCB_ATOM_NONE;
    xcb_atom_t atomIncr = XCB_ATOM_NONE;
    xcb_atom_t atomDest = XCB_ATOM_NONE;  // our transfer property

    explicit Reader(XcbApi& api) : x(api), deadline(Clock::now() + kDeadline) {}

    ~Reader() {
        if (conn != nullptr) {
            if (window != 0) {
                x.destroy_window(conn, window);
                x.flush(conn);
            }
            x.disconnect(conn);
        }
    }

    xcb_atom_t Intern(const char* name) {
        xcb_intern_atom_reply_t* reply =
            x.intern_atom_reply(conn, x.intern_atom(conn, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
        if (reply == nullptr) {
            return XCB_ATOM_NONE;
        }
        const xcb_atom_t atom = reply->atom;
        std::free(reply);
        return atom;
    }

    bool Setup(std::string& detail) {
        int screenIndex = 0;
        conn = x.connect(nullptr, &screenIndex);
        if (conn == nullptr || x.connection_has_error(conn) != 0) {
            detail = "cannot connect to the X server";
            return false;
        }
        // Any screen's root works as the parent of a never-mapped transfer
        // window; take the first rather than walking to screenIndex.
        const xcb_screen_iterator_t it = x.setup_roots_iterator(x.get_setup(conn));
        if (it.rem <= 0 || it.data == nullptr) {
            detail = "X server reports no screens";
            return false;
        }
        xcb_screen_t* screen = it.data;

        // Never mapped; exists only as the destination mailbox for property
        // transfers, so the reader works with no compositor interaction at all.
        window = x.generate_id(conn);
        const uint32_t eventMask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        x.create_window(conn, XCB_COPY_FROM_PARENT, window, screen->root, -1, -1, 1, 1, 0,
                        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_EVENT_MASK, &eventMask);

        atomClipboard = Intern("CLIPBOARD");
        atomTargets = Intern("TARGETS");
        atomUtf8 = Intern("UTF8_STRING");
        atomPng = Intern("image/png");
        atomIncr = Intern("INCR");
        atomDest = Intern("CLIPP_SELECTION");
        if (atomClipboard == XCB_ATOM_NONE || atomTargets == XCB_ATOM_NONE || atomUtf8 == XCB_ATOM_NONE
            || atomIncr == XCB_ATOM_NONE || atomDest == XCB_ATOM_NONE) {
            detail = "failed to intern X atoms";
            return false;
        }
        return true;
    }

    bool HasOwner() {
        xcb_get_selection_owner_reply_t* reply =
            x.get_selection_owner_reply(conn, x.get_selection_owner(conn, atomClipboard), nullptr);
        if (reply == nullptr) {
            return false;
        }
        const bool owned = reply->owner != XCB_NONE;
        std::free(reply);
        return owned;
    }

    // Blocks (bounded by the shared deadline) until an event of `wantedType`
    // arrives that `match` accepts; unrelated events are discarded. Returns
    // nullptr on timeout or connection error; caller frees the event.
    template <typename MatchFn>
    xcb_generic_event_t* WaitForEvent(uint8_t wantedType, MatchFn match) {
        const int fd = x.get_file_descriptor(conn);
        for (;;) {
            xcb_generic_event_t* ev;
            while ((ev = x.poll_for_event(conn)) != nullptr) {
                if ((ev->response_type & 0x7F) == wantedType && match(ev)) {
                    return ev;
                }
                std::free(ev);
            }
            if (x.connection_has_error(conn) != 0) {
                return nullptr;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.count() <= 0) {
                return nullptr;
            }
            pollfd pfd{ fd, POLLIN, 0 };
            if (::poll(&pfd, 1, static_cast<int>(remaining.count())) < 0) {
                return nullptr;
            }
        }
    }

    // Reads (and deletes) the whole transfer property. `outType` receives the
    // property type so the caller can detect INCR. False = protocol error or
    // the 64MB cap; a plain empty property reads as success with empty bytes.
    bool FetchProperty(std::vector<unsigned char>& out, xcb_atom_t& outType, std::string& detail) {
        out.clear();
        outType = XCB_ATOM_NONE;
        uint32_t offsetWords = 0;
        for (;;) {
            // 1MB per round trip; delete only fires once the tail is consumed.
            xcb_get_property_reply_t* reply = x.get_property_reply(
                conn,
                x.get_property(conn, 1, window, atomDest, XCB_GET_PROPERTY_TYPE_ANY, offsetWords, 0x40000),
                nullptr);
            if (reply == nullptr) {
                detail = "clipboard property read failed";
                return false;
            }
            outType = reply->type;
            const int len = x.get_property_value_length(reply);
            if (len > 0) {
                const auto* data = static_cast<const unsigned char*>(x.get_property_value(reply));
                if (out.size() + static_cast<size_t>(len) > ClipboardLimits::kMaxDecompressedClipboardBytes) {
                    std::free(reply);
                    detail = "clipboard content exceeds the 64 MB limit";
                    return false;
                }
                out.insert(out.end(), data, data + len);
            }
            const uint32_t bytesAfter = reply->bytes_after;
            std::free(reply);
            if (bytesAfter == 0) {
                return true;
            }
            // Partial reads return whole 32-bit words (the offset unit) as long
            // as bytes_after > 0, so this division is exact.
            offsetWords += static_cast<uint32_t>(len) / 4;
        }
    }

    // Requests the selection converted to `target` and collects the result,
    // running the INCR protocol when the owner chooses it. Returns false with
    // empty `out` for "owner can't provide this target" (SelectionNotify with
    // property None) and false with `detail` set for real failures.
    bool Convert(xcb_atom_t target, std::vector<unsigned char>& out, std::string& detail) {
        out.clear();
        x.convert_selection(conn, window, atomClipboard, target, atomDest, XCB_CURRENT_TIME);
        x.flush(conn);

        xcb_generic_event_t* ev = WaitForEvent(XCB_SELECTION_NOTIFY, [&](xcb_generic_event_t* e) {
            const auto* sn = reinterpret_cast<xcb_selection_notify_event_t*>(e);
            return sn->requestor == window && sn->selection == atomClipboard;
        });
        if (ev == nullptr) {
            detail = "timed out waiting for the clipboard owner";
            return false;
        }
        const xcb_atom_t replyProperty = reinterpret_cast<xcb_selection_notify_event_t*>(ev)->property;
        std::free(ev);
        if (replyProperty == XCB_ATOM_NONE) {
            return false;  // owner refuses this target; not an error
        }

        xcb_atom_t type = XCB_ATOM_NONE;
        if (!FetchProperty(out, type, detail)) {
            return false;
        }
        if (type != atomIncr) {
            return true;
        }

        // INCR: the initial read (whose value is only a size hint) is discarded;
        // deleting it told the owner to start streaming. Each PropertyNotify
        // NewValue carries a chunk; a zero-length chunk terminates.
        out.clear();
        for (;;) {
            ev = WaitForEvent(XCB_PROPERTY_NOTIFY, [&](xcb_generic_event_t* e) {
                const auto* pn = reinterpret_cast<xcb_property_notify_event_t*>(e);
                return pn->window == window && pn->atom == atomDest && pn->state == XCB_PROPERTY_NEW_VALUE;
            });
            if (ev == nullptr) {
                detail = "clipboard owner stalled mid-transfer (INCR)";
                return false;
            }
            std::free(ev);
            std::vector<unsigned char> chunk;
            if (!FetchProperty(chunk, type, detail)) {
                return false;
            }
            if (chunk.empty()) {
                return true;
            }
            if (out.size() + chunk.size() > ClipboardLimits::kMaxDecompressedClipboardBytes) {
                detail = "clipboard content exceeds the 64 MB limit";
                return false;
            }
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
    }

    // TARGETS answer -> the best target we can use, or NONE.
    xcb_atom_t PickTarget(const std::vector<unsigned char>& targetBytes) const {
        const auto* atoms = reinterpret_cast<const xcb_atom_t*>(targetBytes.data());
        const size_t count = targetBytes.size() / sizeof(xcb_atom_t);
        bool utf8 = false;
        bool latin1 = false;
        bool png = false;
        for (size_t i = 0; i < count; ++i) {
            if (atoms[i] == atomPng && atomPng != XCB_ATOM_NONE) {
                png = true;
            } else if (atoms[i] == atomUtf8) {
                utf8 = true;
            } else if (atoms[i] == XCB_ATOM_STRING) {
                latin1 = true;
            }
        }
        if (png) return atomPng;
        if (utf8) return atomUtf8;
        if (latin1) return XCB_ATOM_STRING;
        return XCB_ATOM_NONE;
    }
};

void Latin1ToUtf8(std::vector<unsigned char>& bytes) {
    std::vector<unsigned char> utf8;
    utf8.reserve(bytes.size());
    for (const unsigned char c : bytes) {
        if (c < 0x80) {
            utf8.push_back(c);
        } else {
            utf8.push_back(static_cast<unsigned char>(0xC0 | (c >> 6)));
            utf8.push_back(static_cast<unsigned char>(0x80 | (c & 0x3F)));
        }
    }
    bytes = std::move(utf8);
}

}  // namespace

DesktopClipboardStatus ReadDesktopClipboard(DesktopClipboardData& out, std::string& detail) {
    const char* display = std::getenv("DISPLAY");
    if (display == nullptr || *display == '\0') {
        const char* wayland = std::getenv("WAYLAND_DISPLAY");
        detail = (wayland != nullptr && *wayland != '\0')
            ? "DISPLAY is not set (Wayland session without XWayland?)"
            : "DISPLAY is not set";
        return DesktopClipboardStatus::NoSession;
    }

    static XcbApi api;
    static const bool loaded = api.Load();
    if (!loaded) {
        detail = "libxcb.so.1 is not available";
        return DesktopClipboardStatus::NoSession;
    }

    Reader reader(api);
    if (!reader.Setup(detail)) {
        return DesktopClipboardStatus::NoSession;
    }
    if (!reader.HasOwner()) {
        return DesktopClipboardStatus::Empty;
    }

    // Negotiate: prefer PNG over text (matches the SendTo sniffing order). An
    // owner that won't answer TARGETS (rare, ancient) gets a direct UTF8 try.
    xcb_atom_t target = reader.atomUtf8;
    std::vector<unsigned char> targetBytes;
    std::string targetsDetail;
    if (reader.Convert(reader.atomTargets, targetBytes, targetsDetail) && !targetBytes.empty()) {
        target = reader.PickTarget(targetBytes);
        if (target == XCB_ATOM_NONE) {
            return DesktopClipboardStatus::Unsupported;
        }
    }

    std::vector<unsigned char> bytes;
    if (!reader.Convert(target, bytes, detail)) {
        if (detail.empty()) {
            // Owner advertised the target but then refused it.
            return DesktopClipboardStatus::Empty;
        }
        return DesktopClipboardStatus::Error;
    }
    if (bytes.empty()) {
        return DesktopClipboardStatus::Empty;
    }

    if (target == reader.atomPng) {
        out.formatId = CLIPP_FORMAT_PNG;
    } else {
        if (target == XCB_ATOM_STRING) {
            Latin1ToUtf8(bytes);
        }
        // X11 text is LF-native; a trailing NUL is the caller's convention.
        out.formatId = CLIPP_FORMAT_UTF8;
    }
    out.bytes = std::move(bytes);
    return DesktopClipboardStatus::Ok;
}

}  // namespace clipp

#endif  // __linux__
