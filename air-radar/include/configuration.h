#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Configuration {
struct Snapshot {
    double latitude = 0, longitude = 0;
    uint32_t revision = 0;
    bool configured = false, wifi = true;
};
Snapshot get();
void publish(double latitude, double longitude, bool wifi, bool configured);
bool coordinates(double latitude, double longitude);
bool credentials(const char *ssid, const char *password);
struct Checks { unsigned passed = 0, failed = 0; };
Checks checks();
}
