#include "radar.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Radar {
namespace {
float heading(float angle) {
    angle = fmodf(angle, 360.0f);
    return angle < 0 ? angle + 360 : angle;
}
float shortestTurn(float from, float to) { return fmodf(to - from + 540.0f, 360.0f) - 180.0f; }
float correctionRemaining(uint32_t elapsed, uint32_t duration) {
    if (!duration) return 0;
    const float t = std::min(1.0f, elapsed / float(duration));
    return 1 - t * t * (3 - 2 * t);
}
bool alive(const Aircraft &a, uint32_t now) {
    return positionAge(a, now) < trackLifetimeMs / 1000.0f &&
           (!a.missing || now - a.missingSince < missingGraceMs);
}
void keepText(char *to, const char *from, size_t size) {
    if (!to[0]) { strncpy(to, from, size - 1); to[size - 1] = 0; }
}
}

float positionAge(const Aircraft &a, uint32_t now) {
    return a.positionAge + (now - a.sampledAt) / 1000.0f;
}

Pose pose(const Aircraft &a, uint32_t now) {
    Pose p = {a.east, a.north, a.hasTrack ? a.track : 0};
    // Ground speed is already km/h. Bounded dead reckoning, then freeze.
    if (a.hasTrack && a.hasSpeed) {
        const float seconds = std::min(positionAge(a, now), predictionMs / 1000.0f);
        const float km = a.speed * seconds / 3600.0f;
        const float angle = a.track * 0.0174532925199433f;
        p.east += km * sinf(angle); p.north += km * cosf(angle);
    }
    const uint32_t elapsed = now - a.motionStart;
    const float remaining = correctionRemaining(elapsed, a.positionCorrectionMs);
    p.east += a.correctionEast * remaining;
    p.north += a.correctionNorth * remaining;
    p.track = heading(p.track + a.correctionTrack * correctionRemaining(elapsed, headingCorrectionMs));
    return p;
}

bool retained(const Aircraft &a, uint32_t now) {
    return a.missing || positionAge(a, now) >= staleMs / 1000.0f;
}

void mergeSnapshot(State &s, const Aircraft *incoming, size_t count, uint32_t received) {
    // Missing IDs are not an explicit deletion. Preserve them for a short grace
    // period, without allowing subsequent empty snapshots to renew that period.
    size_t kept = 0;
    for (size_t i = 0; i < s.count; ++i) {
        auto a = s.aircraft[i];
        if (!alive(a, received)) continue;
        if (!a.missing) { a.missing = true; a.missingSince = received; }
        s.aircraft[kept++] = a;
    }
    s.count = kept;
    for (size_t i = 0; i < std::min(count, capacity); ++i) {
        Aircraft a = incoming[i];
        if (!a.hex[0]) continue;
        size_t at = 0;
        while (at < s.count && strcmp(s.aircraft[at].hex, a.hex)) ++at;
        if (at < s.count) {
            const auto old = s.aircraft[at];
            // An older position must not replace a newer fix or reset its age.
            if (a.positionAge > positionAge(old, received) + 0.25f) continue;
            const bool sameFlight = !a.callsign[0] || !strcmp(old.callsign, a.callsign);
            if (sameFlight) {
                keepText(a.flight, old.flight, sizeof(a.flight));
                keepText(a.callsign, old.callsign, sizeof(a.callsign));
                keepText(a.model, old.model, sizeof(a.model));
                keepText(a.origin, old.origin, sizeof(a.origin));
                keepText(a.destination, old.destination, sizeof(a.destination));
                keepText(a.originCity, old.originCity, sizeof(a.originCity));
                keepText(a.destinationCity, old.destinationCity, sizeof(a.destinationCity));
            }
            const Pose before = pose(old, received);
            a.sampledAt = a.motionStart = received;
            a.positionCorrectionMs = 0;
            a.correctionEast = a.correctionNorth = a.correctionTrack = 0;
            a.missing = false;
            const Pose target = pose(a, received);
            const float eastError = before.east - target.east;
            const float northError = before.north - target.north;
            const float errorKm = hypotf(eastError, northError);
            // Smoothstep's peak correction velocity is 1.5 * distance / time.
            // Bound that velocity, rather than compressing every error into a
            // fixed short animation. Reacquire after a freeze or a large error
            // directly at the measured location, then resume at ground speed.
            if (old.hasTrack && old.hasSpeed && a.hasTrack && a.hasSpeed && a.speed > 1 &&
                positionAge(old, received) < predictionMs / 1000.0f) {
                const float limitKmh = std::min(maximumCorrectionKmh, a.speed * correctionSpeedFraction);
                const float durationMs = std::max(float(minimumCorrectionMs),
                    ceilf(1.5f * errorKm * 3600000.0f / limitKmh));
                if (durationMs <= maximumCorrectionMs &&
                    a.positionAge * 1000 + durationMs <= predictionMs) {
                    a.positionCorrectionMs = uint32_t(durationMs);
                    a.correctionEast = eastError;
                    a.correctionNorth = northError;
                }
            }
            a.correctionTrack = a.hasTrack && old.hasTrack ? shortestTurn(a.track, before.track) : 0;
        } else {
            // Prefer a fresh fix to the oldest held track if all 64 slots are full.
            if (s.count == capacity) {
                at = 0;
                for (size_t j = 1; j < s.count; ++j)
                    if (positionAge(s.aircraft[j], received) > positionAge(s.aircraft[at], received)) at = j;
                if (a.positionAge >= positionAge(s.aircraft[at], received)) continue;
            } else at = s.count++;
            a.sampledAt = a.motionStart = received;
            a.positionCorrectionMs = 0;
            a.correctionEast = a.correctionNorth = a.correctionTrack = 0;
            a.missing = false;
        }
        s.aircraft[at] = a;
    }
}

bool visible(const Aircraft &a, const State &s, uint32_t now) {
    if (!s.hasData || !alive(a, now)) return false;
    const auto p = pose(a, now);
    return fabsf(p.east) <= s.span / 2.0f && fabsf(p.north) <= s.span / 2.0f;
}

size_t visibleCount(const State &s, uint32_t now) {
    size_t count = 0;
    for (size_t i = 0; i < s.count; ++i) if (visible(s.aircraft[i], s, now)) ++count;
    return count;
}

int selectedIndex(const State &s, uint32_t now) {
    int closest = -1, previous = -1;
    float distance = 1e9f, previousDistance = 1e9f;
    for (size_t i = 0; i < s.count; ++i) {
        if (!visible(s.aircraft[i], s, now)) continue;
        const auto p = pose(s.aircraft[i], now);
        const float d = hypotf(p.east, p.north);
        if (!strcmp(s.selected, s.aircraft[i].hex)) { previous = i; previousDistance = d; }
        if (d < distance) { closest = i; distance = d; }
    }
    if (previous >= 0 && (!s.automatic || distance + 0.5f >= previousDistance * 0.85f)) return previous;
    return closest;
}

void updateSelection(State &s, uint32_t now) {
    const int selected = selectedIndex(s, now);
    if (selected >= 0) { strncpy(s.selected, s.aircraft[selected].hex, sizeof(s.selected) - 1); s.selected[8] = 0; }
    else s.selected[0] = 0;
}
}
