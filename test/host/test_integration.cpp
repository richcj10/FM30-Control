/*
 * End-to-end test: the firmware's real Link + command validator talking to
 * tools/mock_stm32.py over a local TCP socket (in place of the UART).
 *
 *   python tools/mock_stm32.py --tcp 7031 &
 *   test_integration 7031
 *
 * tools/run_host_tests.py does both.
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "Commands.h"
#include "Link.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
static void closeSock(sock_t s) { closesocket(s); }
static void setNonBlocking(sock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
static const sock_t INVALID_SOCKET = -1;
static void closeSock(sock_t s) { close(s); }
static void setNonBlocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
#endif

using namespace fm30;

static int g_fail, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t nowMs()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

struct SocketIO : LinkIO {
    sock_t s = INVALID_SOCKET;
    char buf[2048];
    int len = 0, pos = 0;
    int readByte() override
    {
        if (pos >= len) {
            pos = 0;
            len = (int)recv(s, buf, sizeof buf, 0);
            if (len <= 0) { len = 0; return -1; }
        }
        return (unsigned char)buf[pos++];
    }
    void writeBytes(const char *d, size_t n) override { send(s, d, (int)n, 0); }
    uint32_t millis() override { return nowMs(); }
};

static bool connectMock(SocketIO &io, int port)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(0x7F000001);
        if (connect(s, (sockaddr *)&a, sizeof a) == 0) {
            setNonBlocking(s);
            io.s = s;
            return true;
        }
        closeSock(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

struct Done {
    bool done = false;
    Outcome outcome = Outcome::Timeout;
    std::string line, body;
};

// Runs one command to completion, like station::run() on the ESP.
static Done run(Link &link, const char *input)
{
    Command c;
    char err[64];
    Done d;
    if (!parseCommand(input, c, err, sizeof err)) {
        std::printf("FAIL validator rejected '%s': %s\n", input, err);
        g_fail++;
        return d;
    }
    link.submit(c, [&d](const Reply &r) {
        d.done = true;
        d.outcome = r.outcome;
        d.line = r.line;
        d.body = r.body;
    }, c.cmd != Cmd::Save);
    uint32_t t0 = nowMs();
    while (!d.done && nowMs() - t0 < 3000) {
        link.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return d;
}

static void pump(Link &link, uint32_t ms)
{
    uint32_t t0 = nowMs();
    while (nowMs() - t0 < ms) {
        link.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    int port = argc > 1 ? atoi(argv[1]) : 7031;
    SocketIO io;
    if (!connectMock(io, port)) {
        std::printf("can't connect to the mock on port %d\n", port);
        return 2;
    }
    Link link(io);
    int banners = 0, jsons = 0;
    std::string lastJson;
    link.onBanner = [&] { banners++; };
    link.onJson = [&](const char *j) { jsons++; lastJson = j; };
    link.onLog = [](const char *m) { std::printf("  link: %s\n", m); };

    pump(link, 300);
    CHECK(banners == 1);   // boot banner seen as an STM32 reset

    // Every command form the ESP can produce must get a matching OK.
    const char *ok[] = {
        "FREQ 98.1", "POWER 50%", "POWER 0", "POWER 3968", "TX ON", "TX OFF", "VOL 200", "BAL -10",
        "U6TRIM 5", "U6 1400", "U6 AUTO", "VTSCALE 3.0", "RDS ON", "RDSFREE OFF", "RDSLEVEL 400",
        "RDSPHASE -20", "PI C0DE", "PTY 10", "TP 1", "TA 0", "MS 1", "STEREO 1", "PS FM30 RDS", "RT Hello",
        "RT", "LCD ON", "LCD OFF", "LCD PAGE 0", "LCD ROTATE 1 5 6", "LCD ROTATE ALL", "LCD MSG 1 WiFi OK|10.0.0.7",
        "LCD MSG 2 just bottom", "LCD MSG 2", "STATUS", "STATUS 0", "DEFAULTS", "SAVE",
    };
    for (const char *in : ok) {
        Done d = run(link, in);
        if (d.outcome != Outcome::Ok) std::printf("FAIL '%s' -> %s\n", in, d.done ? d.line.c_str() : "(timeout)");
        CHECK(d.outcome == Outcome::Ok);
    }

    // HELP: 14 text lines, then OK.
    Done h = run(link, "HELP");
    int lines = 0;
    for (char c : h.body) lines += c == '\n';
    CHECK(h.outcome == Outcome::Ok && lines == 14);

    // Sanitized text lands as plain ASCII; the STATUS JSON shows it.
    run(link, "RT Bj\xC3\xB6rk \xE2\x80\x93 J\xC3\xB3ga | \"live\"");
    Done s = run(link, "STATUS");
    CHECK(s.outcome == Outcome::Ok);
    CHECK(s.line.find("\"rt\":\"Bjork - Joga | \\\"live\\\"\"") != std::string::npos);

    // Unsolicited STATUS lines arrive with no command outstanding.
    run(link, "STATUS 1");
    int before = jsons;
    pump(link, 2300);
    CHECK(jsons - before >= 2);
    run(link, "STATUS 0");

    // A burst of queued commands is paced one at a time: no retries, no
    // timeouts, no ring overflow on the mock side.
    Link::Stats st0 = link.stats();
    std::vector<Done> burst(Link::kQueueLen);
    for (size_t i = 0; i < burst.size(); i++) {
        Command c;
        char err[32];
        char in[32];
        snprintf(in, sizeof in, i % 2 ? "STATUS" : "LCD MSG 3 burst %u", (unsigned)i);
        parseCommand(in, c, err, sizeof err);
        Done *d = &burst[i];
        CHECK(link.submit(c, [d](const Reply &r) { d->done = true; d->outcome = r.outcome; }));
    }
    pump(link, 2000);
    for (const Done &d : burst) CHECK(d.done && d.outcome == Outcome::Ok);
    CHECK(link.stats().retries == st0.retries && link.stats().timeouts == st0.timeouts);
    CHECK(link.stats().stale == st0.stale);

    // An out-of-range value the validator lets through (none should exist)
    // would show up here as ERR; check one the STM32 itself rejects instead.
    Command bad;
    char err[32];
    parseCommand("TX ON", bad, err, sizeof err);
    strcpy(bad.line, "VOL 999");   // bypass validation on purpose
    Done e;
    link.submit(bad, [&e](const Reply &r) { e.done = true; e.outcome = r.outcome; e.line = r.line; });
    pump(link, 300);
    CHECK(e.outcome == Outcome::Err && e.line == "ERR 0-255");

    closeSock(io.s);
    std::printf("integration: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
