#include "AppConfig.h"
#include <LittleFS.h>
#include "Log.h"
#include "Sanitize.h"

AppConfig config;

static const char kPath[] = "/config.json";

void configDefaults()
{
    config = AppConfig();
    char name[16];
    snprintf(name, sizeof name, "fm30-%06x", ESP.getChipId());
    config.hostname = name;
}

static bool validHostname(const String &s)
{
    if (s.length() < 1 || s.length() > 32 || s[0] == '-') return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '-') return false;
    return true;
}

static bool validTopic(const String &s)
{
    if (s.length() < 1 || s.length() > 48 || s[0] == '/' || s[s.length() - 1] == '/') return false;
    for (char c : s)
        if (c == '+' || c == '#' || (unsigned char)c < 0x21 || (unsigned char)c > 0x7E) return false;
    return true;
}

static bool validField(const String &s, size_t max)
{
    if (s.length() > max) return false;
    for (char c : s)
        if ((unsigned char)c < 0x20) return false;
    return true;
}

bool configLoad()
{
    configDefaults();
    File f = LittleFS.open(kPath, "r");
    if (!f) {
        LOGF("config: none saved, using defaults");
        return false;
    }
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e) {
        LOGF("config: parse error %s, using defaults", e.c_str());
        return false;
    }
    String err;
    if (!configFromJson(doc.as<JsonObjectConst>(), err)) {
        LOGF("config: %s, using defaults", err.c_str());
        configDefaults();
        return false;
    }
    return true;
}

bool configSave()
{
    JsonDocument doc;
    JsonObject o = doc.to<JsonObject>();
    configToJson(o);
    o.remove("ota_pass_set");
    o.remove("http_pass_set");
    o.remove("lock_pass_set");
    o["lock_pass"] = config.lockPass;
    o["mqtt"].as<JsonObject>().remove("pass_set");
    o["ota_pass"] = config.otaPass;
    o["http_pass"] = config.httpPass;
    o["mqtt"]["pass"] = config.mqttPass;

    File f = LittleFS.open("/config.tmp", "w");
    if (!f) { LOGF("config: can't write"); return false; }
    size_t n = serializeJson(doc, f);
    f.close();
    if (!n) { LOGF("config: write failed"); return false; }
    LittleFS.remove(kPath);
    if (!LittleFS.rename("/config.tmp", kPath)) { LOGF("config: rename failed"); return false; }
    LOGF("config: saved (%u bytes)", (unsigned)n);
    return true;
}

bool configApplySetup(String &ssid, String &pass)
{
    File f = LittleFS.open("/setup.json", "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e) {
        LOGF("setup.json: parse error %s; not applied", e.c_str());
        return false;
    }
    String err;
    if (!configFromJson(doc.as<JsonObjectConst>(), err)) {
        LOGF("setup.json: %s; not applied", err.c_str());
        return false;
    }
    ssid = doc["wifi"]["ssid"] | "";
    pass = doc["wifi"]["pass"] | "";
    configSave();
    LittleFS.remove("/setup.json");   // passwords shouldn't linger in a second file
    LOGF("setup.json applied and removed%s", ssid.length() ? " (WiFi included)" : "");
    return true;
}

void configToJson(JsonObject o)
{
    o["hostname"] = config.hostname;
    o["ota_pass_set"] = config.otaPass.length() > 0;
    o["http_user"] = config.httpUser;
    o["http_pass_set"] = config.httpPass.length() > 0;
    o["lock_pass_set"] = config.lockPass.length() > 0;
    JsonObject m = o["mqtt"].to<JsonObject>();
    m["host"] = config.mqttHost;
    m["port"] = config.mqttPort;
    m["user"] = config.mqttUser;
    m["pass_set"] = config.mqttPass.length() > 0;
    m["base"] = config.mqttBase;
    m["ha_discovery"] = config.haDiscovery;
    m["ha_prefix"] = config.haPrefix;
    m["allow_locked"] = config.mqttAllowLocked;
    JsonArray lcd = o["lcd"].to<JsonArray>();
    for (int i = 0; i < fm30::kLcdSlots; i++) {
        JsonObject s = lcd.add<JsonObject>();
        // Slot 1 is the live WiFi status; don't persist it.
        s["top"] = i ? config.lcd[i].top : "";
        s["bottom"] = i ? config.lcd[i].bottom : "";
    }
}

bool configFromJson(JsonObjectConst in, String &err)
{
    AppConfig c = config;   // validate everything before touching the live config

    if (in["hostname"].is<const char *>()) c.hostname = in["hostname"].as<const char *>();
    if (in["ota_pass"].is<const char *>()) c.otaPass = in["ota_pass"].as<const char *>();
    if (in["http_user"].is<const char *>()) c.httpUser = in["http_user"].as<const char *>();
    if (in["http_pass"].is<const char *>()) c.httpPass = in["http_pass"].as<const char *>();
    if (in["lock_pass"].is<const char *>()) c.lockPass = in["lock_pass"].as<const char *>();

    JsonObjectConst m = in["mqtt"];
    if (m) {
        if (m["host"].is<const char *>()) c.mqttHost = m["host"].as<const char *>();
        if (m["port"].is<int>()) {
            int p = m["port"];
            if (p < 1 || p > 65535) { err = "mqtt.port 1-65535"; return false; }
            c.mqttPort = (uint16_t)p;
        }
        if (m["user"].is<const char *>()) c.mqttUser = m["user"].as<const char *>();
        if (m["pass"].is<const char *>()) c.mqttPass = m["pass"].as<const char *>();
        if (m["base"].is<const char *>()) c.mqttBase = m["base"].as<const char *>();
        if (m["ha_discovery"].is<bool>()) c.haDiscovery = m["ha_discovery"];
        if (m["ha_prefix"].is<const char *>()) c.haPrefix = m["ha_prefix"].as<const char *>();
        if (m["allow_locked"].is<bool>()) c.mqttAllowLocked = m["allow_locked"];
    }

    JsonArrayConst lcd = in["lcd"];
    if (lcd) {
        for (int i = 1; i < fm30::kLcdSlots && i < (int)lcd.size(); i++) {
            char top[fm30::kLcdTopLen + 1], bottom[fm30::kLcdBottomLen + 1];
            fm30::sanitizeText(lcd[i]["top"] | "", top, fm30::kLcdTopLen, true);
            fm30::sanitizeText(lcd[i]["bottom"] | "", bottom, fm30::kLcdBottomLen, true);
            c.lcd[i].top = top;
            c.lcd[i].bottom = bottom;
        }
    }

    if (!validHostname(c.hostname)) { err = "hostname: 1-32 of a-z 0-9 -"; return false; }
    if (!validField(c.mqttHost, 64) || c.mqttHost.indexOf(' ') >= 0) { err = "mqtt.host"; return false; }
    if (!validField(c.mqttUser, 64) || !validField(c.mqttPass, 64)) { err = "mqtt user/pass: max 64 chars"; return false; }
    if (!validTopic(c.mqttBase)) { err = "mqtt.base: 1-48 chars, no + # spaces or leading/trailing /"; return false; }
    if (!validTopic(c.haPrefix)) { err = "mqtt.ha_prefix"; return false; }
    if (!validField(c.otaPass, 64)) { err = "ota_pass: max 64 chars"; return false; }
    if (!validField(c.httpUser, 32) || !c.httpUser.length()) { err = "http_user: 1-32 chars"; return false; }
    if (!validField(c.httpPass, 64)) { err = "http_pass: max 64 chars"; return false; }
    if (!validField(c.lockPass, 64)) { err = "lock_pass: max 64 chars"; return false; }

    config = c;
    return true;
}
