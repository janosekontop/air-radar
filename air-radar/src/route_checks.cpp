#include "route_policy.h"
#include <cmath>
#include <cstring>

namespace Routes {
Radar::TrackingChecks checks() {
    Radar::TrackingChecks result;
    auto check = [&](bool ok) { if (ok) ++result.passed; else ++result.failed; };
    check(eligible("ASL123"));
    check(eligible("TVS5NR"));
    check(eligible("DLHABC"));
    check(eligible("LS5"));
    check(!eligible("N123AB"));
    check(!eligible(""));
    check(!eligible("AB"));
    check(!eligible("ABC DEF"));
    check(!eligible("123ABC"));
    check(!eligible("ABCDEFGHIJKLM"));
    Radar::Aircraft aircraft[3];
    strcpy(aircraft[0].callsign, "N123AB");
    strcpy(aircraft[1].callsign, "ASL123");
    strcpy(aircraft[2].callsign, "DLHABC");
    bool pending[] = {true, true, true};
    check(choose(aircraft, 3, "N123AB", pending) == 1); // unsupported selection cannot block other planes
    check(choose(aircraft, 3, "DLHABC", pending) == 2); // selected eligible flight has priority
    pending[1] = false;
    check(choose(aircraft, 3, "ASL123", pending) == 2); // cached routes do not block pending routes
    pending[2] = false;
    check(choose(aircraft, 3, "N123AB", pending) == -1);
    auto &a = aircraft[1];
    a.east = 3; a.north = 4; a.hasTrack = a.hasSpeed = true;
    a.speed = 720; a.track = 90; a.sampledAt = a.motionStart = 1000;
    const auto original = a;
    const auto before = Radar::pose(a, 6000);
    Metadata route;
    strcpy(route.callsign, "DLHABC"); strcpy(route.origin, "BEG"); strcpy(route.destination, "IST");
    strcpy(route.originCity, "Belgrade"); strcpy(route.destinationCity, "Istanbul");
    check(!apply(a, route) && !a.origin[0]); // cannot attach another flight's route
    strcpy(route.callsign, "ASL123");
    check(apply(a, route) && !strcmp(a.origin, "BEG") && !strcmp(a.destinationCity, "Istanbul"));
    const auto after = Radar::pose(a, 6000);
    check(a.sampledAt == original.sampledAt && a.motionStart == original.motionStart &&
          a.positionAge == original.positionAge && a.speed == original.speed &&
          fabsf(before.east - after.east) < .001 && fabsf(before.north - after.north) < .001);
    check(!apply(a, route)); // unchanged metadata does not generate duplicate events
    route.destination[0] = 0;
    check(!apply(a, route) && !strcmp(a.destination, "IST")); // incomplete data does not erase a known route
    return result;
}
}
