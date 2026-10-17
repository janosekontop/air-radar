#include "radar.h"
#include <cmath>
#include <cstring>
#include <new>

namespace Radar {
TrackingChecks trackingChecks() {
    TrackingChecks result;
    auto check = [&](bool ok) { if (ok) ++result.passed; else ++result.failed; };
    auto *s = new(std::nothrow) State;
    if (!s) { ++result.failed; return result; }
    auto reset = [&]() { memset(s, 0, sizeof(State)); s->span = 50; s->automatic = true; s->hasData = true; };
    s->hasData = true;
    Aircraft a; strcpy(a.hex, "test01"); strcpy(a.flight, "TEST001");
    a.east = 3; a.north = 4; a.hasTrack = a.hasSpeed = true; a.track = 90; a.speed = 720;
    mergeSnapshot(*s, &a, 1, 1000);
    check(visibleCount(*s, 1000) == 1);
    const auto initial = pose(s->aircraft[0], 1000), moving = pose(s->aircraft[0], 6000);
    check(fabsf(moving.east - initial.east - 1) < .001 && fabsf(moving.north - initial.north) < .001);
    mergeSnapshot(*s, nullptr, 0, 11000);
    check(visibleCount(*s, 11000) == 1 && retained(s->aircraft[0], 11000));
    mergeSnapshot(*s, nullptr, 0, 21000);
    check(visibleCount(*s, 21000) == 1 && s->aircraft[0].missingSince == 11000);
    check(visibleCount(*s, 41000) == 0); // empty polls do not extend grace
    a.east = 7.3f; a.track = 10;
    const auto before = pose(s->aircraft[0], 22000);
    mergeSnapshot(*s, &a, 1, 22000);
    const auto after = pose(s->aircraft[0], 22000);
    check(fabsf(before.east - after.east) < .001 && fabsf(before.north - after.north) < .001);
    check(!s->aircraft[0].missing && visibleCount(*s, 22000) == 1);
    auto frozen = pose(s->aircraft[0], 54000), later = pose(s->aircraft[0], 60000);
    check(fabsf(frozen.east - later.east) < .001 && fabsf(frozen.north - later.north) < .001);
    check(visibleCount(*s, 97000) == 0); // absolute age expires during feed loss
    reset(); a.positionAge = 0; a.track = 350; a.east = a.north = 0;
    mergeSnapshot(*s, &a, 1, 1000); a.track = 10;
    mergeSnapshot(*s, &a, 1, 2000);
    const float middle = pose(s->aircraft[0], 2750).track;
    check(middle < 1 || middle > 359); // 350 -> 10 turns through north
    a.positionAge = 20; a.east = 20;
    mergeSnapshot(*s, &a, 1, 3000);
    check(s->aircraft[0].east == 0 && s->aircraft[0].sampledAt == 2000);
    reset(); a.positionAge = 2; a.hasTrack = a.hasSpeed = false;
    a.east = 4; a.north = 1;
    mergeSnapshot(*s, &a, 1, 0xFFFFFF00U);
    check(fabsf(positionAge(s->aircraft[0], 0x000003E8U) - 3.256f) < .001);
    check(pose(s->aircraft[0], 0x000003E8U).east == 4); // unknown motion freezes
    reset(); a.positionAge = 0; a.east = 30;
    mergeSnapshot(*s, &a, 1, 1000);
    check(visibleCount(*s, 1000) == 0); // no edge pinning
    reset(); a.east = 0;
    mergeSnapshot(*s, &a, 1, 1000);
    for (uint32_t t = 11000; t < 71000; t += 10000) {
        a.positionAge = (t - 1000) / 1000.0f;
        mergeSnapshot(*s, &a, 1, t);
    }
    check(visibleCount(*s, 76000) == 0); // same old fix cannot renew lifetime

    // A normal delayed poll keeps moving beyond the old 20-second cutoff.
    reset(); a.positionAge = 0; a.hasTrack = a.hasSpeed = true;
    a.east = a.north = 0; a.track = 90; a.speed = 720;
    mergeSnapshot(*s, &a, 1, 1000);
    check(fabsf(pose(s->aircraft[0], 26000).east - 5) < .001);

    // A small correction is continuous and adds at most 15% ground speed,
    // even halfway through its easing curve (where the old burst peaked).
    a.east = 5.2f;
    mergeSnapshot(*s, &a, 1, 26000);
    check(fabsf(pose(s->aircraft[0], 26000).east - 5) < .001 &&
          s->aircraft[0].positionCorrectionMs >= 9990 && s->aircraft[0].positionCorrectionMs <= 10010);
    auto speedBounded = [&](uint32_t start, float speed, float allowance) {
        auto previous = pose(s->aircraft[0], start);
        const uint32_t duration = s->aircraft[0].positionCorrectionMs;
        for (uint32_t dt = 50; dt <= duration + 1000; dt += 50) {
            const auto current = pose(s->aircraft[0], start + dt);
            const float kmh = hypotf(current.east - previous.east, current.north - previous.north) * 72000;
            if (kmh > speed + allowance + 1 || kmh < speed - allowance - 1) return false;
            previous = current;
        }
        return true;
    };
    check(speedBounded(26000, 720, 108));
    const uint32_t settledAt = 26000 + s->aircraft[0].positionCorrectionMs;
    check(fabsf(pose(s->aircraft[0], settledAt).east -
          (5.2f + .2f * (settledAt - 26000) / 1000.0f)) < .001);

    // A fix behind the prediction slows gently instead of reversing/rushing.
    reset(); a.east = 0;
    mergeSnapshot(*s, &a, 1, 1000); a.east = 1.8f;
    mergeSnapshot(*s, &a, 1, 11000);
    check(speedBounded(11000, 720, 108));

    // At higher speeds the absolute correction cap is 120 km/h.
    reset(); a.east = 0; a.speed = 900;
    mergeSnapshot(*s, &a, 1, 1000); a.east = 2.8f;
    mergeSnapshot(*s, &a, 1, 11000);
    check(s->aircraft[0].positionCorrectionMs >= 13490 && speedBounded(11000, 900, 120));

    // After prediction has frozen, reacquisition uses the fresh fix directly.
    reset(); a.east = 0; a.speed = 720;
    mergeSnapshot(*s, &a, 1, 1000);
    check(fabsf(pose(s->aircraft[0], 33000).east - 6) < .001);
    a.east = 6.4f;
    mergeSnapshot(*s, &a, 1, 33000);
    check(s->aircraft[0].positionCorrectionMs == 0 &&
          fabsf(pose(s->aircraft[0], 33000).east - 6.4f) < .001);
    check(fabsf(pose(s->aircraft[0], 34000).east - 6.6f) < .001);

    // A large position disagreement must never become a fast catch-up flight.
    reset(); a.east = 0;
    mergeSnapshot(*s, &a, 1, 1000); a.east = 20;
    mergeSnapshot(*s, &a, 1, 11000);
    check(s->aircraft[0].positionCorrectionMs == 0 &&
          fabsf(pose(s->aircraft[0], 11000).east - 20) < .001 &&
          fabsf(pose(s->aircraft[0], 12500).east - 20.3f) < .001);

    // Unknown/stationary velocity is not invented by a correction animation.
    reset(); a.east = 0; a.hasSpeed = false;
    mergeSnapshot(*s, &a, 1, 1000); a.east = 10;
    mergeSnapshot(*s, &a, 1, 11000);
    check(s->aircraft[0].positionCorrectionMs == 0 && pose(s->aircraft[0], 12500).east == 10);

    // A correction cannot keep moving after the incoming fix's age cutoff.
    reset(); a.east = 0; a.hasSpeed = true;
    mergeSnapshot(*s, &a, 1, 1000); a.east = .1f; a.positionAge = 28;
    mergeSnapshot(*s, &a, 1, 30000);
    check(s->aircraft[0].positionCorrectionMs == 0 &&
          fabsf(pose(s->aircraft[0], 32000).east - pose(s->aircraft[0], 40000).east) < .001);

    // Correction time also remains bounded through the 32-bit clock wrap.
    reset(); a.east = 0; a.positionAge = 0;
    mergeSnapshot(*s, &a, 1, 0xFFFFF000U); a.east = .868f;
    mergeSnapshot(*s, &a, 1, 0xFFFFFF00U);
    const auto wrapBefore = pose(s->aircraft[0], 0xFFFFFF00U);
    const auto wrapAfter = pose(s->aircraft[0], 0x000000F4U);
    check(s->aircraft[0].positionCorrectionMs >= minimumCorrectionMs &&
          wrapAfter.east > wrapBefore.east && wrapAfter.east - wrapBefore.east <= .1151f);
    delete s;
    return result;
}
}
