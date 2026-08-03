#include "TypeLayout.h"

#include "platform.h"

#include <Windows.h>

namespace clipp {

namespace {

// VkKeyScanEx shift-state bits -> modifier virtual-keys. Ctrl+Alt is AltGr;
// reproduce it the way the physical key does (left Ctrl plus right Alt), which
// is what layouts and apps both expect to see.
void AppendModifiers(int shiftState, std::vector<TypeKeyCode>& modifiers) {
    const bool needShift = (shiftState & 1) != 0;
    const bool needCtrl = (shiftState & 2) != 0;
    const bool needAlt = (shiftState & 4) != 0;
    if (needCtrl && needAlt) {
        modifiers.push_back(VK_LCONTROL);
        modifiers.push_back(VK_RMENU);  // AltGr
    } else {
        if (needCtrl) {
            modifiers.push_back(VK_LCONTROL);
        }
        if (needAlt) {
            modifiers.push_back(VK_LMENU);
        }
    }
    if (needShift) {
        modifiers.push_back(VK_LSHIFT);
    }
}

void AppendSimple(TypePlan& plan, TypeKeyCode key) {
    KeyChord chord;
    chord.key = key;
    plan.chords.push_back(std::move(chord));
    plan.characterCount += 1;
}

// UTF-16 -> code points, so line/column counts (and the reported codepoint)
// are in Unicode scalars rather than surrogate halves.
std::vector<char32_t> ToCodepoints(const std::wstring& text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t unit = text[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size()
            && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            const char32_t high = static_cast<char32_t>(unit - 0xD800);
            const char32_t low = static_cast<char32_t>(text[i + 1] - 0xDC00);
            out.push_back(0x10000 + (high << 10) + low);
            ++i;
            continue;
        }
        out.push_back(static_cast<char32_t>(unit));
    }
    return out;
}

}  // namespace

bool ActiveLayoutIsIme() {
    // A real IME is identified by its HKL device handle living in the 0xExxx
    // range; ordinary layouts (US = 0x0409xxxx, alternates like Dvorak =
    // 0xFxxx) are not. Deliberately NOT ImmIsIME(): on TSF-era Windows that
    // reports true even for plain layouts, which would misjudge US.
    const HKL layout = GetKeyboardLayout(0);
    const auto raw = static_cast<unsigned long>(reinterpret_cast<ULONG_PTR>(layout));
    return (raw & 0xF0000000UL) == 0xE0000000UL;
}

std::wstring ActiveKeyboardLayoutName() {
    wchar_t klid[KL_NAMELENGTH] = {};
    if (GetKeyboardLayoutNameW(klid) == FALSE) {
        return {};
    }
    // Friendly name from HKLM\...\Keyboard Layouts\<KLID>\Layout Text, falling
    // back to the KLID itself. ("Layout Display Name" is the localized one but
    // needs an MUI resolve through SHLoadIndirectString.)
    std::wstring path = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\";
    path += klid;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return klid;
    }
    wchar_t text[128] = {};
    DWORD size = sizeof(text);
    DWORD type = 0;
    const LONG status = RegQueryValueExW(key, L"Layout Text", nullptr, &type,
        reinterpret_cast<BYTE*>(text), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || text[0] == L'\0') {
        return klid;
    }
    text[(sizeof(text) / sizeof(text[0])) - 1] = L'\0';
    return text;
}

TypeResult TranslateTextToPlan(const std::wstring& text) {
    TypeResult result;
    TypePlan& plan = result.plan;
    const HKL layout = GetKeyboardLayout(0);

    const std::vector<char32_t> scalars = ToCodepoints(text);
    int line = 1;
    int column = 0;
    for (std::size_t i = 0; i < scalars.size(); ++i) {
        const char32_t cp = scalars[i];
        ++column;

        // Newlines -> Enter (layout-independent). Swallow the CR of a CRLF
        // pair so "\r\n" yields a single Enter.
        if (cp == U'\r') {
            if (i + 1 < scalars.size() && scalars[i + 1] == U'\n') {
                --column;  // the CR isn't its own column; the LF lands next
                continue;
            }
            AppendSimple(plan, VK_RETURN);
            plan.enterCount += 1;
            line += 1;
            column = 0;
            continue;
        }
        if (cp == U'\n') {
            AppendSimple(plan, VK_RETURN);
            plan.enterCount += 1;
            line += 1;
            column = 0;
            continue;
        }
        if (cp == U'\t') {
            AppendSimple(plan, VK_TAB);
            continue;
        }

        // VkKeyScanExW takes a single UTF-16 unit, so anything outside the BMP
        // is unreachable as a keystroke.
        if (cp > 0xFFFF) {
            result.error = TypeError{ cp, line, column };
            return result;
        }
        const SHORT scan = VkKeyScanExW(static_cast<WCHAR>(cp), layout);
        if (scan == -1) {
            result.error = TypeError{ cp, line, column };
            return result;
        }
        const int virtualKey = LOBYTE(scan);
        const int shiftState = HIBYTE(scan);
        if (virtualKey == 0) {
            result.error = TypeError{ cp, line, column };
            return result;
        }
        // A layout that can't map the key back to a scancode can't produce a
        // real key event for it either.
        if (MapVirtualKeyExW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC, layout) == 0) {
            result.error = TypeError{ cp, line, column };
            return result;
        }

        KeyChord chord;
        AppendModifiers(shiftState, chord.modifiers);
        chord.key = static_cast<TypeKeyCode>(virtualKey);
        plan.chords.push_back(std::move(chord));
        plan.characterCount += 1;
    }

    result.ok = true;
    return result;
}

}  // namespace clipp
