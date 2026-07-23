#include "platform.h"

#include "PopupWindow.h"

#include "ClipboardActions.h"
#include "ClipboardActivityStore.h"
#include "Logger.h"
#include "PopupModel.h"
#include "clipp-win32-darkmode32/DMSubclass.h"
#include "platform/uiClippPage.h"
#include "platform/uistrings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <dwmapi.h>
#include <unknwn.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#pragma comment(lib, "dwmapi.lib")

extern ClipboardActivityStore g_clipboardActivityStore;
extern Logger g_logger;

namespace {

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

constexpr wchar_t kPopupClassName[] = L"ClippPopupWindow";
constexpr double kPopupWidthDips = 420;
constexpr double kPopupHeightDips = 540;
// XAML row construction is the expensive part of a re-render; cap what one
// filter state shows and say so with a hint row instead of silently cropping.
constexpr std::size_t kMaxRenderedRows = 40;

int DipsToPixels(double dips, UINT dpi) {
    return static_cast<int>(std::ceil(dips * dpi / USER_DEFAULT_SCREEN_DPI));
}

ElementTheme CurrentTheme() {
    return DarkMode::isEnabled() ? ElementTheme::Dark : ElementTheme::Light;
}

SolidColorBrush ArgbBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b));
}

Brush PopupBackgroundBrush() {
    // Solid (not theme-resource) so the borderless window reads as one crisp
    // surface; tones match the settings window's chrome.
    return DarkMode::isEnabled() ? ArgbBrush(255, 32, 32, 32) : ArgbBrush(255, 243, 243, 243);
}

class PopupWindow {
public:
    void Toggle() {
        if (hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
            Dismiss(/*restoreFocus=*/true);
        } else {
            Summon();
        }
    }

    bool PreTranslateMessage(MSG* msg) {
        if (hwnd_ == nullptr || !IsWindowVisible(hwnd_) || !xamlSource_) {
            return false;
        }
        auto native2 = xamlSource_.try_as<IDesktopWindowXamlSourceNative2>();
        if (!native2) {
            return false;
        }
        BOOL translated = FALSE;
        if (FAILED(native2->PreTranslateMessage(msg, &translated))) {
            return false;
        }
        return translated == TRUE;
    }

    void Destroy() {
        EndActivityNotifications();
        if (xamlSource_) {
            xamlSource_.Close();
            xamlSource_ = nullptr;
        }
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

private:
    // ---- lifecycle ----

    void Summon() {
        if (hwnd_ == nullptr) {
            Create();
            if (hwnd_ == nullptr) {
                return;
            }
        }

        previousForeground_ = GetForegroundWindow();

        RebuildFromStores();
        if (filterBox_) {
            filterBox_.Text(L"");  // fresh session; fires TextChanged -> SetFilter("")
        }
        RenderList();

        PositionOnCursorMonitor();
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (filterBox_) {
            filterBox_.Focus(FocusState::Programmatic);
        }
        BeginActivityNotifications();
    }

    void Dismiss(bool restoreFocus) {
        if (hwnd_ == nullptr || !IsWindowVisible(hwnd_)) {
            return;
        }
        EndActivityNotifications();
        // Session-scoped peeks: anything revealed inside the popup is
        // forgotten the moment it hides.
        uiClippPage::ForgetAllPeekedItems();
        ShowWindow(hwnd_, SW_HIDE);
        if (restoreFocus && previousForeground_ != nullptr && IsWindow(previousForeground_)) {
            SetForegroundWindow(previousForeground_);
        }
        previousForeground_ = nullptr;
    }

    void Create() {
        RegisterPopupClass();
        const HINSTANCE hInstance = GetModuleHandleW(nullptr);
        CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kPopupClassName,
            L"Clipp",
            WS_POPUP,
            0, 0,
            DipsToPixels(kPopupWidthDips, USER_DEFAULT_SCREEN_DPI),
            DipsToPixels(kPopupHeightDips, USER_DEFAULT_SCREEN_DPI),
            nullptr, nullptr, hInstance, this);
        if (hwnd_ == nullptr) {
            g_logger.log(__FUNCTION__, Logger::Level::Error, L"Popup window creation failed.");
            return;
        }

        BOOL dark = DarkMode::isEnabled() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        const DWORD cornerRound = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerRound, sizeof(cornerRound));

        InitializeXamlIsland();
    }

    void InitializeXamlIsland() {
        // The tray thread hosts the settings window's island too; the manager
        // and apartment are idempotent per thread.
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (...) {
            // Already initialized on this thread — fine.
        }
        if (!xamlManager_) {
            xamlManager_ = Hosting::WindowsXamlManager::InitializeForCurrentThread();
        }
        xamlSource_ = Hosting::DesktopWindowXamlSource{};
        auto nativeSource = xamlSource_.as<IDesktopWindowXamlSourceNative>();
        winrt::check_hresult(nativeSource->AttachToWindow(hwnd_));
        winrt::check_hresult(nativeSource->get_WindowHandle(&xamlHost_));
        xamlSource_.Content(BuildContent());
        ResizeXamlHost();
    }

    // ---- content ----

    Grid BuildContent() {
        Grid root;
        root.RequestedTheme(CurrentTheme());
        root.Background(PopupBackgroundBrush());

        RowDefinition filterRow;
        filterRow.Height(GridLength{ 0, GridUnitType::Auto });
        RowDefinition listRow;
        listRow.Height(GridLength{ 1, GridUnitType::Star });
        root.RowDefinitions().Append(filterRow);
        root.RowDefinitions().Append(listRow);

        filterBox_ = TextBox();
        filterBox_.PlaceholderText(winrt::hstring{ CLP_W(CLP_UI_POPUP_FILTER_HINT) });
        filterBox_.Margin(ThicknessHelper::FromLengths(12, 12, 12, 8));
        filterBox_.TextChanged([this](auto const&, auto const&) {
            model_.SetFilter(std::wstring{ filterBox_.Text() });
            RenderList();
        });
        // PreviewKeyDown so navigation wins over the TextBox's own key
        // handling; the box keeps keyboard focus for the popup's whole life
        // (the launcher pattern) and the list is driven from here.
        filterBox_.PreviewKeyDown([this](auto const&, Input::KeyRoutedEventArgs const& args) {
            OnFilterKey(args);
        });
        Grid::SetRow(filterBox_, 0);
        root.Children().Append(filterBox_);

        listScroll_ = ScrollViewer();
        listScroll_.Margin(ThicknessHelper::FromLengths(8, 0, 8, 8));
        listPanel_ = StackPanel();
        listPanel_.Spacing(2);
        listScroll_.Content(listPanel_);
        Grid::SetRow(listScroll_, 1);
        root.Children().Append(listScroll_);

        return root;
    }

    void OnFilterKey(Input::KeyRoutedEventArgs const& args) {
        using winrt::Windows::System::VirtualKey;
        const auto key = args.Key();
        const bool filterEmpty = filterBox_ ? filterBox_.Text().empty() : true;

        switch (key) {
        case VirtualKey::Down:
            model_.MoveDown();
            RenderHighlight();
            args.Handled(true);
            return;
        case VirtualKey::Up:
            model_.MoveUp();
            RenderHighlight();
            args.Handled(true);
            return;
        case VirtualKey::Left:
        case VirtualKey::Right:
            // Group hops — but only when the filter box has no text for the
            // caret to move through.
            if (filterEmpty) {
                if (key == VirtualKey::Left) model_.MoveLeft(); else model_.MoveRight();
                RenderHighlight();
                args.Handled(true);
            }
            return;
        case VirtualKey::Enter:
            ActivateSelected();
            args.Handled(true);
            return;
        case VirtualKey::Delete:
            // With filter text present, Delete edits the text; on an empty
            // filter it deletes the selected item everywhere.
            if (filterEmpty) {
                DeleteSelected();
                args.Handled(true);
            }
            return;
        case VirtualKey::Escape: {
            const auto result = model_.HandleEscape();
            if (result == PopupModel::EscapeResult::ClearedFilter) {
                if (filterBox_) {
                    filterBox_.Text(L"");  // TextChanged re-syncs the (already clear) model
                }
            } else if (result == PopupModel::EscapeResult::Close) {
                Dismiss(/*restoreFocus=*/true);
            }
            args.Handled(true);
            return;
        }
        case VirtualKey::Tab:
            args.Handled(true);  // focus stays in the filter box
            return;
        default:
            return;
        }
    }

    // ---- data ----

    void RebuildFromStores() {
        displayCache_.clear();
        std::vector<PopupItem> history;
        const auto snapshot = g_clipboardActivityStore.Snapshot();  // ascending by ts
        history.reserve(snapshot.size());
        for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
            auto display = g_clipboardActivityStore.DisplayItem(it->id);
            if (!display) {
                continue;
            }
            PopupItem item;
            item.kind = PopupItem::Kind::History;
            item.historyId = it->id;
            // Filter matches the MASKED preview (never the revealed text of a
            // private row) plus the device name.
            item.searchText = display->previewText + L" " + display->deviceName;
            item.actionable = display->kind != ClipboardActivityPayloadKind::PrivatePlaceholder;
            displayCache_.emplace(it->id, std::move(*display));
            history.push_back(std::move(item));
        }
        // Registers group lands with the promote flow (plan B4); until then the
        // model runs history-only and the group column collapses naturally.
        model_.SetItems({}, std::move(history));
    }

    // ---- rendering ----

    void RenderList() {
        if (!listPanel_) {
            return;
        }
        listPanel_.Children().Clear();
        rowBorders_.clear();

        const auto& history = model_.VisibleHistory();
        const std::size_t shown = (std::min)(history.size(), kMaxRenderedRows);
        for (std::size_t i = 0; i < shown; ++i) {
            listPanel_.Children().Append(BuildRow(*history[i], i));
        }
        if (history.size() > shown) {
            TextBlock more;
            more.Text(winrt::hstring{ CLP_W(CLP_UI_POPUP_MORE) });
            more.Opacity(0.55);
            more.FontSize(12);
            more.Margin(ThicknessHelper::FromLengths(10, 6, 10, 6));
            listPanel_.Children().Append(more);
        }
        if (history.empty()) {
            TextBlock empty;
            empty.Text(winrt::hstring{ CLP_W(CLP_UI_CLIPBOARD_EMPTY) });
            empty.Opacity(0.55);
            empty.Margin(ThicknessHelper::FromLengths(10, 16, 10, 6));
            empty.HorizontalAlignment(HorizontalAlignment::Center);
            listPanel_.Children().Append(empty);
        }
        RenderHighlight();
    }

    Border BuildRow(const PopupItem& item, std::size_t index) {
        const auto cached = displayCache_.find(item.historyId);

        StackPanel content;
        content.Spacing(1);

        TextBlock preview;
        std::wstring previewText;
        std::wstring metaText;
        if (cached != displayCache_.end()) {
            const auto& display = cached->second;
            switch (display.kind) {
            case ClipboardActivityPayloadKind::Image:
                previewText = CLP_W(CLP_UI_IMAGE);
                break;
            case ClipboardActivityPayloadKind::PrivatePlaceholder:
                previewText = CLP_W(CLP_UI_PRIVATE_PLACEHOLDER_TITLE);
                break;
            default:
                previewText = display.previewText;
                break;
            }
            metaText = display.deviceName;
        }
        if (previewText.empty()) {
            previewText = L" ";
        }
        preview.Text(winrt::hstring{ previewText });
        preview.TextTrimming(TextTrimming::CharacterEllipsis);
        preview.TextWrapping(TextWrapping::NoWrap);
        preview.MaxLines(1);
        content.Children().Append(preview);

        if (!metaText.empty()) {
            TextBlock meta;
            meta.Text(winrt::hstring{ metaText });
            meta.FontSize(11);
            meta.Opacity(0.6);
            content.Children().Append(meta);
        }

        Border row;
        row.Padding(ThicknessHelper::FromLengths(10, 6, 10, 6));
        row.CornerRadius(CornerRadius{ 6 });
        row.Background(ArgbBrush(0, 0, 0, 0));
        row.Child(content);

        row.PointerPressed([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            RenderHighlight();
        });
        row.DoubleTapped([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            ActivateSelected();
        });

        MenuFlyout menu;
        MenuFlyoutItem copyItem;
        copyItem.Text(winrt::hstring{ CLP_W(CLP_UI_COPY) });
        copyItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            ActivateSelected();
        });
        menu.Items().Append(copyItem);
        MenuFlyoutItem deleteItem;
        deleteItem.Text(winrt::hstring{ CLP_W(CLP_UI_DELETE) });
        deleteItem.Click([this, index](auto const&, auto const&) {
            model_.SelectAt(PopupModel::Group::History, index);
            DeleteSelected();
        });
        menu.Items().Append(deleteItem);
        row.ContextFlyout(menu);

        rowBorders_.push_back(row);
        return row;
    }

    void RenderHighlight() {
        const auto selection = model_.Selected();
        for (std::size_t i = 0; i < rowBorders_.size(); ++i) {
            const bool selected = selection.has_value()
                && selection->group == PopupModel::Group::History
                && selection->index == i;
            rowBorders_[i].Background(selected ? ArgbBrush(56, 127, 127, 127)
                                               : ArgbBrush(0, 0, 0, 0));
            if (selected) {
                rowBorders_[i].StartBringIntoView();
            }
        }
    }

    // ---- actions ----

    void ActivateSelected() {
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr || !item->actionable) {
            return;
        }
        if (item->kind == PopupItem::Kind::History) {
            clipp::ReshareActivityItem(item->historyId);
        }
        Dismiss(/*restoreFocus=*/true);
    }

    void DeleteSelected() {
        const PopupItem* item = model_.SelectedItem();
        if (item == nullptr) {
            return;
        }
        if (item->kind == PopupItem::Kind::History) {
            clipp::DeleteActivityItemEverywhere(item->historyId);
            // The watcher event rebuilds the list.
        }
    }

    // ---- store watcher (visible only) ----

    void BeginActivityNotifications() {
        if (watcherID_ != 0) {
            return;
        }
        if (!dispatcher_) {
            dispatcher_ = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
        }
        const auto registration = g_clipboardActivityStore.QueryAndRegister(&PopupWindow::ActivityWatcher, this);
        watcherID_ = registration.watcherID;
    }

    void EndActivityNotifications() {
        if (watcherID_ == 0) {
            return;
        }
        g_clipboardActivityStore.Unregister(watcherID_);
        watcherID_ = 0;
    }

    static void ActivityWatcher(const ClipboardActivityUpdate&, void* userData) {
        auto* self = static_cast<PopupWindow*>(userData);
        if (self == nullptr || !self->dispatcher_) {
            return;
        }
        // Coarse but correct: any store change re-snapshots while visible.
        self->dispatcher_.TryEnqueue([self]() {
            if (self->hwnd_ != nullptr && IsWindowVisible(self->hwnd_)) {
                self->RebuildFromStores();
                self->RenderList();
            }
        });
    }

    // ---- window plumbing ----

    void PositionOnCursorMonitor() {
        POINT cursor{};
        GetCursorPos(&cursor);
        const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return;
        }
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int width = DipsToPixels(kPopupWidthDips, dpi);
        const int height = DipsToPixels(kPopupHeightDips, dpi);
        const RECT& work = info.rcWork;
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
        ResizeXamlHost();
    }

    void ResizeXamlHost() {
        if (xamlHost_ == nullptr || hwnd_ == nullptr) {
            return;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        SetWindowPos(xamlHost_, nullptr, 0, 0,
            client.right - client.left, client.bottom - client.top,
            SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                // Light dismiss — unless focus went to a window of our own
                // thread (the island's flyout popups live in sibling HWNDs).
                const HWND other = reinterpret_cast<HWND>(lParam);
                const DWORD ourThread = GetCurrentThreadId();
                if (other == nullptr ||
                    GetWindowThreadProcessId(other, nullptr) != ourThread) {
                    Dismiss(/*restoreFocus=*/false);
                }
            }
            return 0;
        case WM_SIZE:
            ResizeXamlHost();
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            Dismiss(/*restoreFocus=*/false);
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }
    }

    static void RegisterPopupClass() {
        static bool registered = false;
        if (registered) {
            return;
        }
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPopupClassName;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PopupWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<PopupWindow*>(createStruct->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self == nullptr) {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        const LRESULT result = self->HandleMessage(msg, wParam, lParam);
        if (msg == WM_NCDESTROY) {
            self->hwnd_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return result;
    }

    HWND hwnd_ = nullptr;
    HWND xamlHost_ = nullptr;
    HWND previousForeground_ = nullptr;
    Hosting::WindowsXamlManager xamlManager_{ nullptr };
    Hosting::DesktopWindowXamlSource xamlSource_{ nullptr };
    winrt::Windows::System::DispatcherQueue dispatcher_{ nullptr };
    TextBox filterBox_{ nullptr };
    ScrollViewer listScroll_{ nullptr };
    StackPanel listPanel_{ nullptr };
    std::vector<Border> rowBorders_;
    std::unordered_map<uint64_t, ClipboardActivityDisplayItem> displayCache_;
    PopupModel model_;
    std::size_t watcherID_ = 0;
};

std::unique_ptr<PopupWindow> g_popupWindow;

}  // namespace

namespace clipp {

void TogglePopupWindow() {
    if (!g_popupWindow) {
        g_popupWindow = std::make_unique<PopupWindow>();
    }
    g_popupWindow->Toggle();
}

bool PopupPreTranslateMessage(MSG* msg) {
    return g_popupWindow ? g_popupWindow->PreTranslateMessage(msg) : false;
}

void DestroyPopupWindow() {
    if (g_popupWindow) {
        g_popupWindow->Destroy();
        g_popupWindow.reset();
    }
}

}  // namespace clipp
