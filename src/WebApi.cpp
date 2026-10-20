#include "WebApi.h"
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include "AppConfig.h"
#include "Log.h"
#include "Mqtt.h"
#include "Station.h"

using namespace fm30;

namespace webapi {

namespace {

ESP8266WebServer server(80);
bool started_;
uint32_t restartAt;
bool resetWifi;

// Control-password sessions. POST /api/unlock trades the password for a
// random token; the page sends it back in X-Unlock. Tokens expire after
// 15 min without use; at most kSessions are live (oldest is dropped).
constexpr int kSessions = 4;
constexpr uint32_t kSessionIdleMs = 15UL * 60 * 1000;
struct Session {
    char token[33];
    uint32_t lastUse;
};
Session sessions[kSessions];
uint8_t badTries;
uint32_t lockoutUntil;

bool lockEnabled() { return config.lockPass.length() > 0; }

Session *findSession()
{
    String t = server.header(F("X-Unlock"));
    if (t.length() != 32) return nullptr;
    for (Session &s : sessions) {
        if (s.token[0] && millis() - s.lastUse < kSessionIdleMs && t == s.token) return &s;
    }
    return nullptr;
}

bool unlocked()
{
    if (!lockEnabled()) return true;
    Session *s = findSession();
    if (s) s->lastUse = millis();
    return s != nullptr;
}

void dropAllSessions()
{
    for (Session &s : sessions) s.token[0] = 0;
}

bool authed()
{
    if (!config.httpPass.length()) return true;
    if (server.authenticate(config.httpUser.c_str(), config.httpPass.c_str())) return true;
    server.requestAuthentication(BASIC_AUTH, "FM30");
    return false;
}

void on(const char *path, HTTPMethod method, std::function<void()> fn)
{
    server.on(path, method, [fn] {
        if (authed()) fn();
    });
}

void sendJson(int code, const JsonDocument &doc)
{
    String out;
    serializeJson(doc, out);
    server.sendHeader(F("Cache-Control"), F("no-store"));
    server.send(code, F("application/json"), out);
}

void sendError(int code, const String &msg)
{
    JsonDocument d;
    d["ok"] = false;
    d["error"] = msg;
    sendJson(code, d);
}

bool isJsonRequest()
{
    return server.header(F("Content-Type")).startsWith(F("application/json"));
}

// JSON bodies must say so in Content-Type. Browsers can't send that
// cross-site without a CORS preflight (which we never approve), so a random
// web page can't drive the transmitter through the user's browser.
bool readJson(JsonDocument &doc)
{
    if (!isJsonRequest()) { sendError(415, F("Content-Type must be application/json")); return false; }
    if (deserializeJson(doc, server.arg(F("plain"))) || !doc.is<JsonObject>()) {
        sendError(400, F("body must be a JSON object"));
        return false;
    }
    return true;
}

// rt/ps: {"text":"..."} JSON, a text=... form/query field, or a text/plain body.
bool readText(String &out)
{
    if (isJsonRequest()) {
        JsonDocument d;
        if (deserializeJson(d, server.arg(F("plain"))) || !d["text"].is<const char *>()) {
            sendError(400, F("expected {\"text\":\"...\"}"));
            return false;
        }
        out = d["text"].as<const char *>();
        return true;
    }
    if (server.hasArg(F("text"))) { out = server.arg(F("text")); return true; }
    if (server.hasArg(F("plain"))) { out = server.arg(F("plain")); return true; }
    sendError(400, F("send {\"text\":\"...\"}, text=..., or a text/plain body"));
    return false;
}

void sendLocked()
{
    JsonDocument d;
    d["ok"] = false;
    d["locked"] = true;
    d["error"] = F("locked: enter the control password to change frequency, power, tuning or settings");
    sendJson(401, d);
}

// For settings/reboot endpoints: true if allowed, else replies 401.
bool requireUnlock()
{
    if (unlocked()) return true;
    sendLocked();
    return false;
}

void execute(const Command &c)
{
    if (isProtected(c.cmd) && !unlocked()) { sendLocked(); return; }
    String err;
    if (!station::allowedFromUser(c, err)) { sendError(400, err); return; }
    station::SyncResult r = station::run(c);
    JsonDocument d;
    d["cmd"] = c.line;
    int code = 200;
    if (!r.queued) {
        d["ok"] = false;
        d["error"] = F("busy: command queue full");
        code = 503;
    } else if (r.outcome == Outcome::Timeout) {
        d["ok"] = false;
        d["error"] = F("no reply from the STM32 (timeout)");
        code = 504;
    } else {
        d["ok"] = r.outcome == Outcome::Ok;
        d["reply"] = r.line;
        if (r.outcome == Outcome::Err) code = 422;
        if (c.cmd == Cmd::Help || c.cmd == Cmd::DacDbg) {
            JsonArray a = d["help"].to<JsonArray>();
            int start = 0, nl;
            while ((nl = r.body.indexOf('\n', start)) >= 0) {
                a.add(r.body.substring(start, nl));
                start = nl + 1;
            }
        }
    }
    sendJson(code, d);
}

void handleIndex()
{
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
        server.send(500, F("text/plain"), F("index.html missing: run 'pio run -t uploadfs'"));
        return;
    }
    server.sendHeader(F("Cache-Control"), F("no-cache"));
    server.streamFile(f, F("text/html"));
    f.close();
}

void handleStatus()
{
    if (!station::haveStatus()) { sendError(503, F("no STATUS from the STM32 yet")); return; }
    server.sendHeader(F("Cache-Control"), F("no-store"));
    server.sendHeader(F("X-Status-Age-Ms"), String(station::statusAgeMs()));
    server.sendHeader(F("X-Link"), station::linkUp() ? F("up") : F("down"));
    server.send(200, F("application/json"), station::statusJson());
}

void handleInfo()
{
    JsonDocument d;
    d["fw"] = FW_VERSION;
    d["hostname"] = config.hostname;
    d["ip"] = WiFi.localIP().toString();
    d["rssi"] = WiFi.RSSI();
    d["uptime"] = millis() / 1000;
    d["heap"] = ESP.getFreeHeap();
    d["mqtt"] = mqtt::state();
    d["ota"] = config.otaPass.length() > 0;
    d["power_max"] = kPowerMax;
    JsonObject lk = d["lock"].to<JsonObject>();
    lk["enabled"] = lockEnabled();
    lk["unlocked"] = unlocked();
    lk["mqtt_allowed"] = !lockEnabled() || config.mqttAllowLocked;

    JsonObject l = d["link"].to<JsonObject>();
    const Link::Stats &s = station::linkStats();
    l["up"] = station::linkUp();
    if (station::haveStatus()) l["status_age_ms"] = station::statusAgeMs();
    l["stm32_resets"] = station::stm32Resets();
    l["queue"] = station::queueDepth();
    l["sent"] = s.sent;
    l["timeouts"] = s.timeouts;
    l["retries"] = s.retries;
    l["stale"] = s.stale;
    l["errors"] = s.errors;
    l["overflows"] = s.overflows;

    JsonObject lcd = d["lcd"].to<JsonObject>();
    JsonArray slots = lcd["slots"].to<JsonArray>();
    for (int i = 1; i <= kLcdSlots; i++) {
        JsonObject o = slots.add<JsonObject>();
        o["top"] = station::lcdTop(i);
        o["bottom"] = station::lcdBottom(i);
    }
    // Unknown (null) until set from here after the STM32 booted.
    int mask = station::lcdRotateMask();
    if (mask >= 0) {
        JsonArray r = lcd["rotate"].to<JsonArray>();
        for (int p = 1; p <= kLcdPages; p++)
            if (mask & (1 << (p - 1))) r.add(p);
    } else {
        lcd["rotate"] = nullptr;
    }
    if (station::lcdPage() >= 0) lcd["page"] = station::lcdPage(); else lcd["page"] = nullptr;
    if (station::lcdOn() >= 0) lcd["on"] = station::lcdOn() == 1; else lcd["on"] = nullptr;
    sendJson(200, d);
}

void handleCmd()
{
    JsonDocument doc;
    if (!readJson(doc)) return;
    if (!doc["cmd"].is<const char *>()) { sendError(400, F("expected {\"cmd\":\"...\"}")); return; }
    Command c;
    char err[64];
    if (!parseCommand(doc["cmd"], c, err, sizeof err)) { sendError(400, err); return; }
    execute(c);
}

void handleRt()
{
    String text;
    if (!readText(text)) return;
    Command c;
    buildRt(text.c_str(), c);
    execute(c);
}

void handlePs()
{
    String text;
    if (!readText(text)) return;
    Command c;
    buildPs(text.c_str(), c);
    execute(c);
}

void handleLcdMsg()
{
    JsonDocument doc;
    if (!readJson(doc)) return;
    Command c;
    char err[64];
    if (!buildLcdMsg(doc["slot"] | 0, doc["top"] | "", doc["bottom"] | "", c, err, sizeof err)) {
        sendError(400, err);
        return;
    }
    execute(c);
}

void handleSave()
{
    Command c;
    char err[32];
    parseCommand("SAVE", c, err, sizeof err);
    execute(c);
}

void handleConfigGet()
{
    JsonDocument d;
    configToJson(d.to<JsonObject>());
    d.remove("lcd");
    sendJson(200, d);
}

void handleConfigPost()
{
    if (!requireUnlock()) return;
    JsonDocument doc;
    if (!readJson(doc)) return;
    doc.remove("lcd");   // message slots are set through LCD MSG, not here
    String oldHost = config.hostname, oldOta = config.otaPass, oldLock = config.lockPass;
    String err;
    if (!configFromJson(doc.as<JsonObjectConst>(), err)) { sendError(400, err); return; }
    if (!configSave()) { sendError(500, F("could not write config")); return; }
    mqtt::reconfigure();
    if (config.lockPass != oldLock) dropAllSessions();   // new password: everyone unlocks again
    JsonDocument d;
    d["ok"] = true;
    d["reboot_needed"] = config.hostname != oldHost || config.otaPass != oldOta;
    sendJson(200, d);
}

void handleUnlock()
{
    JsonDocument doc;
    if (!readJson(doc)) return;
    if (!lockEnabled()) { sendError(400, F("no control password is set")); return; }
    if ((int32_t)(lockoutUntil - millis()) > 0) {
        sendError(429, F("too many wrong passwords; try again in 30 s"));
        return;
    }
    if (config.lockPass != (doc["password"] | "")) {
        if (++badTries >= 5) {
            badTries = 0;
            lockoutUntil = millis() + 30000;
            LOGF("unlock: 5 wrong passwords, locked out for 30 s");
        }
        delay(300);   // slow down guessing a little more
        sendError(403, F("wrong password"));
        return;
    }
    badTries = 0;
    // Reuse a free or expired slot, else the least recently used one.
    Session *slot = &sessions[0];
    for (Session &s : sessions) {
        if (!s.token[0] || millis() - s.lastUse >= kSessionIdleMs) { slot = &s; break; }
        if (s.lastUse < slot->lastUse) slot = &s;
    }
    for (int i = 0; i < 4; i++) snprintf(slot->token + i * 8, 9, "%08x", (unsigned)RANDOM_REG32);
    slot->lastUse = millis();
    JsonDocument d;
    d["ok"] = true;
    d["token"] = slot->token;
    d["idle_timeout_s"] = kSessionIdleMs / 1000;
    sendJson(200, d);
}

void handleLock()
{
    Session *s = findSession();
    if (s) s->token[0] = 0;
    JsonDocument d;
    d["ok"] = true;
    sendJson(200, d);
}

void handleReboot()
{
    if (!requireUnlock()) return;
    JsonDocument d;
    d["ok"] = true;
    sendJson(200, d);
    restartAt = millis() + 500;
}

void handleWifiReset()
{
    if (!requireUnlock()) return;
    JsonDocument d;
    d["ok"] = true;
    d["note"] = F("WiFi credentials cleared; rebooting into the setup portal");
    sendJson(200, d);
    resetWifi = true;
    restartAt = millis() + 500;
}

}  // namespace

void begin()
{
    if (started_) return;
    server.collectHeaders(F("Content-Type"), F("X-Unlock"));

    on("/", HTTP_GET, handleIndex);
    on("/api/status", HTTP_GET, handleStatus);
    on("/api/info", HTTP_GET, handleInfo);
    on("/api/cmd", HTTP_POST, handleCmd);
    on("/api/rt", HTTP_POST, handleRt);
    on("/api/ps", HTTP_POST, handlePs);
    on("/api/lcd/msg", HTTP_POST, handleLcdMsg);
    on("/api/save", HTTP_POST, handleSave);
    on("/api/config", HTTP_GET, handleConfigGet);
    on("/api/config", HTTP_POST, handleConfigPost);
    on("/api/unlock", HTTP_POST, handleUnlock);
    on("/api/lock", HTTP_POST, handleLock);
    on("/api/reboot", HTTP_POST, handleReboot);
    on("/api/wifi/reset", HTTP_POST, handleWifiReset);
    server.onNotFound([] {
        if (!authed()) return;
        sendError(404, F("not found"));
    });
    server.begin();
    started_ = true;
    LOGF("web: listening on port 80");
}

void loop()
{
    if (started_) server.handleClient();
    if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
        if (resetWifi) {
            WiFiManager wm;
            wm.resetSettings();
        }
        LOGF("rebooting");
        ESP.restart();
    }
}

bool started() { return started_; }

}  // namespace webapi
