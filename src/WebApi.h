#pragma once
// Web UI (data/index.html on LittleFS) and the REST API on port 80.
// Started once WiFi is up; WiFiManager's captive portal also uses port 80.

namespace webapi {
void begin();
void loop();
bool started();
}
