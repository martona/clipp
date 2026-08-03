#include "TextTyper.h"

#include "Logger.h"
#include "platform.h"
#include "platform/uistrings.h"

#include <Windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <string>
#include <vector>

namespace clipp {

namespace {

constexpr wchar_t kTyperWindowClassName[] = L"ClippTextTyperHost";
constexpr wchar_t kProgressClassName[] = L"ClippTypingProgress";
constexpr UINT_PTR kTypeTimerId = 1;

bool IsExtendedVirtualKey(TypeKeyCode key) {
    // Extended-key flag matters for the right-hand modifiers: without it,
    // right Alt is indistinguishable from left Alt and AltGr never resolves.
    switch (key) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
        return true;
    default:
        return false;
    }
}

void SendKeyEvent(TypeKeyCode key, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    // Populate the hardware scancode as well as the virtual key. Consumers
    // that re-derive a scancode for their own wire protocol (KVM/SPICE/VNC
    // viewers, which is the whole point of this feature) read it from the
    // message's lParam, and a zero there is exactly what makes synthesized
    // unicode input useless to them.
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP)
        | (IsExtendedVirtualKey(key) ? KEYEVENTF_EXTENDEDKEY : 0u);
    SendInput(1, &input, sizeof(INPUT));
}

// Any modifier the user is still physically holding would corrupt every
// keystroke we send (a held Shift turns the whole run into capitals), so lift
// them before starting. Same scrub the popup's synthetic Ctrl+V does.
void ReleaseHeldModifiers() {
    const TypeKeyCode modifiers[] = {
        VK_LWIN, VK_RWIN, VK_MENU, VK_LMENU, VK_RMENU,
        VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
    };
    for (const TypeKeyCode key : modifiers) {
        if (GetAsyncKeyState(key) & 0x8000) {
            SendKeyEvent(key, false);
        }
    }
}

// ---- progress toast --------------------------------------------------------
// A GDI pill parked above the tray, topmost and never activating. Plain GDI
// for the same reason the popup's coaching toast is: nothing to fight, no
// island to spin up, and it can appear while another app owns the foreground.
class ProgressWindow {
public:
    void Show(const std::wstring& text) {
        text_ = text;
        EnsureCreated();
        if (hwnd_ == nullptr) {
            return;
        }
        Reposition();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void Update(const std::wstring& text) {
        if (hwnd_ == nullptr || text == text_) {
            return;
        }
        text_ = text;
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
    }

    void Hide() {
        if (hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    void Destroy() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (font_ != nullptr) {
            DeleteObject(font_);
            font_ = nullptr;
        }
    }

private:
    void EnsureCreated() {
        if (hwnd_ != nullptr) {
            return;
        }
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kProgressClassName;
            RegisterClassExW(&wc);
            registered = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kProgressClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (hwnd_ != nullptr) {
            const DWORD corner = 2 /*DWMWCP_ROUND*/;
            DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                &corner, sizeof(corner));
        }
    }

    void EnsureFont(UINT dpi) {
        if (font_ != nullptr && fontDpi_ == dpi) {
            return;
        }
        if (font_ != nullptr) {
            DeleteObject(font_);
        }
        font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        fontDpi_ = dpi;
    }

    // Bottom-right of the work area on the monitor that owns the tray: beside
    // the notification area, clear of the taskbar, out of the way of whatever
    // is being typed into.
    void Reposition() {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        HMONITOR monitor = tray != nullptr
            ? MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY)
            : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return;
        }
        const UINT dpi = GetDpiForWindow(hwnd_);
        EnsureFont(dpi);

        RECT measure{};
        if (const HDC hdc = GetDC(hwnd_)) {
            const HGDIOBJ previous = SelectObject(hdc, font_);
            DrawTextW(hdc, text_.c_str(), -1, &measure, DT_CALCRECT | DT_NOPREFIX);
            SelectObject(hdc, previous);
            ReleaseDC(hwnd_, hdc);
        }
        const int padX = MulDiv(14, static_cast<int>(dpi), 96);
        const int padY = MulDiv(9, static_cast<int>(dpi), 96);
        const int margin = MulDiv(12, static_cast<int>(dpi), 96);
        width_ = (measure.right - measure.left) + padX * 2;
        height_ = (measure.bottom - measure.top) + padY * 2;
        const int x = info.rcWork.right - width_ - margin;
        const int y = info.rcWork.bottom - height_ - margin;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void Paint() {
        PAINTSTRUCT ps{};
        const HDC hdc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const HBRUSH background = CreateSolidBrush(RGB(38, 35, 32));
        FillRect(hdc, &client, background);
        DeleteObject(background);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(245, 240, 235));
        const HGDIOBJ previous = SelectObject(hdc, font_);
        DrawTextW(hdc, text_.c_str(), -1, &client,
            DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_SINGLELINE);
        SelectObject(hdc, previous);
        EndPaint(hwnd_, &ps);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ProgressWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ProgressWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self != nullptr && msg == WM_PAINT) {
            self->Paint();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    UINT fontDpi_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::wstring text_;
};

// ---- the run ---------------------------------------------------------------
class TypingSession {
public:
    static TypingSession& Instance() {
        static TypingSession session;
        return session;
    }

    bool Active() const { return active_; }

    bool Start(TypeSchedule schedule) {
        if (active_ || schedule.events.empty()) {
            return false;
        }
        EnsureHost();
        if (host_ == nullptr) {
            return false;
        }
        schedule_ = std::move(schedule);
        next_ = 0;
        pressed_.clear();
        active_ = true;

        ReleaseHeldModifiers();
        InstallHooks();
        progress_.Show(ProgressText());
        SetTimer(host_, kTypeTimerId, kTypeEventIntervalMs, nullptr);
        g_logger.log(__FUNCTION__, Logger::Level::Info,
            L"Typing %d character(s) as %zu key event(s).",
            schedule_.characterCount, schedule_.events.size());
        return true;
    }

    void Cancel() {
        if (!active_) {
            return;
        }
        Stop(/*completed=*/false);
    }

    void Tick() {
        if (!active_) {
            return;
        }
        const TypeKeyEvent& event = schedule_.events[next_++];
        SendKeyEvent(event.key, event.down);
        // Track in-flight presses so an abort can release exactly what's down —
        // a run stopped mid-chord must never leave a modifier stuck, which on a
        // real OS input queue would wedge every other app on the machine.
        if (event.down) {
            pressed_.push_back(event.key);
        } else {
            const auto it = std::find(pressed_.rbegin(), pressed_.rend(), event.key);
            if (it != pressed_.rend()) {
                pressed_.erase(std::next(it).base());
            }
        }

        if (next_ >= schedule_.events.size()) {
            Stop(/*completed=*/true);
            return;
        }
        progress_.Update(ProgressText());
    }

    void Shutdown() {
        Cancel();
        progress_.Destroy();
        if (host_ != nullptr) {
            DestroyWindow(host_);
            host_ = nullptr;
        }
    }

private:
    void EnsureHost() {
        if (host_ != nullptr) {
            return;
        }
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = HostWndProc;
            wc.hInstance = instance;
            wc.lpszClassName = kTyperWindowClassName;
            RegisterClassExW(&wc);
            registered = true;
        }
        // Message-only: it exists solely to own the pacing timer.
        host_ = CreateWindowExW(0, kTyperWindowClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, instance, nullptr);
    }

    std::wstring ProgressText() const {
        const std::size_t remaining = schedule_.events.size() - next_;
        const int seconds = static_cast<int>(
            (remaining * kTypeEventIntervalMs + 999) / 1000);
        std::wstring text = CLP_W(CLP_UI_TYPING_PROGRESS_PREFIX);
        text += std::to_wstring(remaining);
        text += CLP_W(CLP_UI_TYPING_PROGRESS_MIDDLE);
        text += std::to_wstring(seconds);
        text += CLP_W(CLP_UI_TYPING_PROGRESS_SUFFIX);
        return text;
    }

    void Stop(bool completed) {
        KillTimer(host_, kTypeTimerId);
        RemoveHooks();
        // Release whatever is still down, newest first. A normal completion has
        // an empty stack (the schedule ends with every modifier released).
        for (auto it = pressed_.rbegin(); it != pressed_.rend(); ++it) {
            SendKeyEvent(*it, false);
        }
        pressed_.clear();
        active_ = false;
        progress_.Hide();
        const std::size_t done = next_;
        const std::size_t total = schedule_.events.size();
        schedule_ = TypeSchedule{};
        next_ = 0;
        g_logger.log(__FUNCTION__, Logger::Level::Info,
            completed ? L"Typing finished (%zu/%zu events)."
                      : L"Typing canceled (%zu/%zu events).",
            done, total);
    }

    void InstallHooks() {
        // WH_*_LL hooks require a message pump on the installing thread; this
        // all runs on the tray UI thread, which has one.
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc,
            GetModuleHandleW(nullptr), 0);
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MouseProc,
            GetModuleHandleW(nullptr), 0);
    }

    void RemoveHooks() {
        if (keyboardHook_ != nullptr) {
            UnhookWindowsHookEx(keyboardHook_);
            keyboardHook_ = nullptr;
        }
        if (mouseHook_ != nullptr) {
            UnhookWindowsHookEx(mouseHook_);
            mouseHook_ = nullptr;
        }
    }

    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_TIMER && wParam == kTypeTimerId) {
            Instance().Tick();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // Any PHYSICAL key press stops the run. Our own SendInput events carry
    // LLKHF_INJECTED and are ignored. The aborting keystroke is deliberately
    // NOT swallowed: the user pressed it meaning to interact with the window
    // they are looking at, and eating it (or eating its down but not its up)
    // is how injected input leaves apps with stuck keys.
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
            if ((event->flags & LLKHF_INJECTED) == 0) {
                Instance().Cancel();
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Same for a physical mouse CLICK (any button). Movement and wheel are
    // ignored — nudging the mouse must not kill a long run.
    static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HC_ACTION) {
            const bool isButtonDown = wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN
                || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN
                || wParam == WM_NCLBUTTONDOWN || wParam == WM_NCRBUTTONDOWN;
            if (isButtonDown) {
                const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
                if ((event->flags & LLMHF_INJECTED) == 0) {
                    Instance().Cancel();
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    HWND host_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    ProgressWindow progress_;
    TypeSchedule schedule_;
    std::size_t next_ = 0;
    std::vector<TypeKeyCode> pressed_;
    bool active_ = false;
};

}  // namespace

bool StartTyping(TypeSchedule schedule) {
    return TypingSession::Instance().Start(std::move(schedule));
}

bool IsTyping() {
    return TypingSession::Instance().Active();
}

void CancelTyping() {
    TypingSession::Instance().Cancel();
}

void ShutdownTyping() {
    TypingSession::Instance().Shutdown();
}

}  // namespace clipp
