#pragma once
#include "radar.h"

namespace Routes {
struct Metadata {
    char callsign[13] = {};
    char origin[5] = {}, destination[5] = {};
    char originCity[48] = {}, destinationCity[48] = {};
};
bool eligible(const char *callsign);
int choose(const Radar::Aircraft *aircraft, size_t count, const char *selected, const bool *needsLookup);
bool apply(Radar::Aircraft &aircraft, const Metadata &route);
Radar::TrackingChecks checks();
}
