#pragma once
// ESP-side settings, stored as /config.json on LittleFS. These are separate
// from the STM32's own settings, which only change on an explicit SAVE.
#include <Arduino.h>
#include <ArduinoJson.h>
#include "Commands.h"

struct LcdText {
    String top;      // <= 8 chars, already sanitized
    String bottom;   // <= 48 chars, already sanitized
};

struct AppConfig {
    String hostname;              // also the mDNS name: <hostname>.local
    String otaPass;               // OTA stays off until this is set
    String httpUser = "admin";
    String httpPass;              // empty = web UI/API open on the LAN
    // Control password: needed to change frequency, power and tuning, and the
    // settings. Empty = no lock.
    String lockPass;

    String mqttHost;              // empty = MQTT off
    uint16_t mqttPort = 1883;
    String mqttUser, mqttPass;
    String mqttBase = "fm30";
    bool haDiscovery = true;
    // With a control password set, frequency/power/tuning over MQTT is off
    // unless this is set (MQTT has no per-command password; the broker's
    // credentials are the only gate).
    bool mqttAllowLocked = false;
    String haPrefix = "homeassistant";

    // LCD message slots 1-4 (slot 1 is the WiFi status and isn't saved).
    LcdText lcd[fm30::kLcdSlots];
};

extern AppConfig config;

void configDefaults();
bool configLoad();
bool configSave();
// One-shot provisioning: if /setup.json exists (uploaded from data/ with
// "Upload Filesystem Image"), applies it like configFromJson, saves, and
// deletes it. Returns its WiFi network (empty if none) so the caller can
// connect. An invalid file is left in place and reported in the log.
bool configApplySetup(String &ssid, String &pass);
// Public view (no secrets, "*_set" flags instead).
void configToJson(JsonObject out);
// Applies the fields present in `in`; absent fields are unchanged. Returns
// false with a reason if anything is invalid (and changes nothing).
bool configFromJson(JsonObjectConst in, String &err);
