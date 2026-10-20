#pragma once
// Debug log on Serial1 (GPIO2, TX only, 115200). UART0 belongs to the STM32 link.
#include <Arduino.h>

#define LOGF(fmt, ...) Serial1.printf_P(PSTR("[%7lu] " fmt "\n"), (unsigned long)millis(), ##__VA_ARGS__)
