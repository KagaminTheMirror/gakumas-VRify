#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace gakumas::ui {

inline bool IsCjkWrapCharacter(unsigned int c) {
    return (c >= 0x2e80 && c <= 0x9fff) || // CJK punctuation, kana, bopomofo, Han
        (c >= 0xac00 && c <= 0xd7af) ||   // Hangul
        (c >= 0xf900 && c <= 0xfaff) ||
        (c >= 0xff00 && c <= 0xffef) ||
        (c >= 0x20000 && c <= 0x3134f);
}

inline bool CannotStartCjkLine(unsigned int c) {
    switch (c) {
    case ',': case '.': case ':': case ';': case '!': case '?':
    case ')': case ']': case '}':
    case 0x3001: case 0x3002: case 0x3005: case 0x30fc:
    case 0x3009: case 0x300b: case 0x300d: case 0x300f:
    case 0x3011: case 0x3015: case 0x3017: case 0x3019: case 0x301b:
    case 0xff01: case 0xff09: case 0xff0c: case 0xff0e:
    case 0xff1a: case 0xff1b: case 0xff1f: case 0xff3d: case 0xff5d:
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
    case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308e:
    case 0x30a1: case 0x30a3: case 0x30a5: case 0x30a7: case 0x30a9:
    case 0x30c3: case 0x30e3: case 0x30e5: case 0x30e7: case 0x30ee:
        return true;
    default: return false;
    }
}

inline bool CannotEndCjkLine(unsigned int c) {
    switch (c) {
    case '(': case '[': case '{':
    case 0x3008: case 0x300a: case 0x300c: case 0x300e:
    case 0x3010: case 0x3014: case 0x3016: case 0x3018: case 0x301a:
    case 0xff08: case 0xff3b: case 0xff5b:
        return true;
    default: return false;
    }
}

// nullptr preserves the pinned upstream algorithm for non-CJK paragraphs.
// Both CalcTextSizeA and RenderText reach this same boundary calculation.
inline const char* CjkWordWrapPosition(const ImFont& font, float scale,
    const char* text, const char* end, float width) {
    bool hasCjk = false;
    for (const char* p = text; p < end && *p != '\n';) {
        unsigned int c = 0;
        const int length = ImTextCharFromUtf8(&c, p, end);
        if (length <= 0) break;
        if (IsCjkWrapCharacter(c)) { hasCjk = true; break; }
        p += length;
    }
    if (!hasCjk) return nullptr;

    width /= scale;
    float used = 0;
    unsigned int previous = 0;
    const char* boundary = nullptr;
    for (const char* p = text; p < end;) {
        unsigned int c = 0;
        const int length = ImTextCharFromUtf8(&c, p, end);
        if (length <= 0 || c == '\n') return p;
        if (c == '\r') { p += length; continue; }
        const bool blank = ImCharIsBlankW(c);
        const bool previousBlank = ImCharIsBlankW(previous);
        if (p > text && used <= width &&
            !CannotEndCjkLine(previous) && !CannotStartCjkLine(c) &&
            ((blank && !previousBlank) ||
                (!blank && (previousBlank || IsCjkWrapCharacter(previous) || IsCjkWrapCharacter(c))) ||
                previous == '.' || previous == ',' || previous == ';' || previous == '!' || previous == '?'))
            boundary = p;
        used += c < static_cast<unsigned int>(font.IndexAdvanceX.Size)
            ? font.IndexAdvanceX[static_cast<int>(c)] : font.FallbackAdvanceX;
        if (!blank && used > width) {
            if (boundary) return boundary;
            // A token wider than the entire line must still make UTF-8-safe progress.
            return p > text ? p : p + length;
        }
        previous = c;
        p += length;
    }
    return end;
}

} // namespace gakumas::ui
