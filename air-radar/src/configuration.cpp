#include "configuration.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

namespace Configuration {
namespace {
portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
Snapshot current;
}
Snapshot get() {
    portENTER_CRITICAL(&guard);
    const auto result = current;
    portEXIT_CRITICAL(&guard);
    return result;
}
void publish(double latitude, double longitude, bool wifi, bool configured) {
    portENTER_CRITICAL(&guard);
    current.latitude=latitude;current.longitude=longitude;
    current.configured=configured;current.wifi=wifi;++current.revision;
    portEXIT_CRITICAL(&guard);
}
bool coordinates(double latitude, double longitude) {
    // Longitude ceases to be a useful flat-radar coordinate at the exact poles.
    return isfinite(latitude) && isfinite(longitude) && latitude>=-89.9 && latitude<=89.9 && longitude>=-180 && longitude<=180;
}
bool credentials(const char *ssid, const char *password) {
    if(!ssid || !password || !strlen(ssid) || strlen(ssid)>32 || strlen(password)>64) return false;
    const size_t n=strlen(password);
    if(n && n<8) return false;
    if(n==64) for(size_t i=0;i<n;++i) if(!isxdigit(static_cast<unsigned char>(password[i]))) return false;
    return true;
}
Checks checks() {
    Checks c;
    const auto check=[&](bool ok){ok ? ++c.passed : ++c.failed;};
    check(coordinates(0,0));check(coordinates(-33.9,151.2));check(coordinates(89.9,-180));
    check(!coordinates(90,0));check(!coordinates(0,181));check(!coordinates(NAN,0));check(!coordinates(0,INFINITY));
    check(credentials("Example Wi-Fi","example-passphrase"));check(credentials("Open network",""));
    check(!credentials("","example-passphrase"));check(!credentials("Example","short"));
    check(!credentials("123456789012345678901234567890123","example-passphrase"));
    check(credentials("Example","0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    check(!credentials("Example","zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"));
    return c;
}
}
