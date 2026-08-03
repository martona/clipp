#include "TypeLayout.h"

#import <Carbon/Carbon.h>
#import <Foundation/Foundation.h>

#include <unordered_map>
#include <vector>

namespace clipp {

namespace {

// Modifier combinations we are willing to synthesize for plain text, in
// preference order: the simplest chord that produces a character wins. Control
// and Command are deliberately absent — those are shortcuts, not text.
struct ModifierCombination {
    UInt32 carbonMask;                  // shiftKey / optionKey, for UCKeyTranslate
    std::vector<TypeKeyCode> keys;      // the chord's modifier keys
};

const std::vector<ModifierCombination>& ModifierCombinations() {
    static const std::vector<ModifierCombination> combinations = {
        { 0, {} },
        { shiftKey, { kVK_Shift } },
        { optionKey, { kVK_Option } },
        { shiftKey | optionKey, { kVK_Shift, kVK_Option } },
    };
    return combinations;
}

// One character -> the chord that types it on the active layout.
using LayoutTable = std::unordered_map<char32_t, KeyChord>;

// Walks the layout's whole key/modifier space once and inverts it. There is no
// "character -> key" API on macOS (VkKeyScanEx has no counterpart), so the only
// way to answer the question is to translate every key in every modifier state
// and keep the results.
LayoutTable BuildLayoutTable() {
    LayoutTable table;
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (source == nullptr) {
        return table;
    }
    CFDataRef layoutData = static_cast<CFDataRef>(
        TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (layoutData == nullptr) {
        CFRelease(source);
        return table;
    }
    const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layoutData));
    const UInt32 keyboardType = LMGetKbdType();

    for (const ModifierCombination& combination : ModifierCombinations()) {
        // UCKeyTranslate wants the modifiers in the high byte's positions.
        const UInt32 modifierKeyState = (combination.carbonMask >> 8) & 0xFF;
        for (UInt16 keyCode = 0; keyCode < 128; ++keyCode) {
            UInt32 deadKeyState = 0;
            UniChar characters[8] = {};
            UniCharCount length = 0;
            // NoDeadKeys: a dead key resolves to its own standalone character
            // instead of arming an accent. Characters that only exist behind a
            // dead-key SEQUENCE therefore stay out of the table and are
            // reported as untypable, which is the honest answer — we type one
            // key per character and never compose.
            const OSStatus status = UCKeyTranslate(
                layout, keyCode, kUCKeyActionDisplay, modifierKeyState, keyboardType,
                kUCKeyTranslateNoDeadKeysBit, &deadKeyState,
                sizeof(characters) / sizeof(characters[0]), &length, characters);
            if (status != noErr || length != 1) {
                continue;  // no output, or a multi-character expansion we can't address
            }
            const char32_t codepoint = static_cast<char32_t>(characters[0]);
            if (codepoint < 0x20 || codepoint == 0x7F) {
                continue;  // control output (Return/Tab/Escape) is handled explicitly
            }
            // First writer wins, so the simplest modifier combination sticks.
            if (table.find(codepoint) != table.end()) {
                continue;
            }
            KeyChord chord;
            chord.modifiers = combination.keys;
            chord.key = static_cast<TypeKeyCode>(keyCode);
            table.emplace(codepoint, std::move(chord));
        }
    }

    CFRelease(source);
    return table;
}

void AppendSimple(TypePlan& plan, TypeKeyCode key) {
    KeyChord chord;
    chord.key = key;
    plan.chords.push_back(std::move(chord));
    plan.characterCount += 1;
}

std::wstring CFStringToWide(CFStringRef text) {
    if (text == nullptr) {
        return {};
    }
    NSString* value = (__bridge NSString*)text;
    const char* utf8 = [value UTF8String];
    if (utf8 == nullptr) {
        return {};
    }
    // wchar_t is UTF-32 here, so go through NSString's UTF-32 encoding.
    NSData* data = [value dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    if (data == nil) {
        return {};
    }
    const auto* units = static_cast<const uint32_t*>([data bytes]);
    const std::size_t count = [data length] / sizeof(uint32_t);
    std::wstring out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<wchar_t>(units[i]));
    }
    return out;
}

}  // namespace

bool ActiveLayoutIsIme() {
    // An IME (Japanese, Pinyin, ...) is an input source with NO unicode key
    // layout data: there is no character-to-key map to invert, which is exactly
    // why typing is refused. Note this asks the current INPUT SOURCE, not
    // TISCopyCurrentKeyboardLayoutInputSource — the latter reports the layout
    // sitting underneath an active IME and would look perfectly typable.
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (source == nullptr) {
        return false;
    }
    const bool isIme =
        TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) == nullptr;
    CFRelease(source);
    return isIme;
}

std::wstring ActiveKeyboardLayoutName() {
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    if (source == nullptr) {
        return {};
    }
    const auto name = static_cast<CFStringRef>(
        TISGetInputSourceProperty(source, kTISPropertyLocalizedName));
    std::wstring out = CFStringToWide(name);
    CFRelease(source);
    return out;
}

TypeResult TranslateTextToPlan(const std::wstring& text) {
    TypeResult result;
    TypePlan& plan = result.plan;

    // Rebuilt per translation: the user can switch layouts between two runs,
    // and the whole point of the error message is to name the layout in force
    // for THIS attempt.
    const LayoutTable table = BuildLayoutTable();

    int line = 1;
    int column = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // wchar_t is UTF-32 on macOS, so a scalar per element already.
        const char32_t codepoint = static_cast<char32_t>(text[i]);
        ++column;

        if (codepoint == U'\r') {
            if (i + 1 < text.size() && text[i + 1] == U'\n') {
                --column;  // the CR isn't its own column; the LF lands next
                continue;
            }
            AppendSimple(plan, kVK_Return);
            plan.enterCount += 1;
            line += 1;
            column = 0;
            continue;
        }
        if (codepoint == U'\n') {
            AppendSimple(plan, kVK_Return);
            plan.enterCount += 1;
            line += 1;
            column = 0;
            continue;
        }
        if (codepoint == U'\t') {
            AppendSimple(plan, kVK_Tab);
            continue;
        }

        const auto found = table.find(codepoint);
        if (found == table.end()) {
            result.error = TypeError{ codepoint, line, column };
            return result;
        }
        plan.chords.push_back(found->second);
        plan.characterCount += 1;
    }

    result.ok = true;
    return result;
}

}  // namespace clipp
