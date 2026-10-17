#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Radar {
constexpr size_t capacity = 64;
constexpr uint32_t staleMs = 30000;
constexpr uint32_t trackLifetimeMs = 75000;
constexpr uint32_t missingGraceMs = 30000;
constexpr uint32_t predictionMs = 30000;
constexpr uint32_t headingCorrectionMs = 1500;
constexpr uint32_t minimumCorrectionMs = 4000;
constexpr uint32_t maximumCorrectionMs = 15000;
constexpr float correctionSpeedFraction = 0.15f;
constexpr float maximumCorrectionKmh = 120.0f;


struct Aircraft {
    char hex[9] = {};
    char flight[13] = {};
    char callsign[13] = {};
    char model[9] = {};
    char origin[5] = {}, destination[5] = {};
    char originCity[48] = {}, destinationCity[48] = {};
    float east = 0, north = 0, distance = 0;
    float track = 0, speed = 0, positionAge = 0;
    bool hasTrack = false, hasSpeed = false;
    uint32_t sampledAt = 0, motionStart = 0, missingSince = 0;
    uint32_t positionCorrectionMs = 0;
    float correctionEast = 0, correctionNorth = 0, correctionTrack = 0;
    bool missing = false;
};

struct Pose { float east, north, track; };

struct State {
    Aircraft aircraft[capacity];
    size_t count = 0;
    unsigned span = 50; // Full width AND height in kilometres.
    uint32_t received = 0, sequence = 0, updates = 0;
    bool hasData = false, feedError = false;
    bool wifi = true;
    bool needsSetup = true;
    char setupHint[112] = {};
    char message[48] = "Complete radar setup";
    char selected[9] = {};
    bool automatic = true;
};

bool visible(const Aircraft &a, const State &s, uint32_t now);
int selectedIndex(const State &s, uint32_t now);
size_t visibleCount(const State &s, uint32_t now);
const char *status(const State &s, uint32_t now);
float positionAge(const Aircraft &a, uint32_t now);
Pose pose(const Aircraft &a, uint32_t now);
bool retained(const Aircraft &a, uint32_t now);
void mergeSnapshot(State &s, const Aircraft *incoming, size_t count, uint32_t received);
void updateSelection(State &s, uint32_t now);
struct TrackingChecks { unsigned passed = 0, failed = 0; };
TrackingChecks trackingChecks();
void uiInit();
void uiUpdate(const State &s, uint32_t now);
}
