#include "Sanitize.h"
#include <stdint.h>
#include <string.h>

namespace fm30 {

namespace {

// U+00C0..U+00FF; the few two-letter cases are handled in translit().
const char kLatin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
                       "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
static_assert(sizeof kLatin1 == 65, "Latin-1 table must cover C0..FF");

// U+0100..U+017F (Latin Extended-A), base letter only.
const char kLatinA[] = "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh"
                       "IiIiIiIiIi" "Ii" "Jj" "Kkk" "LlLlLlLlLl" "NnNnNnn" "Nn"
                       "OoOoOo" "Oo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu"
                       "Ww" "YyY" "ZzZzZz" "s";
static_assert(sizeof kLatinA == 129, "Latin Extended-A table must cover 100..17F");

// Windows-1252 0x80..0x9F, used when a byte isn't valid UTF-8.
uint32_t cp1252(uint8_t b)
{
    static const uint16_t map[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    uint16_t u = map[b - 0x80];
    return u ? u : 0xFFFD;
}

// Decodes one code point; malformed UTF-8 is read as a single Windows-1252 byte.
uint32_t decode(const unsigned char *&p)
{
    unsigned char b = *p;
    int extra = (b >= 0xC2 && b <= 0xDF) ? 1 : (b >= 0xE0 && b <= 0xEF) ? 2
              : (b >= 0xF0 && b <= 0xF4) ? 3 : 0;
    if (b >= 0x80 && extra) {
        uint32_t cp = b & (0x3F >> extra);
        int i = 1;
        for (; i <= extra; i++) {
            if ((p[i] & 0xC0) != 0x80) break;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (i > extra) { p += extra + 1; return cp; }
    }
    p++;
    if (b < 0x80) return b;
    return b < 0xA0 ? cp1252(b) : b;  // stray Latin-1 byte
}

// Sentinels returned by translit(); compared by address.
const char kDrop[] = "";
const char kSpace[] = " ";

// Returns the ASCII replacement for a code point; kSpace for whitespace and
// control characters, kDrop for zero-width ones. Multi-letter results go in buf.
const char *translit(uint32_t cp, char *buf)
{
    buf[1] = 0;
    if (cp >= 0x20 && cp < 0x7F) { buf[0] = (char)cp; return buf; }
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || cp == 0xA0 ||
        (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F)
        return kSpace;
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200D) || cp == 0xFEFF)
        return kDrop;  // combining marks, zero-width chars, BOM
    switch (cp) {
    case 0xC6: return "AE";
    case 0xDE: return "Th";
    case 0xDF: return "ss";
    case 0xE6: return "ae";
    case 0xFE: return "th";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0x2122: return "TM";
    case 0x2026: return "...";
    case 0x20AC: return "EUR";
    case 0x0152: return "OE";
    case 0x0153: return "oe";
    case 0xB4: case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x2039: case 0x203A:
        buf[0] = '\''; return buf;
    case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: case 0x2033:
        buf[0] = '"'; return buf;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        buf[0] = '-'; return buf;
    case 0xB7: case 0x2022: buf[0] = '*'; return buf;
    case 0xD7: buf[0] = 'x'; return buf;
    case 0x02C6: buf[0] = '^'; return buf;
    case 0x02DC: buf[0] = '~'; return buf;
    case 0x0192: buf[0] = 'f'; return buf;
    }
    if (cp >= 0xC0 && cp <= 0xFF) buf[0] = kLatin1[cp - 0xC0];
    else if (cp >= 0x100 && cp <= 0x17F) buf[0] = kLatinA[cp - 0x100];
    else buf[0] = '?';
    return buf;
}

}  // namespace

size_t sanitizeText(const char *in, char *out, size_t maxChars, bool stripPipe)
{
    size_t n = 0;
    bool pendingSpace = false;   // collapse runs of control chars into one space
    const unsigned char *p = (const unsigned char *)(in ? in : "");
    char buf[2];
    while (*p && n < maxChars) {
        const char *r = translit(decode(p), buf);
        if (r == kDrop) continue;
        if (r == kSpace) { pendingSpace = true; continue; }
        if (pendingSpace) {
            if (n > 0 && out[n - 1] != ' ') out[n++] = ' ';
            pendingSpace = false;
        }
        for (; *r && n < maxChars; r++) {
            if (*r == '|' && stripPipe) continue;
            if (*r == ' ' && n == 0) continue;   // trim leading
            out[n++] = *r;
        }
    }
    while (n > 0 && out[n - 1] == ' ') n--;      // trim trailing
    out[n] = 0;
    return n;
}

}  // namespace fm30
