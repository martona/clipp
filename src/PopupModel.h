#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Platform-free state machine for the visual-paste popup: the two item groups
// (Registers | History), the single roaming highlight, the type-anywhere
// substring filter, and the browse-vs-inline-edit input mode. Pure data in,
// pure state out — no store globals, no rendering, no actions. The platform
// shells feed it snapshots (SetItems), route keys through it, render its
// visible projections, and perform the actual actions (make-current, delete,
// promote, rename) themselves when it tells them what's selected.
//
// Layout contract the navigation encodes: Registers group on the LEFT,
// History on the RIGHT. Up/Down move within the focused group (clamped, no
// wrap); Left/Right hop across groups keeping the closest row.

struct PopupItem {
    enum class Kind {
        Register,
        History,
    };

    Kind kind{ Kind::History };
    // History: the ClipboardActivityStore item id. Registers: the name.
    uint64_t historyId{ 0 };
    std::string registerName;
    // What the filter matches against (preview/name/detail — shell's choice),
    // matched case-insensitively for ASCII.
    std::wstring searchText;
    // False for rows that can't be made current (private placeholders). They
    // are still shown, selectable, and deletable.
    bool actionable{ true };
};

class PopupModel {
public:
    enum class Group {
        Registers,
        History,
    };

    enum class Mode {
        Browse,  // keys navigate/filter
        Edit,    // an inline name editor owns the keyboard
    };

    // What HandleEscape consumed, so the shell knows whether to close.
    enum class EscapeResult {
        LeftEditMode,
        ClearedFilter,
        Close,
    };

    struct Selection {
        Group group{ Group::History };
        std::size_t index{ 0 };  // into the VISIBLE projection of that group
    };

    // Rebuild from fresh snapshots (popup open, store watcher event). Keeps
    // the filter; re-filters; re-clamps the selection (same group if it still
    // has rows, else the other, else none).
    void SetItems(std::vector<PopupItem> registers, std::vector<PopupItem> history);

    // --- filter (Browse mode; the shell routes printable keys here) ---
    void SetFilter(const std::wstring& text);
    void AppendToFilter(wchar_t ch);
    // True if a character was removed (false = filter already empty).
    bool BackspaceFilter();
    const std::wstring& Filter() const { return filter_; }

    // --- visible projections (filtered, in feed order) ---
    const std::vector<const PopupItem*>& VisibleRegisters() const { return visibleRegisters_; }
    const std::vector<const PopupItem*>& VisibleHistory() const { return visibleHistory_; }
    bool Empty() const { return visibleRegisters_.empty() && visibleHistory_.empty(); }

    // --- selection ---
    std::optional<Selection> Selected() const { return selection_; }
    const PopupItem* SelectedItem() const;
    void MoveUp();
    void MoveDown();
    void MoveLeft();   // towards Registers
    void MoveRight();  // towards History
    // Mouse hover/click. Ignored for an out-of-range index.
    void SelectAt(Group group, std::size_t index);

    // --- mode ---
    Mode CurrentMode() const { return mode_; }
    // Enter the inline-editor mode (rename / post-promote naming). The shell
    // owns the editor widget; the model only routes keys accordingly.
    void EnterEditMode() { mode_ = Mode::Edit; }
    void LeaveEditMode() { mode_ = Mode::Browse; }

    // Esc, in priority order: leave edit mode -> clear a non-empty filter ->
    // close the popup.
    EscapeResult HandleEscape();

private:
    void Refilter();
    void ClampSelection(std::optional<Group> preferredGroup);
    const std::vector<const PopupItem*>& GroupItems(Group group) const;

    std::vector<PopupItem> registers_;
    std::vector<PopupItem> history_;
    std::vector<const PopupItem*> visibleRegisters_;
    std::vector<const PopupItem*> visibleHistory_;
    std::wstring filter_;
    std::optional<Selection> selection_;
    Mode mode_{ Mode::Browse };
};

// First unused "item-N" (N >= 1) given the currently-live register names.
// Deliberately lowercase and inside the OLD narrow charset, so machine-created
// registers stay addressable and holdable by old daemons/CLIs during the
// wide-name transition.
std::string NextAutoRegisterName(const std::vector<std::string>& existingNames);
