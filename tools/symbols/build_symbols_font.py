#!/usr/bin/env python3
"""Regenerate the Clipp device-type symbol font + its codepoint header.

Reads ``tools/symbols/manifest.json``, resolves each ``nf-*`` glyph name to a
codepoint via the pinned Nerd Fonts ``glyphnames.json``, subsets the
*Symbols Nerd Font* down to just those glyphs, renames the family to
"Clipp Symbols", and writes two committed outputs:

  * ``src/resources/ClippSymbols.ttf``   (a few KB)
  * ``src/OsGlyphs.h``                    (generated C++ header)

This is run **on demand** (when the glyph set changes), not at build time, so
the build never needs Python/fonttools. See BUILDING.md.

Usage:
    pip install fonttools
    python tools/symbols/build_symbols_font.py
    # offline / pinned inputs:
    python tools/symbols/build_symbols_font.py \\
        --symbols-font /path/SymbolsNerdFont-Regular.ttf \\
        --glyphnames   /path/glyphnames.json

Requires: fonttools (`pip install fonttools`). Network access unless both
inputs are supplied locally.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

# --- Pinned Nerd Fonts source -------------------------------------------------
# Bump together; glyph codepoints are stable within a major version but the v3
# release renumbered Font Awesome / Material Design, so always resolve names via
# the *matching* glyphnames.json rather than hardcoding codepoints.
NERD_FONTS_VERSION = "v3.4.0"
SYMBOLS_ZIP_URL = (
    f"https://github.com/ryanoasis/nerd-fonts/releases/download/"
    f"{NERD_FONTS_VERSION}/NerdFontsSymbolsOnly.zip"
)
SYMBOLS_TTF_IN_ZIP = "SymbolsNerdFont-Regular.ttf"
GLYPHNAMES_URL = (
    f"https://raw.githubusercontent.com/ryanoasis/nerd-fonts/"
    f"{NERD_FONTS_VERSION}/glyphnames.json"
)

FAMILY_NAME = "Clipp Symbols"
POSTSCRIPT_NAME = "ClippSymbols"

# OsType enum order MUST match src/OsType.h.
OSTYPE_ORDER = ["Unknown", "Windows", "MacOS", "IOS_iPhone", "IOS_iPad", "Linux"]

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST_PATH = Path(__file__).resolve().parent / "manifest.json"
OUT_TTF = REPO_ROOT / "src" / "resources" / "ClippSymbols.ttf"
OUT_HEADER = REPO_ROOT / "src" / "OsGlyphs.h"


def fail(msg: str) -> "NoReturn":  # type: ignore[name-defined]
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def fetch(url: str) -> bytes:
    print(f"  fetching {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "clipp-symbols-build"})
    with urllib.request.urlopen(req) as resp:  # noqa: S310 (trusted, pinned URL)
        return resp.read()


def load_source_font(arg_path: str | None) -> bytes:
    if arg_path:
        return Path(arg_path).read_bytes()
    data = fetch(SYMBOLS_ZIP_URL)
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = [n for n in zf.namelist() if n.endswith(SYMBOLS_TTF_IN_ZIP)]
        if not names:
            fail(
                f"{SYMBOLS_TTF_IN_ZIP} not found in {SYMBOLS_ZIP_URL}; "
                f"contents: {zf.namelist()}"
            )
        return zf.read(names[0])


def load_glyphnames(arg_path: str | None) -> dict:
    raw = Path(arg_path).read_bytes() if arg_path else fetch(GLYPHNAMES_URL)
    print(f"  glyphnames.json sha256: {hashlib.sha256(raw).hexdigest()}")
    return json.loads(raw)


def resolve_codepoint(glyphnames: dict, nf_name: str) -> int:
    """Map an 'nf-dev-apple'-style name to its integer codepoint, or fail."""
    key = nf_name[len("nf-"):] if nf_name.startswith("nf-") else nf_name
    entry = glyphnames.get(key)
    if not entry or "code" not in entry:
        fail(
            f"glyph '{nf_name}' (key '{key}') not present in glyphnames.json for "
            f"Nerd Fonts {NERD_FONTS_VERSION}. Check the name on the cheat sheet "
            f"(https://www.nerdfonts.com/cheat-sheet) or bump NERD_FONTS_VERSION."
        )
    return int(entry["code"], 16)


def subset_and_rename(font_bytes: bytes, codepoints: set[int]):
    from fontTools import subset
    from fontTools.ttLib import TTFont

    font = TTFont(io.BytesIO(font_bytes))

    options = subset.Options()
    options.layout_features = []        # icon glyphs need no GSUB/GPOS shaping
    options.glyph_names = False         # drop post glyph names -> smaller
    options.notdef_outline = True       # keep .notdef so unmapped chars show tofu
    options.recalc_bounds = True
    options.recalc_timestamp = False
    options.name_IDs = ["*"]            # keep name table (rewritten below)
    options.name_legacy = True
    options.name_languages = ["*"]
    options.drop_tables = []

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=sorted(codepoints))
    subsetter.subset(font)

    # Rewrite the name table to a single, deterministic family so every platform
    # references the font by the same "Clipp Symbols" name.
    name = font["name"]
    name.names = []

    def set_name(name_id: int, value: str) -> None:
        name.setName(value, name_id, 3, 1, 0x409)  # Windows, Unicode BMP, en-US
        name.setName(value, name_id, 1, 0, 0)       # Mac, Roman, English

    set_name(1, FAMILY_NAME)               # Family
    set_name(2, "Regular")                  # Subfamily
    set_name(3, f"{POSTSCRIPT_NAME};Clipp")  # Unique ID
    set_name(4, FAMILY_NAME)               # Full name
    set_name(6, POSTSCRIPT_NAME)           # PostScript name
    set_name(16, FAMILY_NAME)              # Typographic family
    set_name(17, "Regular")                 # Typographic subfamily

    return font


def const_name(nf_name: str) -> str:
    base = nf_name[len("nf-"):] if nf_name.startswith("nf-") else nf_name
    return "kGlyph_" + base.replace("-", "_")


def emit_header(mapping: dict, codepoints_by_name: dict[str, int]) -> str:
    lines: list[str] = []
    w = lines.append
    w("#pragma once")
    w("//")
    w("// GENERATED FILE - DO NOT EDIT BY HAND.")
    w("// Produced by tools/symbols/build_symbols_font.py from")
    w(f"// tools/symbols/manifest.json (Nerd Fonts {NERD_FONTS_VERSION}).")
    w("// Re-run that script after editing the manifest, and commit both this")
    w("// header and src/resources/ClippSymbols.ttf.")
    w("//")
    w("// Maps OsType -> the two glyph codepoints rendered on a peer row. A")
    w('// codepoint of 0 means "no glyph". Glyphs live in the bundled')
    w('// "Clipp Symbols" font.')
    w("//")
    w('#include "OsType.h"')
    w("")
    w("namespace clipp {")
    w("")
    w("struct OsGlyphs {")
    w("    char32_t family;  // OS-family mark (0 = none)")
    w("    char32_t device;  // device-type mark (0 = none)")
    w("};")
    w("")
    for nf_name in sorted(codepoints_by_name):
        cp = codepoints_by_name[nf_name]
        w(f"// {nf_name}")
        w(f"inline constexpr char32_t {const_name(nf_name)} = 0x{cp:04X};")
    w("")
    w("inline constexpr OsGlyphs OsGlyphsFor(OsType osType) {")
    w("    switch (osType) {")
    for os_name in OSTYPE_ORDER:
        if os_name == "Unknown":
            continue
        pair = mapping[os_name]
        fam = const_name(pair["family"])
        dev = const_name(pair["device"]) if pair["device"] else "0"
        w(f"    case OsType::{os_name}: return {{ {fam}, {dev} }};")
    unknown = mapping["Unknown"]
    fam = const_name(unknown["family"])
    dev = const_name(unknown["device"]) if unknown["device"] else "0"
    w("    case OsType::Unknown:")
    w(f"    default: return {{ {fam}, {dev} }};")
    w("    }")
    w("}")
    w("")
    w("}  // namespace clipp")
    w("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--symbols-font", help="local SymbolsNerdFont-Regular.ttf (skip download)")
    parser.add_argument("--glyphnames", help="local glyphnames.json (skip download)")
    args = parser.parse_args()

    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    mapping = manifest["mapping"]

    missing = set(OSTYPE_ORDER) - set(mapping)
    if missing:
        fail(f"manifest.json is missing OsType entries: {sorted(missing)}")

    print("Resolving glyph codepoints...")
    glyphnames = load_glyphnames(args.glyphnames)
    codepoints_by_name: dict[str, int] = {}
    for pair in mapping.values():
        for slot in ("family", "device"):
            nf_name = pair.get(slot)
            if nf_name and nf_name not in codepoints_by_name:
                cp = resolve_codepoint(glyphnames, nf_name)
                codepoints_by_name[nf_name] = cp
                print(f"  {nf_name:24s} -> U+{cp:04X}")

    print("Subsetting font...")
    font_bytes = load_source_font(args.symbols_font)
    print(f"  source font sha256: {hashlib.sha256(font_bytes).hexdigest()}")
    font = subset_and_rename(font_bytes, set(codepoints_by_name.values()))

    OUT_TTF.parent.mkdir(parents=True, exist_ok=True)
    font.save(str(OUT_TTF))
    size = OUT_TTF.stat().st_size
    print(f"  wrote {OUT_TTF.relative_to(REPO_ROOT)} ({size} bytes, "
          f"{len(codepoints_by_name)} glyphs)")

    OUT_HEADER.write_text(emit_header(mapping, codepoints_by_name), encoding="utf-8")
    print(f"  wrote {OUT_HEADER.relative_to(REPO_ROOT)}")
    print("Done. Commit both generated files.")


if __name__ == "__main__":
    main()
