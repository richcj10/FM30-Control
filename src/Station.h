#pragma once
// The STM32 as seen from the ESP: the serial link, the last STATUS, STM32
// reset detection and the LCD message slots (re-sent after every STM32 reset,
// since the STM32 doesn't save message text).
#include <Arduino.h>
#include <functional>
#include "Commands.h"
#include "Link.h"

namespace station {

void begin();
void loop();

// Checks that apply to commands from users (web, REST, MQTT): LCD slot 1 is
// reserved for the WiFi status, and SAVE is rate-limited (flash wear).
bool allowedFromUser(const fm30::Command &c, String &err);

// Queues a command. Returns false if the queue is full.
bool submit(const fm30::Command &c, fm30::Link::Callback cb = nullptr);

// Sends a command and waits for its reply, servicing only the UART meanwhile
// (at most ~1 s per queued command). For web handlers; don't call from a
// link callback.
struct SyncResult {
    bool queued;            // false = queue full
    fm30::Outcome outcome;
    String line;            // reply line
    String body;            // HELP text
};
SyncResult run(const fm30::Command &c);

bool haveStatus();
const String &statusJson();        // last STATUS line, verbatim
uint32_t statusAgeMs();
bool linkUp();                     // heard from the STM32 in the last 6 s
uint32_t stm32Resets();

// Slot 1 (WiFi status line).
void setWifiMessage(const char *top, const char *bottom);
const String &lcdTop(int slot);    // slot 1..4
const String &lcdBottom(int slot);
int lcdRotateMask();               // -1 = unknown (not set since STM32 boot)
int lcdPage();                     // -1 = unknown
int lcdOn();                       // -1 = unknown

const fm30::Link::Stats &linkStats();
size_t queueDepth();

// New STATUS line (also unsolicited ones), and link up/down changes.
void onStatus(std::function<void(const String &json)> cb);
void onLinkChange(std::function<void(bool up)> cb);

}  // namespace station
