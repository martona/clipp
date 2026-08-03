#pragma once

#include "TypePlan.h"

#include <string>

namespace clipp {

// Translates text into a keystroke plan using the CURRENTLY ACTIVE Windows
// keyboard layout (hitsc offers a layout picker; clipp deliberately doesn't —
// if the receiving side expects another layout, switch layouts and retype).
// Newlines become Enter (CRLF folds to one), tab becomes Tab. Everything else
// goes through VkKeyScanEx: unreachable characters fail the whole translation
// with the offending codepoint and its line/column.
TypeResult TranslateTextToPlan(const std::wstring& text);

// Human name of the active layout ("United States-International"), for the
// "can't type X in this layout" message. Empty if it can't be resolved.
std::wstring ActiveKeyboardLayoutName();

// True when the active input profile is a real IME (Pinyin, Japanese, ...).
// An IME has no 1:1 character-to-keystroke map, so typing is refused outright.
bool ActiveLayoutIsIme();

}  // namespace clipp
