#include "Mqtt.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <math.h>
#include "AppConfig.h"
#include "Log.h"
#include "Station.h"

using namespace fm30;

namespace mqtt {

namespace {

constexpr uint32_t kRetryMs = 10000;
constexpr size_t kMaxPayload = 400;

WiFiClient net;
PubSubClient client(net);
uint32_t lastAttempt;
bool everAttempted;
bool rediscover;   // Home Assistant came back online
String devId;

String topic(const char *suffix) { return config.mqttBase + '/' + suffix; }

bool enabled() { return config.mqttHost.length() > 0; }

// Frequency/power/tuning over MQTT: always with no control password set,
// otherwise only if the user opted in.
bool lockedAllowed() { return !config.lockPass.length() || config.mqttAllowLocked; }

void publish(const String &t, const String &payload, bool retain)
{
    if (client.connected() && !client.publish(t.c_str(), payload.c_str(), retain))
        LOGF("mqtt: publish to %s failed (%u bytes)", t.c_str(), (unsigned)payload.length());
}

void publishResult(const char *cmd, bool ok, const char *reply, const char *error)
{
    JsonDocument d;
    d["cmd"] = cmd;
    d["ok"] = ok;
    if (reply) d["reply"] = reply;
    if (error) d["error"] = error;
    String out;
    serializeJson(d, out);
    publish(topic("cmd/result"), out, false);
}

void submit(const Command &c)
{
    if (isProtected(c.cmd) && !lockedAllowed()) {
        publishResult(c.line, false, nullptr,
                      "locked: frequency/power/tuning over MQTT is off (Settings > MQTT to allow)");
        return;
    }
    String err;
    if (!station::allowedFromUser(c, err)) {
        publishResult(c.line, false, nullptr, err.c_str());
        return;
    }
    String line = c.line;
    bool queued = station::submit(c, [line](const Reply &r) {
        publishResult(line.c_str(), r.outcome == Outcome::Ok,
                      r.outcome == Outcome::Timeout ? nullptr : r.line,
                      r.outcome == Outcome::Timeout ? "timeout" : nullptr);
    });
    if (!queued) publishResult(c.line, false, nullptr, "busy: command queue full");
}

void reject(const char *what, const char *err)
{
    LOGF("mqtt: %s rejected: %s", what, err);
    publishResult(what, false, nullptr, err);
}

void onMessage(char *t, uint8_t *payload, unsigned int len)
{
    String tp(t);
    if (tp == config.haPrefix + "/status") {
        // Home Assistant restarted: send discovery again.
        if (len == 6 && !memcmp(payload, "online", 6)) rediscover = true;
        return;
    }
    if (len > kMaxPayload) { reject(t, "payload too long"); return; }
    char buf[kMaxPayload + 1];
    memcpy(buf, payload, len);
    buf[len] = 0;

    Command c;
    char err[64];
    String base = config.mqttBase + '/';
    if (!tp.startsWith(base)) return;
    String sub = tp.substring(base.length());

    if (sub == "cmd") {
        if (!parseCommand(buf, c, err, sizeof err)) return reject(buf, err);
    } else if (sub == "rt/set") {
        buildRt(buf, c);
    } else if (sub == "ps/set") {
        buildPs(buf, c);
    } else if (sub == "freq/set") {
        String s(buf);
        s.trim();
        if (!buildFreq(s.c_str(), c, err, sizeof err)) return reject(buf, err);
    } else if (sub == "tx/set") {
        String s = String("TX ") + buf;
        if (!parseCommand(s.c_str(), c, err, sizeof err)) return reject(buf, err);
    } else if (sub == "power/set") {
        // Percent only (Home Assistant may send "50" or "50.0").
        char *end;
        double pct = strtod(buf, &end);
        while (*end == ' ') end++;
        if (end == buf || *end || !isfinite(pct)) return reject(buf, "POWER 0-100 (percent)");
        if (!buildPowerPercent((int)lround(pct), c, err, sizeof err)) return reject(buf, err);
    } else {
        return;
    }
    submit(c);
}

void discovery(const char *component, const char *id, const char *name,
               std::function<void(JsonObject)> extra)
{
    JsonDocument d;
    d["name"] = name;
    d["uniq_id"] = devId + '_' + id;
    d["obj_id"] = config.hostname + '_' + id;
    d["stat_t"] = topic("status");
    JsonArray av = d["avty"].to<JsonArray>();
    av.add<JsonObject>()["t"] = topic("availability");
    JsonObject l = av.add<JsonObject>();
    l["t"] = topic("link");
    l["pl_avail"] = "up";
    l["pl_not_avail"] = "down";
    d["avty_mode"] = "all";
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"].to<JsonArray>().add(devId);
    dev["name"] = "FM30 transmitter";
    dev["mdl"] = "FM30 + STM32 controller";
    dev["sw"] = FW_VERSION;
    dev["cu"] = String("http://") + WiFi.localIP().toString() + '/';
    if (extra) extra(d.as<JsonObject>());
    String out;
    serializeJson(d, out);
    publish(config.haPrefix + '/' + component + '/' + devId + '/' + id + "/config", out, true);
}

void sendDiscovery()
{
    discovery("sensor", "state", "State", [](JsonObject o) {
        o["val_tpl"] = "{{ value_json.state }}";
        o["ic"] = "mdi:radio-tower";
    });
    // Frequency and power are controls only when MQTT may change them;
    // otherwise read-only sensors. Clear the other kind's retained config.
    bool rw = lockedAllowed();
    auto clear = [](const char *component, const char *id) {
        publish(config.haPrefix + '/' + component + '/' + devId + '/' + id + "/config", "", true);
    };
    clear(rw ? "sensor" : "number", "freq");
    clear(rw ? "sensor" : "number", "power");
    discovery(rw ? "number" : "sensor", "freq", "Frequency", [rw](JsonObject o) {
        o["val_tpl"] = "{{ value_json.freq }}";
        o["unit_of_meas"] = "MHz";
        if (!rw) return;
        o["cmd_t"] = topic("freq/set");
        o["min"] = 87.5;
        o["max"] = 108.0;
        o["step"] = 0.1;
        o["mode"] = "box";
    });
    discovery("switch", "tx", "Transmit", [](JsonObject o) {
        o["cmd_t"] = topic("tx/set");
        o["val_tpl"] = "{{ 'ON' if value_json.tx else 'OFF' }}";
        o["ic"] = "mdi:broadcast";
    });
    discovery(rw ? "number" : "sensor", "power", "Power", [rw](JsonObject o) {
        o["val_tpl"] = "{{ (value_json.power_target * 100 / 3968) | round(0) | int }}";
        o["unit_of_meas"] = "%";
        if (!rw) return;
        o["cmd_t"] = topic("power/set");
        o["min"] = 0;
        o["max"] = 100;
        o["step"] = 1;
        o["mode"] = "slider";
    });
    discovery("sensor", "vtune", "Vtune", [](JsonObject o) {
        o["val_tpl"] = "{{ value_json.vtune }}";
        o["unit_of_meas"] = "V";
        o["dev_cla"] = "voltage";
        o["stat_cla"] = "measurement";
        o["ent_cat"] = "diagnostic";
    });
    discovery("binary_sensor", "pilot", "Pilot lock", [](JsonObject o) {
        o["val_tpl"] = "{{ 'ON' if value_json.pilot_lock else 'OFF' }}";
        o["ic"] = "mdi:lock";
    });
    discovery("sensor", "fault", "Fault", [](JsonObject o) {
        o["val_tpl"] = "{{ value_json.fault if value_json.fault else 'none' }}";
        o["ic"] = "mdi:alert";
    });
    discovery("sensor", "faults", "Fault count", [](JsonObject o) {
        o["val_tpl"] = "{{ value_json.faults }}";
        o["stat_cla"] = "total_increasing";
        o["ent_cat"] = "diagnostic";
    });
    discovery("text", "ps", "RDS PS", [](JsonObject o) {
        o["cmd_t"] = topic("ps/set");
        o["val_tpl"] = "{{ value_json.ps | trim }}";
        o["max"] = 8;
    });
    discovery("text", "rt", "RDS RadioText", [](JsonObject o) {
        o["cmd_t"] = topic("rt/set");
        o["val_tpl"] = "{{ value_json.rt }}";
        o["max"] = 64;
    });
    discovery("sensor", "uptime", "Controller uptime", [](JsonObject o) {
        o["val_tpl"] = "{{ value_json.uptime }}";
        o["unit_of_meas"] = "s";
        o["dev_cla"] = "duration";
        o["ent_cat"] = "diagnostic";
    });
    discovery("button", "save", "Save settings to flash", [](JsonObject o) {
        o.remove("stat_t");
        o["cmd_t"] = topic("cmd");
        o["pl_prs"] = "SAVE";
        o["ent_cat"] = "config";
    });
    LOGF("mqtt: Home Assistant discovery sent");
}

void connect()
{
    lastAttempt = millis();
    everAttempted = true;
    client.setServer(config.mqttHost.c_str(), config.mqttPort);
    String avail = topic("availability");
    bool ok = config.mqttUser.length()
        ? client.connect(config.hostname.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str(),
                         avail.c_str(), 0, true, "offline")
        : client.connect(config.hostname.c_str(), avail.c_str(), 0, true, "offline");
    if (!ok) {
        LOGF("mqtt: connect to %s:%u failed (state %d)", config.mqttHost.c_str(), config.mqttPort, client.state());
        return;
    }
    LOGF("mqtt: connected to %s:%u", config.mqttHost.c_str(), config.mqttPort);
    publish(avail, "online", true);
    publish(topic("link"), station::linkUp() ? "up" : "down", true);
    for (const char *s : {"cmd", "rt/set", "ps/set", "freq/set", "tx/set", "power/set"})
        client.subscribe(topic(s).c_str());
    if (config.haDiscovery) {
        client.subscribe((config.haPrefix + "/status").c_str());
        sendDiscovery();
    }
    if (station::haveStatus()) publish(topic("status"), station::statusJson(), true);
}

}  // namespace

void begin()
{
    char id[16];
    snprintf(id, sizeof id, "fm30_%06x", ESP.getChipId());
    devId = id;
    net.setTimeout(1500);           // keep a dead broker from stalling the loop for long
    client.setBufferSize(1400);     // STATUS JSON (~620 B) and discovery payloads
    client.setKeepAlive(30);
    client.setCallback(onMessage);
    station::onStatus([](const String &json) { publish(topic("status"), json, true); });
    station::onLinkChange([](bool up) { publish(topic("link"), up ? "up" : "down", true); });
}

void loop()
{
    if (!enabled() || WiFi.status() != WL_CONNECTED) return;
    if (client.connected()) {
        client.loop();
        if (rediscover && config.haDiscovery) sendDiscovery();
        rediscover = false;
        return;
    }
    if (!everAttempted || millis() - lastAttempt >= kRetryMs) connect();
}

void reconfigure()
{
    if (client.connected()) {
        publish(topic("availability"), "offline", true);
        client.disconnect();
    }
    everAttempted = false;   // reconnect on the next loop with the new settings
}

const char *state()
{
    if (!enabled()) return "off";
    return client.connected() ? "connected" : "disconnected";
}

}  // namespace mqtt
