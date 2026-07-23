// PopupModel: the popup's platform-free selection/filter/mode state machine,
// and the item-N auto-name allocator.

#include <doctest/doctest.h>

#include "PopupModel.h"

#include <string>
#include <vector>

namespace {

PopupItem Reg(const std::string& name, const std::wstring& search, bool actionable = true) {
    PopupItem item;
    item.kind = PopupItem::Kind::Register;
    item.registerName = name;
    item.searchText = search;
    item.actionable = actionable;
    return item;
}

PopupItem Hist(uint64_t id, const std::wstring& search, bool actionable = true) {
    PopupItem item;
    item.kind = PopupItem::Kind::History;
    item.historyId = id;
    item.searchText = search;
    item.actionable = actionable;
    return item;
}

PopupModel MakeModel() {
    PopupModel model;
    model.SetItems(
        { Reg("url", L"url https://example.com"), Reg("notes", L"notes shopping list") },
        { Hist(1, L"newest clipboard text"), Hist(2, L"older entry"), Hist(3, L"oldest URL here") });
    return model;
}

}  // namespace

TEST_CASE("popup model: initial selection lands on the first history row") {
    PopupModel model = MakeModel();
    REQUIRE(model.Selected().has_value());
    CHECK(model.Selected()->group == PopupModel::Group::History);
    CHECK(model.Selected()->index == 0);
    REQUIRE(model.SelectedItem() != nullptr);
    CHECK(model.SelectedItem()->historyId == 1);

    // History empty -> falls back to registers; both empty -> none.
    PopupModel registersOnly;
    registersOnly.SetItems({ Reg("only", L"only") }, {});
    REQUIRE(registersOnly.Selected().has_value());
    CHECK(registersOnly.Selected()->group == PopupModel::Group::Registers);

    PopupModel empty;
    empty.SetItems({}, {});
    CHECK_FALSE(empty.Selected().has_value());
    CHECK(empty.SelectedItem() == nullptr);
    CHECK(empty.Empty());
}

TEST_CASE("popup model: up/down clamp, left/right hop groups keeping the row") {
    PopupModel model = MakeModel();

    model.MoveUp();  // already at the top: clamped
    CHECK(model.Selected()->index == 0);
    model.MoveDown();
    model.MoveDown();
    CHECK(model.Selected()->index == 2);
    model.MoveDown();  // bottom: clamped
    CHECK(model.Selected()->index == 2);

    // Left from History row 2 -> Registers, clamped to its last row (1).
    model.MoveLeft();
    CHECK(model.Selected()->group == PopupModel::Group::Registers);
    CHECK(model.Selected()->index == 1);
    model.MoveLeft();  // already leftmost: no-op
    CHECK(model.Selected()->group == PopupModel::Group::Registers);

    model.MoveRight();
    CHECK(model.Selected()->group == PopupModel::Group::History);
    CHECK(model.Selected()->index == 1);

    // Hopping into an empty group is a no-op.
    PopupModel historyOnly;
    historyOnly.SetItems({}, { Hist(1, L"a") });
    historyOnly.MoveLeft();
    CHECK(historyOnly.Selected()->group == PopupModel::Group::History);

    model.SelectAt(PopupModel::Group::Registers, 0);
    CHECK(model.SelectedItem()->registerName == "url");
    model.SelectAt(PopupModel::Group::Registers, 99);  // out of range: ignored
    CHECK(model.SelectedItem()->registerName == "url");
}

TEST_CASE("popup model: filter narrows both groups, case-insensitively") {
    PopupModel model = MakeModel();

    model.SetFilter(L"URL");
    CHECK(model.VisibleRegisters().size() == 1);   // "url ..."
    CHECK(model.VisibleHistory().size() == 1);     // "... URL here"
    REQUIRE(model.Selected().has_value());
    CHECK(model.Selected()->group == PopupModel::Group::History);
    CHECK(model.SelectedItem()->historyId == 3);   // the only history match

    // Typing more can empty the current group: selection hops to the other.
    model.AppendToFilter(L' ');
    model.AppendToFilter(L'h');  // "URL h" -> only "oldest URL here" matches? no:
                                 // register "url https..." also contains "url h".
    CHECK(model.VisibleRegisters().size() == 1);
    CHECK(model.VisibleHistory().size() == 1);

    model.SetFilter(L"shopping");
    CHECK(model.VisibleHistory().empty());
    REQUIRE(model.Selected().has_value());
    CHECK(model.Selected()->group == PopupModel::Group::Registers);
    CHECK(model.SelectedItem()->registerName == "notes");

    model.SetFilter(L"no-such-string-anywhere");
    CHECK(model.Empty());
    CHECK_FALSE(model.Selected().has_value());

    CHECK(model.BackspaceFilter());  // shrinking the filter restores matches
    CHECK_FALSE(model.Filter().empty());
    model.SetFilter(L"");
    CHECK(model.VisibleRegisters().size() == 2);
    CHECK(model.VisibleHistory().size() == 3);
    CHECK_FALSE(model.BackspaceFilter());  // empty filter: nothing to remove
}

TEST_CASE("popup model: SetItems keeps group, reclamps index") {
    PopupModel model = MakeModel();
    model.MoveDown();
    model.MoveDown();  // History index 2

    // A store update shrinks history to one row: same group, clamped index.
    model.SetItems({ Reg("url", L"url") }, { Hist(9, L"only") });
    REQUIRE(model.Selected().has_value());
    CHECK(model.Selected()->group == PopupModel::Group::History);
    CHECK(model.Selected()->index == 0);
    CHECK(model.SelectedItem()->historyId == 9);

    // History vanishes entirely: selection crosses to registers.
    model.SetItems({ Reg("url", L"url") }, {});
    REQUIRE(model.Selected().has_value());
    CHECK(model.Selected()->group == PopupModel::Group::Registers);
}

TEST_CASE("popup model: escape = leave edit, then clear filter, then close") {
    PopupModel model = MakeModel();
    model.SetFilter(L"url");
    model.EnterEditMode();
    CHECK(model.CurrentMode() == PopupModel::Mode::Edit);

    CHECK(model.HandleEscape() == PopupModel::EscapeResult::LeftEditMode);
    CHECK(model.CurrentMode() == PopupModel::Mode::Browse);
    CHECK_FALSE(model.Filter().empty());

    CHECK(model.HandleEscape() == PopupModel::EscapeResult::ClearedFilter);
    CHECK(model.Filter().empty());

    CHECK(model.HandleEscape() == PopupModel::EscapeResult::Close);
}

TEST_CASE("auto register names: first unused item-N") {
    CHECK(NextAutoRegisterName({}) == "item-1");
    CHECK(NextAutoRegisterName({ "item-1", "item-2" }) == "item-3");
    CHECK(NextAutoRegisterName({ "item-2" }) == "item-1");           // gaps fill first
    CHECK(NextAutoRegisterName({ "item-1", "url", "item-3" }) == "item-2");
}
