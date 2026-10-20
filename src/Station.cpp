#include "Station.h"
#include <ArduinoJson.h>
#include <memory>
#include "AppConfig.h"
#include "Log.h"

using namespace fm30;

namespace station {

namespace {

constexpr uint32_t kPollMs = 2000;          // STATUS poll period
constexpr uint32_t kLinkTimeoutMs = 6000;   // nothing heard for this long = link down
constexpr uint32_t kSaveMinGapMs = 10000;   // SAVE rate limit (STM32 flash wear)
constexpr uint32_t kConfigSaveDelayMs = 5000;
constexpr uint32_t kBannerResyncMs = 300;   // let the STM32 finish booting first

class SerialIO : public LinkIO {
public:
    int readByte() override { return Serial.read(); }
    void writeBytes(const char *d, size_t n) override { Serial.write((const uint8_t *)d, n); }
    uint32_t millis() override { return ::millis(); }
};

SerialIO io;
Link link(io);

String status;
uint32_t statusAt;
bool haveStatus_;
uint32_t lastUptime;
bool haveUptime;
uint32_t resets;

bool resyncPending;
uint32_t resyncAt;
int rotMask = -1, page = -1, lcdEnabled = -1;

uint32_t lastPoll, lastSaveAt;
bool savedOnce;
bool configDirty;
uint32_t configDirtyAt;
bool wasUp;

std::function<void(const String &)> statusCb;
std::function<void(bool)> linkCb;

void scheduleResync(uint32_t delayMs)
{
    resyncPending = true;
    resyncAt = millis() + delayMs;
}

// The STM32 rebooted: its LCD state is back to whatever was last SAVEd, and
// the message slots are empty.
void forgetStm32State()
{
    rotMask = page = lcdEnabled = -1;
}

void handleBanner()
{
    resets++;
    haveUptime = false;     // the next uptime is small; don't count this reset twice
    forgetStm32State();
    scheduleResync(kBannerResyncMs);
}

void handleJson(const char *line)
{
    JsonDocument filter;
    filter["uptime"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, line, DeserializationOption::Filter(filter));
    if (e) {
        LOGF("STATUS: bad JSON (%s)", e.c_str());
        return;
    }
    if (!doc["uptime"].is<uint32_t>()) {
        LOGF("STATUS: no uptime field");
        return;
    }
    uint32_t up = doc["uptime"];
    if (!haveStatus_) {
        LOGF("STM32 link up, uptime %lu s", (unsigned long)up);
        scheduleResync(0);   // the ESP just booted: push our LCD messages
    } else if (haveUptime && up < lastUptime) {
        // (also fires when the STM32's 49-day tick counter wraps; harmless)
        resets++;
        LOGF("STM32 reset detected (uptime %lu -> %lu)", (unsigned long)lastUptime, (unsigned long)up);
        forgetStm32State();
        scheduleResync(0);
    }
    lastUptime = up;
    haveUptime = true;
    status = line;
    statusAt = millis();
    haveStatus_ = true;
    if (statusCb) statusCb(status);
}

void resync()
{
    for (int slot = 1; slot <= kLcdSlots; slot++) {
        Command c;
        char err[48];
        if (!buildLcdMsg(slot, config.lcd[slot - 1].top.c_str(), config.lcd[slot - 1].bottom.c_str(), c, err, sizeof err)) {
            LOGF("resync slot %d: %s", slot, err);
            continue;
        }
        if (!link.submit(c)) {
            scheduleResync(1000);   // queue full; try the whole set again later
            return;
        }
    }
    resyncPending = false;
    LOGF("LCD messages sent to STM32");
}

void applySideEffects(const Command &c, const Reply &r)
{
    if (r.outcome != Outcome::Ok) return;
    switch (c.cmd) {
    case Cmd::LcdMsg:
        config.lcd[c.lcdSlot - 1].top = c.lcdTop;
        config.lcd[c.lcdSlot - 1].bottom = c.lcdBottom;
        if (c.lcdSlot > 1) {
            configDirty = true;
            configDirtyAt = millis();
        }
        break;
    case Cmd::LcdRotate: rotMask = c.rotateMask; break;
    case Cmd::LcdPage: page = atoi(c.line + 9); break;          // "LCD PAGE n"
    case Cmd::LcdOnOff: lcdEnabled = !strcmp(c.line, "LCD ON"); break;
    case Cmd::Defaults: rotMask = 0xFF; page = 0; lcdEnabled = 1; break;
    default: break;
    }
}

}  // namespace

void begin()
{
    Serial.setRxBufferSize(1024);   // a STATUS line is ~620 bytes
    Serial.begin(115200);
    Serial.println(F("\r\nfm30-esp " FW_VERSION ": UART0 moving to GPIO13 (RX) / GPIO15 (TX); log on GPIO2"));
    Serial.flush();
    Serial.swap();
    while (Serial.available()) Serial.read();
    // Terminate any partial line the STM32 may have collected from noise.
    Serial.write('\n');

    link.onJson = handleJson;
    link.onBanner = handleBanner;
    link.onLog = [](const char *m) { LOGF("link: %s", m); };
    lastPoll = millis() - kPollMs + 200;   // first poll soon after boot
}

void loop()
{
    link.poll();
    uint32_t now = millis();
    if (now - lastPoll >= kPollMs) {
        lastPoll = now;
        if (!link.statusPending()) link.submitLine("STATUS", nullptr, false);
    }
    if (resyncPending && (int32_t)(now - resyncAt) >= 0) resync();

    bool up = linkUp();
    if (up != wasUp) {
        wasUp = up;
        LOGF("STM32 link %s", up ? "up" : "DOWN");
        // Back after an outage with no reset seen (e.g. a loose wire): anything
        // sent meanwhile, such as a WiFi status change, was lost.
        if (up && haveStatus_) scheduleResync(0);
        if (linkCb) linkCb(up);
    }
    if (configDirty && now - configDirtyAt >= kConfigSaveDelayMs) {
        configDirty = false;
        configSave();
    }
}

bool allowedFromUser(const Command &c, String &err)
{
    if (c.cmd == Cmd::LcdMsg && c.lcdSlot == 1) {
        err = F("LCD MSG slot 1 is reserved for the WiFi status; use 2-4");
        return false;
    }
    if (c.cmd == Cmd::Save && savedOnce && millis() - lastSaveAt < kSaveMinGapMs) {
        err = F("SAVE rate-limited: wait 10 s between saves (flash wear)");
        return false;
    }
    return true;
}

bool submit(const Command &c, Link::Callback cb)
{
    if (c.cmd == Cmd::Save) {
        savedOnce = true;
        lastSaveAt = millis();
    }
    bool retry = c.cmd != Cmd::Save;   // a retried SAVE could write flash twice
    return link.submit(c, [c, cb](const Reply &r) {
        applySideEffects(c, r);
        if (cb) cb(r);
    }, retry);
}

SyncResult run(const Command &c)
{
    struct State {
        bool done = false;
        SyncResult res;
    };
    // Shared so a reply arriving after we give up can't write to a dead stack frame.
    auto st = std::make_shared<State>();
    st->res.outcome = Outcome::Timeout;
    st->res.queued = submit(c, [st](const Reply &r) {
        st->res.outcome = r.outcome;
        st->res.line = r.line;
        st->res.body = r.body;
        st->done = true;
    });
    if (!st->res.queued) return st->res;
    uint32_t t0 = millis();
    // Worst case: a full queue ahead of us, each command timing out twice.
    const uint32_t limit = (Link::kQueueLen + 1) * (2 * Link::kTimeoutMs + 20);
    while (!st->done && millis() - t0 < limit) {
        link.poll();
        yield();
    }
    return st->res;
}

bool haveStatus() { return haveStatus_; }
const String &statusJson() { return status; }
uint32_t statusAgeMs() { return haveStatus_ ? millis() - statusAt : UINT32_MAX; }
bool linkUp() { return link.everReceived() && millis() - link.lastRxMs() < kLinkTimeoutMs; }
uint32_t stm32Resets() { return resets; }

void setWifiMessage(const char *top, const char *bottom)
{
    Command c;
    char err[48];
    if (!buildLcdMsg(1, top, bottom, c, err, sizeof err)) return;
    // Store first, so a resync after an STM32 reset sends the current text
    // even if this send is lost.
    config.lcd[0].top = c.lcdTop;
    config.lcd[0].bottom = c.lcdBottom;
    submit(c);
}

const String &lcdTop(int slot) { return config.lcd[constrain(slot, 1, kLcdSlots) - 1].top; }
const String &lcdBottom(int slot) { return config.lcd[constrain(slot, 1, kLcdSlots) - 1].bottom; }
int lcdRotateMask() { return rotMask; }
int lcdPage() { return page; }
int lcdOn() { return lcdEnabled; }

const Link::Stats &linkStats() { return link.stats(); }
size_t queueDepth() { return link.pending(); }

void onStatus(std::function<void(const String &)> cb) { statusCb = std::move(cb); }
void onLinkChange(std::function<void(bool)> cb) { linkCb = std::move(cb); }

}  // namespace station
