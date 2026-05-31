#include "ClippGlyphBadge.h"

#ifdef __APPLE__

#include "OsGlyphs.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

namespace {

// Tuning, shared by both platforms. The device box is a fraction of the view so it
// scales with whatever size each platform gives the badge.
constexpr CGFloat kDeviceBoxFraction = 13.0 / 27.0;  // device badge box / view size
constexpr CGFloat kGlyphFill = 0.85;                 // glyph ink as a fraction of its box
constexpr CGFloat kHaloPct = 9.0;                    // halo width as % of the em

void EnsureSymbolsFontRegistered() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURL* url = [[NSBundle mainBundle] URLForResource:@"ClippSymbols" withExtension:@"ttf"];
        if (url != nil) {
            CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url, kCTFontManagerScopeProcess, NULL);
        }
    });
}

NSString* GlyphString(char32_t codepoint) {
    return [[NSString alloc] initWithBytes:&codepoint
                                    length:sizeof(codepoint)
                                  encoding:NSUTF32LittleEndianStringEncoding];
}

void DrawGlyph(CGContextRef ctx,
               char32_t codepoint,
               CGColorRef fill,
               CGColorRef stroke,
               CGFloat strokePct,
               CGRect rect) {
    if (codepoint == 0) {
        return;
    }
    NSString* str = GlyphString(codepoint);
    // Reference size only -- the glyph is scaled to fit `rect` below.
    const CGFloat refSize = 128.0;
    CTFontRef font = CTFontCreateWithName(CFSTR("Clipp Symbols"), refSize, NULL);
    if (str == nil || font == NULL) {
        if (font != NULL) {
            CFRelease(font);
        }
        return;
    }

    // Shape into a single glyph (this is how the astral md-* codepoints survive the
    // surrogate pair) and capture the glyph id + the font CoreText actually used.
    NSDictionary* attrs = @{ (__bridge NSString*)kCTFontAttributeName: (__bridge id)font };
    NSAttributedString* attributed = [[NSAttributedString alloc] initWithString:str attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CGGlyph glyph = 0;
    CTFontRef glyphFont = NULL;
    if (CFArrayGetCount(runs) > 0) {
        CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, 0);
        if (CTRunGetGlyphCount(run) > 0) {
            CTRunGetGlyphs(run, CFRangeMake(0, 1), &glyph);
            CTFontRef runFont = (CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
            if (runFont != NULL) {
                glyphFont = (CTFontRef)CFRetain(runFont);
            }
        }
    }
    CFRelease(line);
    if (glyphFont == NULL || glyph == 0) {
        CFRelease(font);
        return;
    }

    // Fit the glyph's TRUE ink bounds into rect (times the fill fraction) and center
    // it -- icon metrics vary too much to size by point alone, and fitting can't
    // overflow the box so nothing clips at the view edge.
    const CGRect ink = CTFontGetBoundingRectsForGlyphs(glyphFont, kCTFontOrientationHorizontal, &glyph, NULL, 1);
    if (ink.size.width > 0.0 && ink.size.height > 0.0) {
        const CGFloat scale = kGlyphFill * MIN(rect.size.width / ink.size.width,
                                               rect.size.height / ink.size.height);
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, CGRectGetMidX(rect), CGRectGetMidY(rect));
        CGContextScaleCTM(ctx, scale, scale);

        const CGPoint pos = CGPointMake(-(ink.origin.x + ink.size.width / 2.0),
                                        -(ink.origin.y + ink.size.height / 2.0));

        // Halo FIRST (stroke only, bg color), then the fill ON TOP -- a single
        // fill+stroke pass strokes over the fill, so the bg halo would eat into (and
        // for thin glyphs erase) it. Width is doubled since the fill covers the inner
        // half, leaving an outward-only halo.
        if (stroke != NULL && strokePct > 0.0) {
            CGContextSetTextDrawingMode(ctx, kCGTextStroke);
            CGContextSetStrokeColorWithColor(ctx, stroke);
            CGContextSetLineWidth(ctx, refSize * strokePct / 100.0 * 2.0);
            CGContextSetLineJoin(ctx, kCGLineJoinRound);
            CTFontDrawGlyphs(glyphFont, &glyph, &pos, 1, ctx);
        }
        CGContextSetTextDrawingMode(ctx, kCGTextFill);
        CGContextSetFillColorWithColor(ctx, fill);
        CTFontDrawGlyphs(glyphFont, &glyph, &pos, 1, ctx);
        CGContextRestoreGState(ctx);
    }

    CFRelease(glyphFont);
    CFRelease(font);
}

}  // namespace

void ClippDrawDeviceBadge(CGContextRef ctx,
                          CGRect bounds,
                          OsType osType,
                          CGColorRef familyColor,
                          CGColorRef deviceColor,
                          CGColorRef haloColor) {
    EnsureSymbolsFontRegistered();
    const clipp::OsGlyphs glyphs = clipp::OsGlyphsFor(osType);

    // Family glyph: primary, fills the box.
    DrawGlyph(ctx, glyphs.family, familyColor, NULL, 0.0, bounds);

    // Device glyph: smaller badge over the bottom-right quarter (y-up: min-y = bottom),
    // with the bg-color knockout halo.
    if (glyphs.device != 0) {
        const CGFloat dev = bounds.size.width * kDeviceBoxFraction;
        const CGRect devRect = CGRectMake(CGRectGetMaxX(bounds) - dev,
                                          CGRectGetMinY(bounds),
                                          dev, dev);
        DrawGlyph(ctx, glyphs.device, deviceColor, haloColor, kHaloPct, devRect);
    }
}

#endif  // __APPLE__
