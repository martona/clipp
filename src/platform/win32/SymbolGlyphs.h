#pragma once

// Renders glyphs from the embedded "Clipp Symbols" Nerd Fonts subset (RCDATA in
// the exe) to bitmaps via DirectWrite + Direct2D, cached by (codepoint, color).
//
// Why rasterize instead of a XAML FontIcon: system XAML (Windows.UI.Xaml) can
// only resolve a FontFamily from an ms-appx URI or an installed system font, not
// from a font embedded in the PE resource section. Embedding keeps the unpackaged
// exe self-contained, so we load the font into an in-memory DirectWrite font set
// and draw the glyphs ourselves.
//
// UI-thread affinity: construct and call on the XAML UI thread only.

#include <cstdint>
#include <memory>

#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

namespace clipp::win32 {

class SymbolGlyphs {
public:
    // Lazily constructed singleton (first use loads the embedded font).
    static SymbolGlyphs& Instance();

    // An ImageSource for `codepoint` filled with `fill`. When `haloFrac > 0` and
    // `halo` is opaque, the glyph is first dilated in `halo` (a shape-hugging
    // knockout outline whose width is that fraction of the bitmap) so an overlaid
    // badge stays legible against whatever it sits on. Returns a null WriteableBitmap
    // (operator bool == false) when codepoint == 0 or rendering is unavailable;
    // callers should hide the Image in that case. Cached per (codepoint, fill, halo,
    // haloFrac).
    winrt::Windows::UI::Xaml::Media::Imaging::WriteableBitmap
    Glyph(char32_t codepoint,
          winrt::Windows::UI::Color fill,
          winrt::Windows::UI::Color halo = {},
          float haloFrac = 0.0f);

    SymbolGlyphs(const SymbolGlyphs&) = delete;
    SymbolGlyphs& operator=(const SymbolGlyphs&) = delete;

private:
    SymbolGlyphs();

    struct Impl;
    // Opaque to keep d2d/dwrite/wic headers out of this header.
    std::shared_ptr<Impl> impl_;
};

}  // namespace clipp::win32
