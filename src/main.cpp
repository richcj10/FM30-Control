/*
 * FM30 network front-end (ESP8266).
 *
 * The STM32 controller runs the transmitter; this board only talks to it over
 * UART0 (swapped to GPIO13 RX / GPIO15 TX) and puts a web UI, REST API, MQTT
 * and OTA in front of it. Debug log: Serial1 on GPIO2.
 */
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include "AppConfig.h"
#include "Log.h"
#include "Mqtt.h"
#include "Station.h"
#include "WebApi.h"

static WiFiManager wm;
static String apName;
static bool wifiUp, netStarted, otaStarted;
static uint32_t wifiDownSince, lastReconnect;

// Captive-portal fields for the ESP-side settings (WiFi itself is handled by WiFiManager).
static WiFiManagerParameter *pMqttHost, *pMqttPort, *pMqttUser, *pMqttPass, *pMqttBase, *pOtaPass, *pHttpPass, *pLockPass;

static void saveParams()
{
    JsonDocument d;
    JsonObject m = d["mqtt"].to<JsonObject>();
    m["host"] = pMqttHost->getValue();
    if (atoi(pMqttPort->getValue()) > 0) m["port"] = atoi(pMqttPort->getValue());
    m["user"] = pMqttUser->getValue();
    if (*pMqttPass->getValue()) m["pass"] = pMqttPass->getValue();
    m["base"] = pMqttBase->getValue();
    if (*pOtaPass->getValue()) d["ota_pass"] = pOtaPass->getValue();
    if (*pHttpPass->getValue()) d["http_pass"] = pHttpPass->getValue();
    if (*pLockPass->getValue()) d["lock_pass"] = pLockPass->getValue();
    String err;
    if (!configFromJson(d.as<JsonObjectConst>(), err)) {
        LOGF("portal settings rejected: %s", err.c_str());
        return;
    }
    configSave();
    mqtt::reconfigure();
}

static void setupWifiManager()
{
    static char port[6];
    snprintf(port, sizeof port, "%u", config.mqttPort);
    // Passwords are never pre-filled; leave blank to keep the current one.
    pMqttHost = new WiFiManagerParameter("mqtt_host", "MQTT broker (blank = MQTT off)", config.mqttHost.c_str(), 64);
    pMqttPort = new WiFiManagerParameter("mqtt_port", "MQTT port", port, 5);
    pMqttUser = new WiFiManagerParameter("mqtt_user", "MQTT user", config.mqttUser.c_str(), 64);
    pMqttPass = new WiFiManagerParameter("mqtt_pass", "MQTT password (blank = unchanged)", "", 64, "type='password'");
    pMqttBase = new WiFiManagerParameter("mqtt_base", "MQTT base topic", config.mqttBase.c_str(), 48);
    pOtaPass = new WiFiManagerParameter("ota_pass", "OTA password (required for OTA)", "", 64, "type='password'");
    pHttpPass = new WiFiManagerParameter("http_pass", "Web UI password, user 'admin' (blank = none)", "", 64, "type='password'");
    pLockPass = new WiFiManagerParameter("lock_pass", "Control password: frequency/power/tuning (blank = none)", "", 64, "type='password'");
    for (WiFiManagerParameter *p : {pMqttHost, pMqttPort, pMqttUser, pMqttPass, pMqttBase, pOtaPass, pHttpPass, pLockPass})
        wm.addParameter(p);

    wm.setSaveParamsCallback(saveParams);
    wm.setAPCallback([](WiFiManager *) {
        LOGF("WiFi setup portal up: join AP '%s', browse to 192.168.4.1", apName.c_str());
        station::setWifiMessage("WiFi", ("setup AP " + apName).c_str());
    });
    wm.setConfigPortalBlocking(false);   // keep serving the STM32 link meanwhile
    wm.setConfigPortalTimeout(300);      // then fall back to retrying the saved network
    wm.setConnectTimeout(20);
    wm.setHostname(config.hostname.c_str());
    wm.setDebugOutput(false);

    apName = "FM30-Setup-" + String(ESP.getChipId() & 0xFFFF, HEX);
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    wm.autoConnect(apName.c_str());      // returns at once if the portal opens
}

static void startOta()
{
    if (!config.otaPass.length()) {
        LOGF("OTA off: set an OTA password in the web UI or setup portal");
        return;
    }
    ArduinoOTA.setHostname(config.hostname.c_str());
    ArduinoOTA.setPassword(config.otaPass.c_str());
    ArduinoOTA.onStart([] {
        bool fs = ArduinoOTA.getCommand() == U_FS;
        LOGF("OTA start (%s)", fs ? "filesystem" : "firmware");
        if (fs) LittleFS.end();
        fm30::Command c;
        char err[32];
        if (fm30::buildLcdMsg(1, "OTA", "updating", c, err, sizeof err)) station::run(c);
    });
    ArduinoOTA.onEnd([] { LOGF("OTA done, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) { LOGF("OTA error %u", (unsigned)e); });
    ArduinoOTA.begin(false);   // mDNS is started below
    MDNS.enableArduino(8266, true);
    otaStarted = true;
    LOGF("OTA ready on %s.local:8266", config.hostname.c_str());
}

// Starts the network services once, on the first connection.
static void startNetServices()
{
    if (netStarted) return;
    netStarted = true;
    if (MDNS.begin(config.hostname.c_str())) MDNS.addService("http", "tcp", 80);
    startOta();
    webapi::begin();
}

static void wifiTask()
{
    bool up = WiFi.status() == WL_CONNECTED;
    uint32_t now = millis();
    // The portal's web server holds port 80; close it before ours starts
    // (e.g. the saved network came back while the portal was open).
    if (up && wm.getConfigPortalActive()) {
        LOGF("WiFi connected, closing setup portal");
        wm.stopConfigPortal();
        return;
    }
    if (up && !wifiUp) {
        String ip = WiFi.localIP().toString();
        LOGF("WiFi connected: %s (%s, %d dBm)", ip.c_str(), WiFi.SSID().c_str(), WiFi.RSSI());
        station::setWifiMessage("WiFi OK", ip.c_str());
        startNetServices();
    } else if (!up && wifiUp) {
        LOGF("WiFi disconnected");
        station::setWifiMessage("WiFi", "disconnected");
        wifiDownSince = lastReconnect = now;
    }
    wifiUp = up;

    // The SDK reconnects by itself; nudge it now and then in case it gave up.
    if (!up && netStarted && !wm.getConfigPortalActive() && now - lastReconnect >= 30000) {
        lastReconnect = now;
        LOGF("WiFi still down after %lu s, reconnecting", (unsigned long)((now - wifiDownSince) / 1000));
        WiFi.reconnect();
    }
    // Never connected and the portal timed out: keep retrying the saved network.
    if (!up && !netStarted && !wm.getConfigPortalActive() && now - lastReconnect >= 30000) {
        lastReconnect = now;
        if (WiFi.SSID().length()) WiFi.begin();
        else wm.startConfigPortal(apName.c_str());   // nothing saved: reopen the portal
    }
}

void setup()
{
    Serial1.begin(115200);   // debug log, GPIO2
    LOGF("fm30-esp %s, chip %06x", FW_VERSION, ESP.getChipId());
    station::begin();        // swaps UART0 to GPIO13/15

    if (!LittleFS.begin()) LOGF("LittleFS mount failed (web UI missing until 'pio run -t uploadfs')");
    configLoad();
    String ssid, pass;
    bool provisioned = configApplySetup(ssid, pass);
    WiFi.hostname(config.hostname);
    WiFi.mode(WIFI_STA);
    if (provisioned && ssid.length()) {
        // Stored by the SDK, so later boots (and WiFiManager) use it too.
        WiFi.persistent(true);
        WiFi.begin(ssid, pass);
        LOGF("WiFi from setup.json: %s", ssid.c_str());
    }

    mqtt::begin();
    setupWifiManager();
}

void loop()
{
    station::loop();
    wm.process();
    wifiTask();
    if (netStarted) MDNS.update();
    if (otaStarted) ArduinoOTA.handle();
    webapi::loop();
    mqtt::loop();
}
