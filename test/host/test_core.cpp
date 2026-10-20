/*
 * Host tests for lib/Fm30Core (sanitizer, command validation, link queue).
 * Plain C++17, no framework, builds with g++/clang++ or MSVC:
 *
 *   python tools/run_host_tests.py
 *
 * or by hand:
 *   g++ -std=c++17 -Wall -Ilib/Fm30Core/src test/host/test_core.cpp lib/Fm30Core/src/*.cpp -o test_core
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Commands.h"
#include "Link.h"
#include "Sanitize.h"

using namespace fm30;

static int g_fail, g_checks;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; std::string _a = (a), _b = (b); if (_a != _b) { g_fail++; \
    std::printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); } } while (0)

/* ---------------- sanitizer ---------------- */

static std::string san(const char *in, size_t max = 64, bool pipe = false)
{
    char out[128];
    sanitizeText(in, out, max, pipe);
    return out;
}

static void test_sanitize()
{
    CHECK_STR(san("Bj\xC3\xB6rk \xE2\x80\x93 J\xC3\xB3ga"), "Bjork - Joga");
    CHECK_STR(san("line1\r\nline2"), "line1 line2");
    CHECK_STR(san("tab\there"), "tab here");
    CHECK_STR(san("a|b", 64, true), "ab");
    CHECK_STR(san("a|b", 64, false), "a|b");
    CHECK_STR(san("  ABCDEFGHIJ  ", 8), "ABCDEFGH");
    CHECK_STR(san("ABCDEFG HIJ", 8), "ABCDEFG");
    CHECK_STR(san("\x93hi\x94"), "\"hi\"");                    // Windows-1252 quotes
    CHECK_STR(san("\xF0\x9F\x8E\xB5 x"), "? x");               // emoji
    CHECK_STR(san("e\xCC\x81"), "e");                          // combining accent
    CHECK_STR(san("\xC5\x81\xC3\xB3" "d\xC5\xBA"), "Lodz");    // Lodz, Latin Extended-A
    CHECK_STR(san("Stra\xC3\x9F" "e"), "Strasse");
    CHECK_STR(san("Caf\xE9"), "Cafe");                         // stray Latin-1 byte
    CHECK_STR(san("wait\xE2\x80\xA6"), "wait...");
    CHECK_STR(san("\x01\x02\x7F"), "");
    CHECK_STR(san(""), "");
    CHECK_STR(san(nullptr), "");
    CHECK_STR(san("\xC3"), "A");                               // truncated UTF-8 -> Latin-1 byte
}

/* ---------------- command validation ---------------- */

static std::string cmd(const char *in)
{
    Command c;
    char err[64];
    if (!parseCommand(in, c, err, sizeof err)) return std::string("!") + err;
    return c.line;
}

static void test_commands()
{
    CHECK_STR(cmd("freq 98.1"), "FREQ 98.1");
    CHECK_STR(cmd("  FREQ   88  "), "FREQ 88.0");
    CHECK_STR(cmd("FREQ 108.04"), "FREQ 108.0");
    CHECK(cmd("FREQ 108.05")[0] == '!');
    CHECK(cmd("FREQ 87.4")[0] == '!');
    CHECK(cmd("FREQ 1e2")[0] == '!');
    CHECK(cmd("FREQ")[0] == '!');
    CHECK(cmd("FREQ -98.1")[0] == '!');
    CHECK(cmd("FREQ 98.1\nSAVE")[0] == '!');
    CHECK(cmd("FREQ 98.1\rSAVE")[0] == '!');

    CHECK_STR(cmd("power 50%"), "POWER 50%");
    CHECK_STR(cmd("POWER 100%"), "POWER 100%");
    CHECK_STR(cmd("POWER 080"), "POWER 80");                   // never forwarded as octal
    CHECK_STR(cmd("POWER 3968"), "POWER 3968");
    CHECK(cmd("POWER 101%")[0] == '!');
    CHECK(cmd("POWER 3969")[0] == '!');
    CHECK(cmd("POWER -1")[0] == '!');
    CHECK(cmd("POWER 0x10")[0] == '!');
    CHECK(cmd("POWER 50.5%")[0] == '!');
    CHECK(cmd("POWER")[0] == '!');

    CHECK_STR(cmd("tx on"), "TX ON");
    CHECK_STR(cmd("TX 0"), "TX OFF");
    CHECK(cmd("TX maybe")[0] == '!');
    CHECK_STR(cmd("VOL 255"), "VOL 255");
    CHECK(cmd("VOL 256")[0] == '!');
    CHECK_STR(cmd("BAL -127"), "BAL -127");
    CHECK(cmd("BAL 128")[0] == '!');
    CHECK_STR(cmd("u6 auto"), "U6 AUTO");
    CHECK(cmd("U6 4096")[0] == '!');
    CHECK_STR(cmd("U6TRIM -20"), "U6TRIM -20");
    CHECK_STR(cmd("VTSCALE 3"), "VTSCALE 3.000");
    CHECK_STR(cmd("VTSCALE 2.9876"), "VTSCALE 2.988");
    CHECK(cmd("VTSCALE 0.5")[0] == '!');

    CHECK_STR(cmd("RDS off"), "RDS OFF");
    CHECK_STR(cmd("RDSFREE 1"), "RDSFREE ON");
    CHECK_STR(cmd("RDSLEVEL 400"), "RDSLEVEL 400");
    CHECK(cmd("RDSLEVEL 2048")[0] == '!');
    CHECK_STR(cmd("RDSPHASE -180"), "RDSPHASE -180");
    CHECK_STR(cmd("pi c0de"), "PI C0DE");
    CHECK_STR(cmd("PI 0x1"), "PI 0001");
    CHECK(cmd("PI 12345")[0] == '!');
    CHECK(cmd("PI xyz")[0] == '!');
    CHECK_STR(cmd("PTY 31"), "PTY 31");
    CHECK(cmd("PTY 32")[0] == '!');
    CHECK_STR(cmd("tp on"), "TP 1");
    CHECK_STR(cmd("STEREO 0"), "STEREO 0");

    CHECK_STR(cmd("PS   My Station Name"), "PS My Stati");
    CHECK_STR(cmd("RT"), "RT");
    CHECK_STR(cmd("rt Now playing: Bj\xC3\xB6rk"), "RT Now playing: Bjork");
    std::string longRt = "RT " + std::string(100, 'x');
    CHECK_STR(cmd(longRt.c_str()), "RT " + std::string(64, 'x'));

    CHECK_STR(cmd("LCD MSG 2 Hi|there"), "LCD MSG 2 Hi|there");
    CHECK_STR(cmd("lcd msg 3 just bottom"), "LCD MSG 3 just bottom");
    CHECK_STR(cmd("LCD MSG 4"), "LCD MSG 4");
    CHECK_STR(cmd("LCD MSG 2 TopTooLong|x"), "LCD MSG 2 TopTooLo|x");
    CHECK_STR(cmd("LCD MSG 2 a|b|c"), "LCD MSG 2 a|bc");
    CHECK_STR(cmd("LCD MSG 2 |bottom only"), "LCD MSG 2 bottom only");
    CHECK(cmd("LCD MSG 5 x")[0] == '!');
    CHECK(cmd("LCD MSG 2x")[0] == '!');
    std::string longBottom = "LCD MSG 2 top|" + std::string(60, 'b');
    CHECK_STR(cmd(longBottom.c_str()), "LCD MSG 2 top|" + std::string(48, 'b'));
    CHECK_STR(cmd("LCD ROTATE 6,1 5"), "LCD ROTATE 1 5 6");
    CHECK_STR(cmd("lcd rotate all"), "LCD ROTATE ALL");
    CHECK(cmd("LCD ROTATE 15")[0] == '!');
    CHECK(cmd("LCD ROTATE 0")[0] == '!');
    CHECK_STR(cmd("LCD PAGE 0"), "LCD PAGE 0");
    CHECK(cmd("LCD PAGE 9")[0] == '!');
    CHECK_STR(cmd("lcd off"), "LCD OFF");
    CHECK(cmd("LCD")[0] == '!');

    CHECK_STR(cmd("status"), "STATUS");
    CHECK_STR(cmd("STATUS 2"), "STATUS 2");
    CHECK(cmd("STATUS 99999")[0] == '!');
    CHECK_STR(cmd("save"), "SAVE");
    CHECK(cmd("SAVE now")[0] == '!');
    CHECK_STR(cmd("?"), "HELP");
    CHECK_STR(cmd("DEFAULTS"), "DEFAULTS");
    CHECK_STR(cmd("dacdbg"), "DACDBG");
    CHECK(cmd("DACDBG 1")[0] == '!');
    CHECK_STR(cmd("DACTEST 2"), "DACTEST 2");
    CHECK(cmd("DACTEST 3")[0] == '!');
    CHECK(cmd("BOGUS 1")[0] == '!');
    CHECK(cmd("")[0] == '!');

    Command c;
    char err[64];
    CHECK(parseCommand("LCD MSG 3 Top|Bottom", c, err, sizeof err));
    CHECK(c.cmd == Cmd::LcdMsg && c.lcdSlot == 3);
    CHECK_STR(c.lcdTop, "Top");
    CHECK_STR(c.lcdBottom, "Bottom");
    CHECK(parseCommand("LCD ROTATE 1 5", c, err, sizeof err) && c.rotateMask == 0x11);
    CHECK(!buildPowerPercent(101, c, err, sizeof err));
    CHECK(buildPowerPercent(0, c, err, sizeof err));
    CHECK_STR(c.line, "POWER 0%");

    CHECK(isProtected(Cmd::Freq) && isProtected(Cmd::Power) && isProtected(Cmd::VtScale) &&
          isProtected(Cmd::U6) && isProtected(Cmd::U6Trim) && isProtected(Cmd::RdsLevel) && isProtected(Cmd::Defaults) &&
          isProtected(Cmd::DacTest));
    CHECK(!isProtected(Cmd::Tx) && !isProtected(Cmd::Rt) && !isProtected(Cmd::LcdMsg) && !isProtected(Cmd::Save) &&
          !isProtected(Cmd::Vol) && !isProtected(Cmd::Status) && !isProtected(Cmd::DacDbg));

    char buf[12];
    CHECK_STR(expectedReplyWord("FREQ 98.1", buf, sizeof buf), "FREQ");
    CHECK_STR(expectedReplyWord("SAVE", buf, sizeof buf), "SAVED");
    CHECK_STR(expectedReplyWord("VTSCALE 3.000", buf, sizeof buf), "");
    CHECK_STR(expectedReplyWord("DACDBG", buf, sizeof buf), "");
    CHECK_STR(expectedReplyWord("DACTEST 1", buf, sizeof buf), "DACTEST");
    CHECK(expectedReplyWord("STATUS 2", buf, sizeof buf) == nullptr);
}

/* ---------------- link queue ---------------- */

struct FakeIO : LinkIO {
    std::string rx;      // bytes the "STM32" has sent us
    size_t pos = 0;
    std::string tx;      // bytes we sent
    uint32_t t = 1000;
    int readByte() override { return pos < rx.size() ? (unsigned char)rx[pos++] : -1; }
    void writeBytes(const char *d, size_t n) override { tx.append(d, n); }
    uint32_t millis() override { return t; }
    void stm32(const std::string &line) { rx += line + "\r\n"; }
    // Lines sent since the last call.
    std::vector<std::string> sent()
    {
        std::vector<std::string> v;
        size_t s = 0, e;
        while ((e = tx.find('\n', s)) != std::string::npos) { v.push_back(tx.substr(s, e - s)); s = e + 1; }
        tx.erase(0, s);
        return v;
    }
};

struct Result {
    int calls = 0;
    Outcome outcome = Outcome::Timeout;
    std::string line, body;
    Link::Callback cb()
    {
        return [this](const Reply &r) { calls++; outcome = r.outcome; line = r.line; body = r.body; };
    }
};

static void advance(Link &l, FakeIO &io, uint32_t ms, uint32_t step = 5)
{
    for (uint32_t i = 0; i < ms; i += step) { io.t += step; l.poll(); }
}

static void test_link_basic()
{
    FakeIO io;
    Link l(io);
    Result a, b, c;
    CHECK(l.submitLine("FREQ 98.1", a.cb()));
    CHECK(l.submitLine("VOL 10", b.cb()));
    CHECK(l.submitLine("STATUS", c.cb()));
    l.poll();
    auto s = io.sent();
    CHECK(s.size() == 1 && s[0] == "FREQ 98.1");      // only one outstanding
    advance(l, io, 100);
    CHECK(io.sent().empty());                         // still waiting
    io.stm32("OK FREQ 98.1");
    l.poll();
    CHECK(a.calls == 1 && a.outcome == Outcome::Ok);
    CHECK_STR(a.line, "OK FREQ 98.1");
    CHECK(io.sent().empty());                         // gap before the next one
    advance(l, io, 10);
    s = io.sent();
    CHECK(s.size() == 1 && s[0] == "VOL 10");
    io.stm32("ERR 0-255");
    advance(l, io, 10);
    CHECK(b.outcome == Outcome::Err);
    s = io.sent();
    CHECK(s.size() == 1 && s[0] == "STATUS");
    CHECK(l.statusPending());
    io.stm32("{\"fw\":\"fm30ctl 0.3\",\"uptime\":5}");
    l.poll();
    CHECK(c.calls == 1 && c.outcome == Outcome::Ok && c.line[0] == '{');
    CHECK(l.idle() && !l.statusPending());
}

static void test_link_retry_and_timeout()
{
    FakeIO io;
    Link l(io);
    Result a;
    l.submitLine("TX ON", a.cb());
    l.poll();
    CHECK(io.sent().size() == 1);
    advance(l, io, 495);
    CHECK(io.sent().empty());
    advance(l, io, 10);
    auto s = io.sent();
    CHECK(s.size() == 1 && s[0] == "TX ON");          // one retry
    CHECK(l.stats().retries == 1);
    io.stm32("OK TX 1");
    l.poll();
    CHECK(a.outcome == Outcome::Ok);

    Result b, c;
    l.submitLine("TX OFF", b.cb());
    l.submitLine("VOL 5", c.cb());
    advance(l, io, 1100);
    CHECK(b.calls == 1 && b.outcome == Outcome::Timeout);
    s = io.sent();
    CHECK(s.size() == 3 && s[0] == "TX OFF" && s[1] == "TX OFF" && s[2] == "VOL 5");

    // SAVE is sent without retry.
    FakeIO io2;
    Link l2(io2);
    Result d;
    l2.submitLine("SAVE", d.cb(), false);
    advance(l2, io2, 1200);
    CHECK(io2.sent().size() == 1);
    CHECK(d.outcome == Outcome::Timeout);
}

static void test_link_stale_reply()
{
    FakeIO io;
    Link l(io);
    Result a, b;
    l.submitLine("FREQ 98.1", a.cb());
    l.submitLine("VOL 10", b.cb());
    advance(l, io, 1020);                             // both FREQ attempts time out
    CHECK(a.outcome == Outcome::Timeout);
    io.sent();
    io.stm32("OK FREQ 98.1");                         // late reply to the old command
    l.poll();
    CHECK(b.calls == 0);                              // not mistaken for VOL's reply
    CHECK(l.stats().stale == 1);
    io.stm32("OK VOL 10");
    l.poll();
    CHECK(b.calls == 1 && b.outcome == Outcome::Ok);

    Result c;
    l.submitLine("VTSCALE 3.000", c.cb());
    advance(l, io, 10);
    io.stm32("OK LCD MSG 1");                         // wrong echo
    l.poll();
    CHECK(c.calls == 0);
    io.stm32("OK");                                   // VTSCALE answers a bare OK
    l.poll();
    CHECK(c.calls == 1 && c.outcome == Outcome::Ok);
}

static void test_link_unsolicited()
{
    FakeIO io;
    Link l(io);
    int json = 0, banner = 0;
    l.onJson = [&](const char *) { json++; };
    l.onBanner = [&] { banner++; };
    Result a;
    l.submitLine("PS MYSTN", a.cb());
    l.poll();
    io.sent();
    io.stm32("{\"uptime\":10}");                      // STATUS n output while waiting
    l.poll();
    CHECK(json == 1 && a.calls == 0);
    io.stm32("OK PS \"MYSTN   \"");
    l.poll();
    CHECK(a.outcome == Outcome::Ok);

    // Reset while a command is in flight: banner is reported and the command
    // is re-sent shortly after.
    Result b;
    l.submitLine("RT hello", b.cb());
    advance(l, io, 10);
    CHECK(io.sent().size() == 1);
    io.rx += "\r\nfm30ctl 0.3 ready\r\nsettings: flash\r\nclock: HSE 24 MHz\r\n";
    l.poll();
    CHECK(banner == 1);
    advance(l, io, 120);
    auto s = io.sent();
    CHECK(s.size() == 1 && s[0] == "RT hello");
    io.stm32("OK RT");
    l.poll();
    CHECK(b.outcome == Outcome::Ok);

    // HELP collects its text lines and ends with OK.
    Result h;
    l.submitLine("HELP", h.cb());
    advance(l, io, 10);
    io.stm32("FREQ <MHz>   e.g. FREQ 98.1");
    io.stm32("SAVE  DEFAULTS  HELP");
    io.stm32("OK");
    l.poll();
    CHECK(h.outcome == Outcome::Ok);
    CHECK_STR(h.body, "FREQ <MHz>   e.g. FREQ 98.1\nSAVE  DEFAULTS  HELP\n");
}

static void test_link_limits()
{
    FakeIO io;
    Link l(io);
    int json = 0;
    l.onJson = [&](const char *) { json++; };
    io.rx += "{" + std::string(1100, 'x') + "}\r\n";  // over-long line is dropped whole
    io.stm32("{\"ok\":1}");
    l.poll();
    CHECK(json == 1 && l.stats().overflows == 1);

    std::string big = "{\"rt\":\"" + std::string(600, 'r') + "\"}";   // ~620 B STATUS fits
    io.stm32(big);
    l.poll();
    CHECK(json == 2);

    for (size_t i = 0; i < Link::kQueueLen; i++) CHECK(l.submitLine("VOL 1"));
    CHECK(!l.submitLine("VOL 1"));                     // full
    CHECK(!l.submitLine(""));
    CHECK(!l.submitLine("VOL 1\nSAVE"));
    CHECK(!l.submitLine(std::string(130, 'A').c_str()));
}

int main()
{
    test_sanitize();
    test_commands();
    test_link_basic();
    test_link_retry_and_timeout();
    test_link_stale_reply();
    test_link_unsolicited();
    test_link_limits();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
