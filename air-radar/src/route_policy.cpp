#include "route_policy.h"
#include <algorithm>
#include <cstring>

namespace Routes {
bool eligible(const char *callsign) {
    if (!callsign) return false;
    const size_t length = strlen(callsign);
    if (length < 3 || length > 12) return false;
    if (callsign[0] < 'A' || callsign[0] > 'Z' || callsign[1] < 'A' || callsign[1] > 'Z') return false;
    // The suffix may contain letters only. Counting all leading letters wrongly
    // rejected such flights and left them blocking the lookup queue forever.
    for (size_t i = 2; i < length; ++i)
        if (!(callsign[i] >= 'A' && callsign[i] <= 'Z') && !(callsign[i] >= '0' && callsign[i] <= '9')) return false;
    return true;
}

int choose(const Radar::Aircraft *aircraft, size_t count, const char *selected, const bool *needsLookup) {
    int first = -1;
    for (size_t i = 0; i < std::min(count, Radar::capacity); ++i) {
        if (!needsLookup[i] || !eligible(aircraft[i].callsign)) continue;
        if (first < 0) first = i;
        if (selected && !strcmp(aircraft[i].callsign, selected)) return i;
    }
    return first;
}

bool apply(Radar::Aircraft &a, const Metadata &r) {
    if (!r.callsign[0] || strcmp(a.callsign, r.callsign) || !r.origin[0] || !r.destination[0]) return false;
    const bool changed = strcmp(a.origin, r.origin) || strcmp(a.destination, r.destination) ||
                         strcmp(a.originCity, r.originCity) || strcmp(a.destinationCity, r.destinationCity);
    if (changed) {
        memcpy(a.origin, r.origin, sizeof(a.origin)); memcpy(a.destination, r.destination, sizeof(a.destination));
        memcpy(a.originCity, r.originCity, sizeof(a.originCity)); memcpy(a.destinationCity, r.destinationCity, sizeof(a.destinationCity));
    }
    // Metadata never renews a position, restarts motion, or changes selection.
    return changed;
}
}
