#include "radar.h"
#include "aircraft_icon.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace Radar {
namespace {
lv_obj_t *plot, *header, *details, *metrics, *message, *scale;
const State *current;
uint32_t currentTime;
constexpr uint32_t green = 0x62E8B2, muted = 0x7895A2, amber = 0xFFC375;
constexpr uint32_t pink = 0xFF55B7, speedBlue = 0x35AAFF;
constexpr uint32_t radarBackground = 0x000000;
constexpr uint32_t radarGrid = 0x00D000;
constexpr int plotSize = 184;

lv_obj_t *label(const char *text, int x, int y, int width, uint32_t color) {
    auto *o = lv_label_create(lv_scr_act());
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, width);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    return o;
}

void line(lv_draw_ctx_t *ctx, int x1, int y1, int x2, int y2,
          uint32_t color, int width = 1, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color); d.width = width; d.opa = opa;
    lv_point_t a = {static_cast<lv_coord_t>(x1), static_cast<lv_coord_t>(y1)};
    lv_point_t b = {static_cast<lv_coord_t>(x2), static_cast<lv_coord_t>(y2)};
    lv_draw_line(ctx, &d, &a, &b);
}

void rectangle(lv_draw_ctx_t *ctx, int x1, int y1, int x2, int y2, uint32_t color, lv_opa_t opacity = LV_OPA_COVER) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color); d.bg_opa = opacity;
    lv_area_t area = {static_cast<lv_coord_t>(x1), static_cast<lv_coord_t>(y1),
                      static_cast<lv_coord_t>(x2), static_cast<lv_coord_t>(y2)};
    lv_draw_rect(ctx, &d, &area);
}

void polygon(lv_draw_ctx_t *ctx, const float *vx, const float *vy, size_t count, uint32_t color) {
    float low = vy[0], high = vy[0];
    for (size_t i = 1; i < count; ++i) { low = std::min(low, vy[i]); high = std::max(high, vy[i]); }
    for (int row = floorf(low); row < ceilf(high); ++row) {
        float hits[8]; size_t n = 0;
        const float scan = row + 0.5f;
        for (size_t i = 0; i < count; ++i) {
            const size_t j = (i + 1) % count;
            if ((vy[i] <= scan && vy[j] > scan) || (vy[j] <= scan && vy[i] > scan))
                hits[n++] = vx[i] + (scan - vy[i]) * (vx[j] - vx[i]) / (vy[j] - vy[i]);
        }
        std::sort(hits, hits + n);
        for (size_t i = 0; i + 1 < n; i += 2) {
            const int left = ceilf(hits[i] - .5f), right = ceilf(hits[i + 1] - .5f) - 1;
            if (right >= left) rectangle(ctx, left, row, right, row, color);
        }
    }
}

void selection(lv_draw_ctx_t *ctx, float x, float y, float track) {
    // Four identical mirrored L shapes share the plane's exact animated pose.
    // Plot clipping handles edges; do not shift the reticle away from its plane.
    constexpr float corner[][2] = {{-17,-17},{-9,-17},{-9,-15},{-15,-15},{-15,-9},{-17,-9}};
    const float angle = track * .0174532925199433f, c = cosf(angle), sn = sinf(angle);
    for (int sx : {-1, 1}) for (int sy : {-1, 1}) {
        float vx[6], vy[6];
        for (int i = 0; i < 6; ++i) {
            const float u = corner[i][0] * sx, v = corner[i][1] * sy;
            vx[i] = x + u * c - v * sn; vy[i] = y + u * sn + v * c;
        }
        polygon(ctx, vx, vy, 6, 0xFFFFFF);
    }
}

void speedText(const Aircraft &a, char *out, size_t size) {
    if (a.hasSpeed) snprintf(out, size, "%s%.0f km/h", retained(a, currentTime) ? "~" : "", a.speed);
    else snprintf(out, size, "-- km/h");
}

struct FlightLabel { lv_area_t area; int x, y; size_t aircraft; char hex[9]; };
FlightLabel placed[capacity], previous[capacity];
size_t placedCount;
lv_img_dsc_t aircraftImage;
uint8_t aircraftPixels[AircraftIcon::width * AircraftIcon::height * 3];

int overlap(const lv_area_t &a, const lv_area_t &b) {
    const int width = std::min(a.x2, b.x2) - std::max(a.x1, b.x1) + 1;
    const int height = std::min(a.y2, b.y2) - std::max(a.y1, b.y1) + 1;
    return width > 0 && height > 0 ? width * height : 0;
}

void placeFlightLabels(const lv_area_t &box, int cx, int cy, float pixelsPerKm, int selected) {
    const size_t previousCount = placedCount;
    memcpy(previous, placed, previousCount * sizeof(FlightLabel));
    size_t count = 0;
    lv_area_t icons[capacity];
    size_t iconOwners[capacity];
    size_t iconCount = 0;
    for (size_t i = 0; i < current->count; ++i) {
        const auto &a = current->aircraft[i];
        if (!visible(a, *current, currentTime)) continue;
        const auto point = pose(a, currentTime);
        const int x = lroundf(cx + point.east * pixelsPerKm), y = lroundf(cy - point.north * pixelsPerKm);
        const int margin = static_cast<int>(i) == selected ? 24 : 17;
        iconOwners[iconCount] = i;
        icons[iconCount++] = {static_cast<lv_coord_t>(x - margin), static_cast<lv_coord_t>(y - margin),
                              static_cast<lv_coord_t>(x + margin), static_cast<lv_coord_t>(y + margin)};
    }
    // Give the active label first choice, then retain an identifier for every plane.
    for (size_t pass = 0; pass <= current->count; ++pass) {
        const int index = pass == 0 ? selected : static_cast<int>(pass - 1);
        if (index < 0 || (pass && index == selected)) continue;
        const auto &a = current->aircraft[index];
        if (!visible(a, *current, currentTime)) continue;
        const char *text = a.flight[0] ? a.flight : a.hex;
        const auto point = pose(a, currentTime);
        const int x = lroundf(cx + point.east * pixelsPerKm), y = lroundf(cy - point.north * pixelsPerKm);
        lv_point_t size;
        lv_txt_get_size(&size, text, &lv_font_montserrat_10, 0, 0, 180, LV_TEXT_FLAG_NONE);
        char speed[24]; speedText(a, speed, sizeof(speed));
        lv_point_t speedSize;
        lv_txt_get_size(&speedSize, speed, &lv_font_montserrat_10, 0, 0, 180, LV_TEXT_FLAG_NONE);
        const int width = std::max(size.x, speedSize.x) + 4, height = size.y + speedSize.y + 4;
        int preferredLeft = 0, preferredTop = 0; bool hasPrevious = false;
        for (size_t i = 0; i < previousCount; ++i) if (!strcmp(previous[i].hex, a.hex)) {
            preferredLeft = previous[i].area.x1 + x - previous[i].x;
            preferredTop = previous[i].area.y1 + y - previous[i].y;
            hasPrevious = true; break;
        }
        lv_area_t best = {};
        float bestScore = 1e20f;
        auto candidate = [&](int left, int top) {
            left = std::max<int>(box.x1 + 2, std::min<int>(left, box.x2 - width - 1));
            top = std::max<int>(box.y1 + 2, std::min<int>(top, box.y2 - height - 1));
            lv_area_t area = {static_cast<lv_coord_t>(left), static_cast<lv_coord_t>(top),
                              static_cast<lv_coord_t>(left + width - 1), static_cast<lv_coord_t>(top + height - 1)};
            const int nearX = std::max<int>(left, std::min<int>(x, area.x2));
            const int nearY = std::max<int>(top, std::min<int>(y, area.y2));
            float score = hypotf(nearX - x, nearY - y);
            if (hasPrevious) score += 2 * hypotf(left - preferredLeft, top - preferredTop);
            for (size_t i = 0; i < count; ++i) score += overlap(area, placed[i].area) * 100;
            for (size_t i = 0; i < iconCount; ++i)
                score += overlap(area, icons[i]) * (iconOwners[i] == static_cast<size_t>(index) ? 500 : 20);
            const lv_area_t north = {112, 24, 130, 39}, south = {112, 191, 130, 207};
            score += (overlap(area, north) + overlap(area, south)) * 100;
            if (score < bestScore) { bestScore = score; best = area; }
        };
        // Keep labels beside their own plane. Collision avoidance only chooses
        // between nearby side positions; it must never send a label across the plot.
        const int gap = index == selected ? 25 : 18;
        for (int offset : {0, -6, 6, -height, height}) {
            candidate(x + gap, y - height / 2 + offset);
            candidate(x - gap - width, y - height / 2 + offset);
        }
        placed[count] = {best, x, y, static_cast<size_t>(index), {}};
        strlcpy(placed[count].hex, a.hex, sizeof(placed[count].hex));
        ++count;
    }
    placedCount = count;
}

void drawFlightLabels(lv_draw_ctx_t *ctx, int selected) {
    for (size_t i = 0; i < placedCount; ++i) {
        const auto &p = placed[i];
        rectangle(ctx, p.area.x1, p.area.y1, p.area.x2, p.area.y2, 0x001A0B);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = &lv_font_montserrat_10;
        d.color = lv_color_hex(static_cast<int>(p.aircraft) == selected ? 0xFFFFFF : 0xD5FFE5);
        lv_area_t textArea = p.area; textArea.x1 += 2; textArea.y1 += 1;
        const auto &a = current->aircraft[p.aircraft];
        lv_draw_label(ctx, &d, &textArea, a.flight[0] ? a.flight : a.hex, nullptr);
        textArea.y1 += lv_font_montserrat_10.line_height + 1;
        d.color = lv_color_hex(speedBlue);
        char speed[24]; speedText(a, speed, sizeof(speed));
        lv_draw_label(ctx, &d, &textArea, speed, nullptr);
    }
}

void airplane(lv_draw_ctx_t *ctx, float x, float y, float track, bool hasTrack, bool held) {
    lv_draw_img_dsc_t image;
    lv_draw_img_dsc_init(&image);
    image.angle = hasTrack ? lroundf(track * 10) : 0;
    image.pivot = {AircraftIcon::width / 2, AircraftIcon::height / 2};
    image.antialias = 1;
    image.opa = held ? LV_OPA_70 : LV_OPA_COVER;
    const int left = lroundf(x) - AircraftIcon::width / 2;
    const int top = lroundf(y) - AircraftIcon::height / 2;
    lv_area_t area = {static_cast<lv_coord_t>(left), static_cast<lv_coord_t>(top),
        static_cast<lv_coord_t>(left + AircraftIcon::width - 1), static_cast<lv_coord_t>(top + AircraftIcon::height - 1)};
    lv_draw_img(ctx, &image, &area, &aircraftImage);
    if (!hasTrack) {
        lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
        d.font = &lv_font_montserrat_10; d.color = lv_color_hex(0xFFFFFF);
        lv_area_t area = {static_cast<lv_coord_t>(x+12),static_cast<lv_coord_t>(y-12),
                          static_cast<lv_coord_t>(x+20),static_cast<lv_coord_t>(y)};
        lv_draw_label(ctx, &d, &area, "?", nullptr);
    }
}

void draw(lv_event_t *event) {
    if (!current) return;
    auto *ctx = lv_event_get_draw_ctx(event);
    lv_area_t box;
    lv_obj_get_coords(plot, &box);
    // Explicit plot clipping prevents aircraft at the edges covering the labels.
    lv_area_t clip;
    if (!_lv_area_intersect(&clip, ctx->clip_area, &box)) return;
    const lv_area_t *oldClip = ctx->clip_area;
    ctx->clip_area = &clip;
    const int cx = box.x1 + plotSize / 2, cy = box.y1 + plotSize / 2;
    const float pixelsPerKm = (plotSize - 4.0f) / current->span;
    for (int i = 1; i < 4; ++i) {
        const int at = i * plotSize / 4;
        line(ctx, box.x1 + at, box.y1, box.x1 + at, box.y2, radarGrid);
        line(ctx, box.x1, box.y1 + at, box.x2, box.y1 + at, radarGrid);
    }
    // A five-kilometre neighbourhood, not an animated sensor sweep.
    const int radius = lroundf(5 * pixelsPerKm);
    int px = cx + radius, py = cy;
    for (int degree = 6; degree <= 360; degree += 6) {
        const float angle = degree * 0.0174532925f;
        const int nx = lroundf(cx + radius * cosf(angle));
        const int ny = lroundf(cy + radius * sinf(angle));
        line(ctx, px, py, nx, ny, 0x00E000);
        px = nx; py = ny;
    }
    const int selected = selectedIndex(*current, currentTime);
    for (size_t i = 0; i < current->count; ++i) {
        const auto &a = current->aircraft[i];
        if (!visible(a, *current, currentTime)) continue;
        const auto point = pose(a, currentTime);
        airplane(ctx, lroundf(cx + point.east * pixelsPerKm), lroundf(cy - point.north * pixelsPerKm),
                 point.track, a.hasTrack, retained(a, currentTime));
    }
    drawFlightLabels(ctx, selected);
    if (selected >= 0) {
        const auto &a = current->aircraft[selected];
        const auto point = pose(a, currentTime);
        selection(ctx, lroundf(cx + point.east * pixelsPerKm), lroundf(cy - point.north * pixelsPerKm), point.track);
    }
    // Home stays readable when an aircraft is directly over the centre.
    line(ctx, cx - 4, cy, cx + 4, cy, amber, 2);
    line(ctx, cx, cy - 4, cx, cy + 4, amber, 2);
    ctx->clip_area = oldClip;
}
}

const char *status(const State &s, uint32_t now) {
    if(s.needsSetup)return "SETUP";
    if (!s.hasData) return s.feedError ? "OFFLINE" : s.wifi ? "CONNECTING" : "WAIT USB";
    if (now - s.received >= staleMs) return "STALE";
    return s.feedError ? "RETRY" : s.wifi ? "LIVE WIFI" : "LIVE USB";
}

void uiInit() {
    // Expand the 264-byte alpha mask once; LVGL rotates RGB565+alpha natively.
    const uint16_t color = lv_color_hex(pink).full;
    for (int y = 0; y < AircraftIcon::height; ++y) for (int x = 0; x < AircraftIcon::width; ++x) {
        const int i = (y * AircraftIcon::width + x) * 3;
        aircraftPixels[i] = color & 255; aircraftPixels[i + 1] = color >> 8;
        aircraftPixels[i + 2] = AircraftIcon::coverage(x,y);
    }
    aircraftImage.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    aircraftImage.header.w = AircraftIcon::width; aircraftImage.header.h = AircraftIcon::height;
    aircraftImage.data_size = sizeof(aircraftPixels); aircraftImage.data = aircraftPixels;
    auto *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x030F0A), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    label("RADAR", 8, 4, 68, green);
    header = label("0  WiFi", 85, 4, 150, muted);
    lv_obj_set_style_text_align(header, LV_TEXT_ALIGN_RIGHT, 0);
    plot = lv_obj_create(screen);
    lv_obj_remove_style_all(plot);
    lv_obj_set_pos(plot, 28, 24);
    lv_obj_set_size(plot, plotSize, plotSize);
    lv_obj_set_style_bg_color(plot, lv_color_hex(radarBackground), 0);
    lv_obj_set_style_bg_opa(plot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(plot, 1, 0);
    lv_obj_set_style_border_color(plot, lv_color_hex(0x00D000), 0);
    lv_obj_clear_flag(plot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(plot, draw, LV_EVENT_DRAW_MAIN, nullptr);
    label("N", 115, 25, 12, 0xB9FFD6);
    label("W", 8, 110, 18, muted);
    label("E", 218, 110, 18, muted);
    label("S", 115, 193, 12, 0xB9FFD6);
    scale = label("50", 2, 173, 26, muted);
    label("km", 2, 187, 26, muted);
    details = label("", 8, 211, 224, 0xFFFFFF);
    lv_obj_set_style_text_font(details, &lv_font_montserrat_10, 0);
    metrics = label("50 x 50 km  /  ADS-B.lol", 8, 226, 224, muted);
    lv_label_set_long_mode(metrics, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(metrics, 22, 0);
    message = label("No aircraft\nnearby", 32, 90, 176, 0xB9FFD6);
    lv_obj_set_style_text_font(message, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -4);
}

void uiUpdate(const State &s, uint32_t now) {
    current = &s; currentTime = now;
    const auto count = visibleCount(s, now);
    bool held = false;
    for (size_t i = 0; i < s.count; ++i) if (visible(s.aircraft[i], s, now) && retained(s.aircraft[i], now)) held = true;
    if (s.hasData && now - s.received < staleMs)
        lv_label_set_text_fmt(header, "%s%u  %s  %lus", held ? "~" : "", static_cast<unsigned>(count), s.wifi ? "WiFi" : "USB",
                              static_cast<unsigned long>((now - s.received) / 1000));
    else lv_label_set_text_fmt(header, "%s%u  %s", held ? "~" : "", static_cast<unsigned>(count),s.wifi ? "WiFi" : "USB");
    lv_obj_set_style_text_color(header, lv_color_hex(count ? green : muted), 0);
    lv_label_set_text_fmt(scale, "%u", s.span);
    const int selected = selectedIndex(s, now);
    if (selected >= 0) {
        const auto &a = s.aircraft[selected];
        // LVGL's compact formatter omits floating-point support by default.
        char speed[24];
        if (a.hasSpeed) snprintf(speed, sizeof(speed), "%.0f km/h", a.speed);
        else snprintf(speed, sizeof(speed), "-- km/h");
        const auto point = pose(a, now);
        const float distance = hypotf(point.east, point.north);
        char detailText[96];
        snprintf(detailText, sizeof(detailText), "%s %s %.1fkm %s%s", a.flight[0] ? a.flight : a.hex,
                 a.model[0] ? a.model : "---", distance, speed, distance <= 5 ? " NEAR" : "");
        lv_label_set_text(details, detailText);
        const auto city = [](const char *code, const char *name) {
            
            if (name[0]) return name;
            return code[0] ? code : "---";
        };
        char routeText[128];
        snprintf(routeText, sizeof(routeText), "%s%s > %s", a.origin[0] && a.destination[0] ? "~" : "",
                 city(a.origin, a.originCity), city(a.destination, a.destinationCity));
        // Replacing an unchanged string every frame restarts LVGL's marquee.
        if(s.setupHint[0])strlcpy(routeText,s.setupHint,sizeof(routeText));
        if (strcmp(lv_label_get_text(metrics), routeText)) lv_label_set_text(metrics, routeText);
        lv_label_set_text(message, "");
    } else {
        lv_label_set_text(details, "");
        char footer[128];
        if(s.setupHint[0]) strlcpy(footer,s.setupHint,sizeof(footer));
        else snprintf(footer,sizeof(footer),"%u x %u km  /  ADS-B.lol",s.span,s.span);
        if(strcmp(lv_label_get_text(metrics),footer)) lv_label_set_text(metrics,footer);
        lv_label_set_text(message, s.needsSetup ? "SET UP\nYOUR RADAR" : !s.hasData ? (s.wifi ? "CONNECTING\nTO WI-FI" : "WAITING\nFOR USB") : "No aircraft\nnearby");
    }
    lv_area_t box;
    lv_obj_get_coords(plot, &box);
    placeFlightLabels(box, box.x1 + plotSize / 2, box.y1 + plotSize / 2,
                      (plotSize - 4.0f) / s.span, selected);
    lv_obj_invalidate(plot);
}
}
