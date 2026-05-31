#pragma once
//
// GENERATED FILE - DO NOT EDIT BY HAND.
// Produced by tools/symbols/build_symbols_font.py from
// tools/symbols/manifest.json (Nerd Fonts v3.4.0).
// Re-run that script after editing the manifest, and commit both this
// header and src/resources/ClippSymbols.ttf.
//
// Maps OsType -> the two glyph codepoints rendered on a peer row. A
// codepoint of 0 means "no glyph". Glyphs live in the bundled
// "Clipp Symbols" font.
//
#include "OsType.h"

namespace clipp {

struct OsGlyphs {
    char32_t family;  // OS-family mark (0 = none)
    char32_t device;  // device-type mark (0 = none)
};

// nf-cod-question
inline constexpr char32_t kGlyph_cod_question = 0xEB32;
// nf-dev-apple
inline constexpr char32_t kGlyph_dev_apple = 0xE711;
// nf-dev-linux
inline constexpr char32_t kGlyph_dev_linux = 0xE712;
// nf-dev-windows11
inline constexpr char32_t kGlyph_dev_windows11 = 0xE8E5;
// nf-fa-tablet
inline constexpr char32_t kGlyph_fa_tablet = 0xED2E;
// nf-md-cellphone
inline constexpr char32_t kGlyph_md_cellphone = 0xF011C;
// nf-md-desktop_classic
inline constexpr char32_t kGlyph_md_desktop_classic = 0xF07C0;

inline constexpr OsGlyphs OsGlyphsFor(OsType osType) {
    switch (osType) {
    case OsType::Windows: return { kGlyph_dev_windows11, kGlyph_md_desktop_classic };
    case OsType::MacOS: return { kGlyph_dev_apple, kGlyph_md_desktop_classic };
    case OsType::IOS_iPhone: return { kGlyph_dev_apple, kGlyph_md_cellphone };
    case OsType::IOS_iPad: return { kGlyph_dev_apple, kGlyph_fa_tablet };
    case OsType::Linux: return { kGlyph_dev_linux, kGlyph_md_desktop_classic };
    case OsType::Unknown:
    default: return { kGlyph_cod_question, 0 };
    }
}

}  // namespace clipp
