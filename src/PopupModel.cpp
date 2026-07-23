#include "PopupModel.h"

#include <algorithm>
#include <cwctype>

namespace {

// ASCII-case-insensitive substring. Non-ASCII compares byte-exact — good
// enough for a live filter, and free of locale surprises.
bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto fold = [](wchar_t ch) -> wchar_t {
        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool match = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            if (fold(haystack[start + i]) != fold(needle[i])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

}  // namespace

void PopupModel::SetItems(std::vector<PopupItem> registers, std::vector<PopupItem> history) {
    const std::optional<Group> keepGroup =
        selection_.has_value() ? std::optional<Group>(selection_->group) : std::nullopt;
    registers_ = std::move(registers);
    history_ = std::move(history);
    Refilter();
    ClampSelection(keepGroup);
}

void PopupModel::SetFilter(const std::wstring& text) {
    filter_ = text;
    const std::optional<Group> keepGroup =
        selection_.has_value() ? std::optional<Group>(selection_->group) : std::nullopt;
    Refilter();
    // A filter change re-anchors to the top of the (kept) group: the first
    // match is what Enter should hit after typing.
    selection_.reset();
    ClampSelection(keepGroup);
}

void PopupModel::AppendToFilter(wchar_t ch) {
    SetFilter(filter_ + ch);
}

bool PopupModel::BackspaceFilter() {
    if (filter_.empty()) {
        return false;
    }
    std::wstring shorter = filter_;
    shorter.pop_back();
    SetFilter(shorter);
    return true;
}

const PopupItem* PopupModel::SelectedItem() const {
    if (!selection_.has_value()) {
        return nullptr;
    }
    const auto& items = GroupItems(selection_->group);
    if (selection_->index >= items.size()) {
        return nullptr;
    }
    return items[selection_->index];
}

void PopupModel::MoveUp() {
    if (selection_.has_value() && selection_->index > 0) {
        --selection_->index;
    }
}

void PopupModel::MoveDown() {
    if (!selection_.has_value()) {
        return;
    }
    const auto& items = GroupItems(selection_->group);
    if (selection_->index + 1 < items.size()) {
        ++selection_->index;
    }
}

void PopupModel::MoveLeft() {
    if (!selection_.has_value() || selection_->group == Group::Registers) {
        return;
    }
    if (visibleRegisters_.empty()) {
        return;  // nothing to hop to
    }
    selection_->group = Group::Registers;
    selection_->index = (std::min)(selection_->index, visibleRegisters_.size() - 1);
}

void PopupModel::MoveRight() {
    if (!selection_.has_value() || selection_->group == Group::History) {
        return;
    }
    if (visibleHistory_.empty()) {
        return;
    }
    selection_->group = Group::History;
    selection_->index = (std::min)(selection_->index, visibleHistory_.size() - 1);
}

void PopupModel::SelectAt(Group group, std::size_t index) {
    if (index >= GroupItems(group).size()) {
        return;
    }
    selection_ = Selection{ group, index };
}

PopupModel::EscapeResult PopupModel::HandleEscape() {
    if (mode_ == Mode::Edit) {
        mode_ = Mode::Browse;
        return EscapeResult::LeftEditMode;
    }
    if (!filter_.empty()) {
        SetFilter({});
        return EscapeResult::ClearedFilter;
    }
    return EscapeResult::Close;
}

void PopupModel::Refilter() {
    visibleRegisters_.clear();
    visibleHistory_.clear();
    for (const auto& item : registers_) {
        if (ContainsInsensitive(item.searchText, filter_)) {
            visibleRegisters_.push_back(&item);
        }
    }
    for (const auto& item : history_) {
        if (ContainsInsensitive(item.searchText, filter_)) {
            visibleHistory_.push_back(&item);
        }
    }
}

void PopupModel::ClampSelection(std::optional<Group> preferredGroup) {
    const auto firstNonEmpty = [this]() -> std::optional<Selection> {
        // History is the default landing spot: the popup's headline use is
        // "grab a recent item"; registers are the durable side dish.
        if (!visibleHistory_.empty()) {
            return Selection{ Group::History, 0 };
        }
        if (!visibleRegisters_.empty()) {
            return Selection{ Group::Registers, 0 };
        }
        return std::nullopt;
    };

    if (!selection_.has_value()) {
        if (preferredGroup.has_value() && !GroupItems(*preferredGroup).empty()) {
            selection_ = Selection{ *preferredGroup, 0 };
        } else {
            selection_ = firstNonEmpty();
        }
        return;
    }

    const auto& items = GroupItems(selection_->group);
    if (items.empty()) {
        const Group other =
            selection_->group == Group::History ? Group::Registers : Group::History;
        if (!GroupItems(other).empty()) {
            selection_ = Selection{ other, 0 };
        } else {
            selection_.reset();
        }
        return;
    }
    selection_->index = (std::min)(selection_->index, items.size() - 1);
}

const std::vector<const PopupItem*>& PopupModel::GroupItems(Group group) const {
    return group == Group::Registers ? visibleRegisters_ : visibleHistory_;
}

std::string NextAutoRegisterName(const std::vector<std::string>& existingNames) {
    for (uint64_t n = 1;; ++n) {
        const std::string candidate = "item-" + std::to_string(n);
        if (std::find(existingNames.begin(), existingNames.end(), candidate) ==
            existingNames.end()) {
            return candidate;
        }
    }
}
