#pragma once

#include "TypePlan.h"

#include <string>

namespace clipp {

// Translates text into a keystroke plan using the CURRENTLY ACTIVE keyboard
// layout (hitsc offers a layout picker; clipp deliberately doesn't — if the
// receiving side expects another layout, switch layouts and retype). Newlines
// become Return (CRLF folds to one), tab becomes Tab. Everything else is
// reverse-mapped through the OS layout — Windows: VkKeyScanEx; macOS:
// UCKeyTranslate over the layout's key/modifier space — and any character the
// layout cannot produce fails the WHOLE translation with its codepoint and
// line/column. Partial typing would punch silent holes in the text.
//
// Backends differ in how a chord's modifiers reach the OS, and the difference
// is invisible above this line: Windows emits real modifier key events, while
// macOS carries them as CGEvent flags on the character event (the platform's
// own convention — and it means a cancelled run there can't strand a modifier).
TypeResult TranslateTextToPlan(const std::wstring& text);

// Human name of the active layout ("United States-International", "ABC"), for
// the "can't type X in this layout" message. Empty if it can't be resolved.
std::wstring ActiveKeyboardLayoutName();

// True when the active input profile is a real IME (Pinyin, Japanese, ...).
// An IME has no 1:1 character-to-keystroke map, so typing is refused outright.
bool ActiveLayoutIsIme();

}  // namespace clipp
