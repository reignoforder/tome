/* SPDX-License-Identifier: MIT-0 */
/*
 * UTF-8 helpers for Tome mods. Plain C, header-only, no CRT objects.
 *
 * Every string Tome and the game exchange is UTF-8: JASS strings, frame
 * text, chat, paths in the mod API. JASS's own StringLength and SubString
 * count bytes, so a name like "Jaina Proudmoore" works while "Ζεύς" has
 * twice as many "characters" as letters. These helpers count and cut by code
 * point instead, and convert to and from the UTF-16 that Windows' W
 * functions take.
 *
 * Invalid input never fails: each invalid or truncated sequence reads as one
 * U+FFFD (the Unicode recommendation, "maximal subparts"). Functions that
 * write into a buffer take its size in units including the terminating NUL,
 * always terminate it (if the size isn't 0), never split a code point, and
 * return the length the whole result needs without the NUL, like snprintf:
 * a result >= the size means it was cut short.
 */
#ifndef TOME_UTF8_H
#define TOME_UTF8_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define TOME_UTF8_REPLACEMENT 0xFFFDu

/*
 * Decodes the code point at the start of `s` (`len` > 0 bytes available) into
 * *cp and returns how many bytes it takes (1-4). An invalid sequence gives
 * U+FFFD and the length of its invalid part (at least 1).
 */
static inline size_t tome_utf8_decode(const char* s, size_t len, uint32_t* cp) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char lo = 0x80, hi = 0xBF;
    size_t need, i;
    uint32_t value;
    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if (p[0] >= 0xC2 && p[0] <= 0xDF) {
        need = 1;
        value = p[0] & 0x1Fu;
    } else if (p[0] >= 0xE0 && p[0] <= 0xEF) {
        need = 2;
        value = p[0] & 0x0Fu;
        if (p[0] == 0xE0) lo = 0xA0;      /* overlong */
        if (p[0] == 0xED) hi = 0x9F;      /* UTF-16 surrogates */
    } else if (p[0] >= 0xF0 && p[0] <= 0xF4) {
        need = 3;
        value = p[0] & 0x07u;
        if (p[0] == 0xF0) lo = 0x90;      /* overlong */
        if (p[0] == 0xF4) hi = 0x8F;      /* above U+10FFFF */
    } else {
        *cp = TOME_UTF8_REPLACEMENT;
        return 1;
    }
    for (i = 1; i <= need; ++i) {
        if (i >= len || p[i] < lo || p[i] > hi) {
            *cp = TOME_UTF8_REPLACEMENT;
            return i;
        }
        value = value << 6 | (p[i] & 0x3Fu);
        lo = 0x80;
        hi = 0xBF;
    }
    *cp = value;
    return need + 1;
}

/*
 * Writes `cp` as UTF-8 to `out` (room for 4 bytes, not terminated) and
 * returns how many bytes it took. Surrogates and values above U+10FFFF are
 * written as U+FFFD.
 */
static inline size_t tome_utf8_encode(uint32_t cp, char* out) {
    unsigned char* p = (unsigned char*)out;
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = TOME_UTF8_REPLACEMENT;
    if (cp < 0x80) {
        p[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        p[0] = (unsigned char)(0xC0 | cp >> 6);
        p[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        p[0] = (unsigned char)(0xE0 | cp >> 12);
        p[1] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
        p[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    p[0] = (unsigned char)(0xF0 | cp >> 18);
    p[1] = (unsigned char)(0x80 | (cp >> 12 & 0x3F));
    p[2] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
    p[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

static inline size_t tome_utf8_strlen_(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

/* 1 if the NUL-terminated `s` is valid UTF-8, else 0. */
static inline int tome_utf8_valid(const char* s) {
    size_t len = tome_utf8_strlen_(s), i = 0;
    while (i < len) {
        uint32_t cp;
        size_t n = tome_utf8_decode(s + i, len - i, &cp);
        /* U+FFFD itself is EF BF BD; an invalid part is shorter or starts with F0-F4. */
        if (cp == TOME_UTF8_REPLACEMENT && (n != 3 || (unsigned char)s[i] != 0xEF)) return 0;
        i += n;
    }
    return 1;
}

/* The number of code points in `s` (each invalid part counts as one). */
static inline size_t tome_utf8_length(const char* s) {
    size_t len = tome_utf8_strlen_(s), i = 0, count = 0;
    while (i < len) {
        uint32_t cp;
        i += tome_utf8_decode(s + i, len - i, &cp);
        ++count;
    }
    return count;
}

/*
 * The byte offset of code point number `index` (from 0) in `s`, or the
 * string's length in bytes if it has fewer. A code-point substring of s
 * from a to b is the bytes from tome_utf8_offset(s, a) to
 * tome_utf8_offset(s, b).
 */
static inline size_t tome_utf8_offset(const char* s, size_t index) {
    size_t len = tome_utf8_strlen_(s), i = 0;
    while (i < len && index > 0) {
        uint32_t cp;
        i += tome_utf8_decode(s + i, len - i, &cp);
        --index;
    }
    return i;
}

/* Copies `src` to `dst`, replacing invalid sequences with U+FFFD. */
static inline size_t tome_utf8_sanitize(char* dst, size_t size, const char* src) {
    size_t len = tome_utf8_strlen_(src), i = 0, out = 0;
    int full = 0;
    while (i < len) {
        char buf[4];
        uint32_t cp;
        size_t k, n;
        i += tome_utf8_decode(src + i, len - i, &cp);
        n = tome_utf8_encode(cp, buf);
        if (!full && size > 0 && out + n < size) {
            for (k = 0; k < n; ++k) dst[out + k] = buf[k];
        } else if (!full) {
            full = 1;
            if (size > 0) dst[out] = '\0';
        }
        out += n;
    }
    if (!full && size > 0) dst[out] = '\0';
    return out;
}

/* UTF-8 to UTF-16 (Windows' wchar_t), for W functions. Counts wchar_ts. */
static inline size_t tome_utf8_to_utf16(wchar_t* dst, size_t size, const char* src) {
    size_t len = tome_utf8_strlen_(src), i = 0, out = 0;
    int full = 0;
    while (i < len) {
        wchar_t buf[2];
        uint32_t cp;
        size_t k, n;
        i += tome_utf8_decode(src + i, len - i, &cp);
        if (cp >= 0x10000) {
            buf[0] = (wchar_t)(0xD800 + ((cp - 0x10000) >> 10));
            buf[1] = (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
            n = 2;
        } else {
            buf[0] = (wchar_t)cp;
            n = 1;
        }
        if (!full && size > 0 && out + n < size) {
            for (k = 0; k < n; ++k) dst[out + k] = buf[k];
        } else if (!full) {
            full = 1;
            if (size > 0) dst[out] = 0;
        }
        out += n;
    }
    if (!full && size > 0) dst[out] = 0;
    return out;
}

/* UTF-16 to UTF-8. Unpaired surrogates become U+FFFD. Counts bytes. */
static inline size_t tome_utf16_to_utf8(char* dst, size_t size, const wchar_t* src) {
    size_t i = 0, out = 0;
    int full = 0;
    while (src[i]) {
        uint32_t cp = (uint16_t)src[i++];
        char buf[4];
        size_t k, n;
        if (cp >= 0xD800 && cp <= 0xDBFF && (uint16_t)src[i] >= 0xDC00 && (uint16_t)src[i] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint16_t)src[i++] - 0xDC00);
        }
        n = tome_utf8_encode(cp, buf);  /* a lone surrogate becomes U+FFFD */
        if (!full && size > 0 && out + n < size) {
            for (k = 0; k < n; ++k) dst[out + k] = buf[k];
        } else if (!full) {
            full = 1;
            if (size > 0) dst[out] = '\0';
        }
        out += n;
    }
    if (!full && size > 0) dst[out] = '\0';
    return out;
}

#endif /* TOME_UTF8_H */
