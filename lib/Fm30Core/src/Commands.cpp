#include "Commands.h"
#include "Sanitize.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace fm30 {

namespace {

constexpr size_t kMaxInput = 400;   // raw request text; RT may arrive as multi-byte UTF-8

bool fail(char *err, size_t errLen, const char *msg)
{
    if (err && errLen) snprintf(err, errLen, "%s", msg);
    return false;
}

bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    return *a == *b;
}

void trimRight(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
}

char *skipSpaces(char *s)
{
    while (*s == ' ') s++;
    return s;
}

// Splits "WORD rest" in place; returns rest (possibly "").
char *splitWord(char *s)
{
    char *sp = strchr(s, ' ');
    if (!sp) return s + strlen(s);
    *sp = 0;
    return skipSpaces(sp + 1);
}

// Optional sign, 1-9 decimal digits, nothing else.
bool parseLong(const char *s, long lo, long hi, long &v)
{
    bool neg = false;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    if (!*s) return false;
    long x = 0;
    int digits = 0;
    for (; *s; s++, digits++) {
        if (!isdigit((unsigned char)*s) || digits >= 9) return false;
        x = x * 10 + (*s - '0');
    }
    if (neg) x = -x;
    if (x < lo || x > hi) return false;
    v = x;
    return true;
}

// Unsigned decimal "98", "98.1", "3.0", ".5"; returns thousandths, rounded.
bool parseMilli(const char *s, long &milli)
{
    long ip = 0, frac = 0;
    int idigits = 0, fdigits = 0;
    for (; isdigit((unsigned char)*s); s++) {
        if (++idigits > 6) return false;
        ip = ip * 10 + (*s - '0');
    }
    if (*s == '.') {
        s++;
        for (; isdigit((unsigned char)*s); s++, fdigits++) {
            if (fdigits < 4) frac = frac * 10 + (*s - '0');
            if (fdigits > 12) return false;
        }
    }
    if (*s || idigits + fdigits == 0) return false;
    for (int i = fdigits < 4 ? fdigits : 4; i < 4; i++) frac *= 10;   // to 1/10000
    milli = ip * 1000 + (frac + 5) / 10;
    return true;
}

// 1 = on, 0 = off, -1 = invalid.
int parseOnOff(const char *s)
{
    if (ieq(s, "ON") || ieq(s, "1")) return 1;
    if (ieq(s, "OFF") || ieq(s, "0")) return 0;
    return -1;
}

bool parseHex16(const char *s, unsigned &v)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned x = 0;
    int n = 0;
    for (; *s; s++, n++) {
        if (!isxdigit((unsigned char)*s) || n >= 4) return false;
        x = x * 16 + (isdigit((unsigned char)*s) ? *s - '0' : (toupper((unsigned char)*s) - 'A' + 10));
    }
    if (!n) return false;
    v = x;
    return true;
}

void init(Command &out, Cmd cmd)
{
    memset(&out, 0, sizeof out);
    out.cmd = cmd;
}

bool setLine(Command &out, char *err, size_t errLen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out.line, sizeof out.line, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n > kMaxLine) return fail(err, errLen, "command too long");
    return true;
}

// Numeric command with one integer argument.
bool intCmd(Command &out, Cmd cmd, const char *name, const char *arg, long lo, long hi,
            char *err, size_t errLen)
{
    long v;
    init(out, cmd);
    if (!parseLong(arg, lo, hi, v)) {
        char msg[48];
        snprintf(msg, sizeof msg, "%s %ld..%ld", name, lo, hi);
        return fail(err, errLen, msg);
    }
    return setLine(out, err, errLen, "%s %ld", name, v);
}

bool onOffCmd(Command &out, Cmd cmd, const char *name, const char *arg, bool numeric,
              char *err, size_t errLen)
{
    init(out, cmd);
    int b = parseOnOff(arg);
    if (b < 0) {
        char msg[32];
        snprintf(msg, sizeof msg, numeric ? "%s 0|1" : "%s ON|OFF", name);
        return fail(err, errLen, msg);
    }
    if (numeric) return setLine(out, err, errLen, "%s %d", name, b);
    return setLine(out, err, errLen, "%s %s", name, b ? "ON" : "OFF");
}

bool noArgCmd(Command &out, Cmd cmd, const char *name, const char *arg, char *err, size_t errLen)
{
    init(out, cmd);
    if (*arg) {
        char msg[40];
        snprintf(msg, sizeof msg, "%s takes no arguments", name);
        return fail(err, errLen, msg);
    }
    return setLine(out, err, errLen, "%s", name);
}

bool lcdCmd(char *rest, Command &out, char *err, size_t errLen)
{
    char *sub = rest;
    char *arg = splitWord(sub);
    int b = parseOnOff(sub);
    if (b >= 0 && !*arg) {
        init(out, Cmd::LcdOnOff);
        return setLine(out, err, errLen, "LCD %s", b ? "ON" : "OFF");
    }
    if (ieq(sub, "PAGE")) {
        trimRight(arg);
        long v;
        init(out, Cmd::LcdPage);
        if (!parseLong(arg, 0, kLcdPages, v)) return fail(err, errLen, "LCD PAGE 0-8");
        return setLine(out, err, errLen, "LCD PAGE %ld", v);
    }
    if (ieq(sub, "MSG")) {
        // "<slot> [top]|<bottom>"; no '|' puts everything on the bottom row.
        char *text = arg;
        char slotStr[4] = {0};
        size_t i = 0;
        while (isdigit((unsigned char)*text) && i < sizeof slotStr - 1) slotStr[i++] = *text++;
        if (*text && *text != ' ') return fail(err, errLen, "LCD MSG 1-4 [top]|<bottom>");
        long slot;
        if (!parseLong(slotStr, 1, kLcdSlots, slot)) return fail(err, errLen, "LCD MSG 1-4 [top]|<bottom>");
        text = skipSpaces(text);
        char *bar = strchr(text, '|');
        if (bar) {
            *bar = 0;
            return buildLcdMsg((int)slot, text, bar + 1, out, err, errLen);
        }
        return buildLcdMsg((int)slot, "", text, out, err, errLen);
    }
    if (ieq(sub, "ROTATE")) {
        trimRight(arg);
        init(out, Cmd::LcdRotate);
        if (ieq(arg, "ALL")) {
            out.rotateMask = 0xFF;
            return setLine(out, err, errLen, "LCD ROTATE ALL");
        }
        unsigned mask = 0;
        char *p = arg;
        while (*p) {
            char tok[8];
            size_t n = 0;
            while (*p && *p != ' ' && *p != ',') {
                if (n >= sizeof tok - 1) return fail(err, errLen, "LCD ROTATE 1-8 ... | ALL");
                tok[n++] = *p++;
            }
            tok[n] = 0;
            while (*p == ' ' || *p == ',') p++;
            long page;
            if (!parseLong(tok, 1, kLcdPages, page)) return fail(err, errLen, "LCD ROTATE 1-8 ... | ALL");
            mask |= 1u << (page - 1);
        }
        if (!mask) return fail(err, errLen, "LCD ROTATE 1-8 ... | ALL");
        out.rotateMask = (uint8_t)mask;
        char list[2 * kLcdPages + 1], *q = list;
        for (int pg = 1; pg <= kLcdPages; pg++)
            if (mask & (1u << (pg - 1))) { *q++ = (char)('0' + pg); *q++ = ' '; }
        q[-1] = 0;
        return setLine(out, err, errLen, "LCD ROTATE %s", list);
    }
    init(out, Cmd::LcdOnOff);
    return fail(err, errLen, "LCD ON|OFF|PAGE n|MSG n text|ROTATE list");
}

}  // namespace

void buildPs(const char *text, Command &out)
{
    init(out, Cmd::Ps);
    char ps[kPsLen + 1];
    sanitizeText(text, ps, kPsLen, false);
    snprintf(out.line, sizeof out.line, "PS %s", ps);
}

void buildRt(const char *text, Command &out)
{
    init(out, Cmd::Rt);
    char rt[kRtLen + 1];
    sanitizeText(text, rt, kRtLen, false);
    snprintf(out.line, sizeof out.line, rt[0] ? "RT %s" : "RT", rt);
}

bool buildLcdMsg(int slot, const char *top, const char *bottom, Command &out, char *err, size_t errLen)
{
    init(out, Cmd::LcdMsg);
    if (slot < 1 || slot > kLcdSlots) return fail(err, errLen, "LCD MSG slot 1-4");
    out.lcdSlot = slot;
    sanitizeText(top, out.lcdTop, kLcdTopLen, true);
    sanitizeText(bottom, out.lcdBottom, kLcdBottomLen, true);
    if (!out.lcdTop[0] && !out.lcdBottom[0])
        return setLine(out, err, errLen, "LCD MSG %d", slot);
    if (!out.lcdTop[0])
        return setLine(out, err, errLen, "LCD MSG %d %s", slot, out.lcdBottom);
    return setLine(out, err, errLen, "LCD MSG %d %s|%s", slot, out.lcdTop, out.lcdBottom);
}

bool buildPowerPercent(int percent, Command &out, char *err, size_t errLen)
{
    init(out, Cmd::Power);
    if (percent < 0 || percent > 100) return fail(err, errLen, "POWER 0-100%");
    return setLine(out, err, errLen, "POWER %d%%", percent);
}

bool buildFreq(const char *mhz, Command &out, char *err, size_t errLen)
{
    init(out, Cmd::Freq);
    long milli;
    if (!parseMilli(mhz, milli)) return fail(err, errLen, "FREQ 87.5-108.0");
    long f10 = (milli + 50) / 100;
    if (f10 < 875 || f10 > 1080) return fail(err, errLen, "FREQ 87.5-108.0");
    return setLine(out, err, errLen, "FREQ %ld.%ld", f10 / 10, f10 % 10);
}

bool parseCommand(const char *input, Command &out, char *err, size_t errLen)
{
    init(out, Cmd::Help);
    if (!input) return fail(err, errLen, "empty command");
    size_t len = strlen(input);
    if (len > kMaxInput) return fail(err, errLen, "command too long");
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c < 0x20 || c == 0x7F) return fail(err, errLen, "control characters (CR/LF/TAB) not allowed");
    }

    char buf[kMaxInput + 1];
    memcpy(buf, input, len + 1);
    char *cmd = skipSpaces(buf);
    if (!*cmd) return fail(err, errLen, "empty command");
    char *rest = splitWord(cmd);
    for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

    // Text commands keep their argument as-is (the sanitizer trims it);
    // every other argument is trimmed here.
    if (!strcmp(cmd, "PS")) { buildPs(rest, out); return true; }
    if (!strcmp(cmd, "RT")) { buildRt(rest, out); return true; }
    if (!strcmp(cmd, "LCD")) return lcdCmd(rest, out, err, errLen);
    trimRight(rest);

    if (!strcmp(cmd, "FREQ")) return buildFreq(rest, out, err, errLen);
    if (!strcmp(cmd, "POWER")) {
        size_t n = strlen(rest);
        if (n && rest[n - 1] == '%') {
            rest[n - 1] = 0;
            long pct;
            if (!parseLong(rest, 0, 100, pct)) { init(out, Cmd::Power); return fail(err, errLen, "POWER 0-100% or 0-3968"); }
            return buildPowerPercent((int)pct, out, err, errLen);
        }
        init(out, Cmd::Power);
        long v;
        if (!parseLong(rest, 0, kPowerMax, v)) return fail(err, errLen, "POWER 0-100% or 0-3968");
        return setLine(out, err, errLen, "POWER %ld", v);
    }
    if (!strcmp(cmd, "TX")) return onOffCmd(out, Cmd::Tx, "TX", rest, false, err, errLen);
    if (!strcmp(cmd, "VOL")) return intCmd(out, Cmd::Vol, "VOL", rest, 0, 255, err, errLen);
    if (!strcmp(cmd, "BAL")) return intCmd(out, Cmd::Bal, "BAL", rest, -127, 127, err, errLen);
    if (!strcmp(cmd, "U6TRIM")) return intCmd(out, Cmd::U6Trim, "U6TRIM", rest, -1000, 1000, err, errLen);
    if (!strcmp(cmd, "U6")) {
        if (ieq(rest, "AUTO")) { init(out, Cmd::U6); return setLine(out, err, errLen, "U6 AUTO"); }
        return intCmd(out, Cmd::U6, "U6", rest, 0, 4095, err, errLen);
    }
    if (!strcmp(cmd, "VTSCALE")) {
        init(out, Cmd::VtScale);
        long milli;
        if (!parseMilli(rest, milli) || milli < 1000 || milli > 10000) return fail(err, errLen, "VTSCALE 1.0-10.0");
        return setLine(out, err, errLen, "VTSCALE %ld.%03ld", milli / 1000, milli % 1000);
    }
    if (!strcmp(cmd, "RDS")) return onOffCmd(out, Cmd::Rds, "RDS", rest, false, err, errLen);
    if (!strcmp(cmd, "RDSFREE")) return onOffCmd(out, Cmd::RdsFree, "RDSFREE", rest, false, err, errLen);
    if (!strcmp(cmd, "RDSLEVEL")) return intCmd(out, Cmd::RdsLevel, "RDSLEVEL", rest, 0, 2047, err, errLen);
    if (!strcmp(cmd, "RDSPHASE")) return intCmd(out, Cmd::RdsPhase, "RDSPHASE", rest, -180, 180, err, errLen);
    if (!strcmp(cmd, "PI")) {
        init(out, Cmd::Pi);
        unsigned v;
        if (!parseHex16(rest, v)) return fail(err, errLen, "PI hex 0000-FFFF");
        return setLine(out, err, errLen, "PI %04X", v);
    }
    if (!strcmp(cmd, "PTY")) return intCmd(out, Cmd::Pty, "PTY", rest, 0, 31, err, errLen);
    if (!strcmp(cmd, "TP") || !strcmp(cmd, "TA") || !strcmp(cmd, "MS") || !strcmp(cmd, "STEREO"))
        return onOffCmd(out, Cmd::Flag, cmd, rest, true, err, errLen);
    if (!strcmp(cmd, "STATUS")) {
        init(out, Cmd::Status);
        if (!*rest) return setLine(out, err, errLen, "STATUS");
        long v;
        if (!parseLong(rest, 0, 3600, v)) return fail(err, errLen, "STATUS [0-3600]");
        return setLine(out, err, errLen, "STATUS %ld", v);
    }
    if (!strcmp(cmd, "SAVE")) return noArgCmd(out, Cmd::Save, "SAVE", rest, err, errLen);
    if (!strcmp(cmd, "DEFAULTS")) return noArgCmd(out, Cmd::Defaults, "DEFAULTS", rest, err, errLen);
    if (!strcmp(cmd, "HELP") || !strcmp(cmd, "?")) return noArgCmd(out, Cmd::Help, "HELP", rest, err, errLen);
    return fail(err, errLen, "unknown command (HELP)");
}

bool isProtected(Cmd cmd)
{
    switch (cmd) {
    case Cmd::Freq: case Cmd::Power: case Cmd::U6Trim: case Cmd::U6: case Cmd::VtScale:
    case Cmd::RdsLevel: case Cmd::Defaults:
        return true;
    default:
        return false;
    }
}

const char *expectedReplyWord(const char *line, char *buf, size_t bufLen)
{
    size_t n = 0;
    while (line[n] && line[n] != ' ' && n + 1 < bufLen) {
        buf[n] = (char)toupper((unsigned char)line[n]);
        n++;
    }
    buf[n] = 0;
    if (!strcmp(buf, "STATUS")) return nullptr;
    if (!strcmp(buf, "SAVE")) return "SAVED";
    if (!strcmp(buf, "VTSCALE") || !strcmp(buf, "HELP") || !strcmp(buf, "?")) return "";
    return buf;
}

}  // namespace fm30
