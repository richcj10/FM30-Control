#pragma once
// Optional MQTT: status out, commands/RT/PS in, Home Assistant discovery.
// Off while config.mqttHost is empty.

namespace mqtt {
void begin();
void loop();
void reconfigure();          // broker settings changed
const char *state();         // "off", "connected", "disconnected"
}
