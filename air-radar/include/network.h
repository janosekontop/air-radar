#pragma once
#include <ArduinoJson.h>
#include "radar.h"
#include "configuration.h"

namespace Network {
using KeyHandler = void (*)(char);
void begin(KeyHandler handler);
bool loop(Radar::State &state, uint64_t &sourceTimestamp);
bool configure(const char *ssid, const char *password);
bool applyConfiguration(JsonDocument &document);
void resetConfiguration();
void openSetup();
void info(JsonDocument &document);
bool probeRoute(const char *callsign, double latitude, double longitude);
}
