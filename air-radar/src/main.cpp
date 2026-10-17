#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <esp32s3/spiram.h>
#include <math.h>
#include <algorithm>
#include "radar.h"
#include "network.h"
#include "route_policy.h"

namespace {
TFT_eSPI tft;
lv_disp_draw_buf_t drawBuffer;
lv_disp_drv_t displayDriver;
lv_color_t pixels[240 * 24];
uint16_t *frame;
Radar::State state;
Radar::Aircraft incoming[Radar::capacity];
DynamicJsonDocument document(32768);
char input[32768];
size_t inputLength = 0;
bool discarding = false;
uint32_t lastTick = 0, lastUi = 0, lastLog = 0, refreshes = 0;
uint64_t sourceTimestamp = 0;
uint8_t brightness = 200, rotation = 2; // Prism mounting: half turn plus horizontal reflection.
constexpr bool mirrorHorizontally = true;

void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors) {
    const uint32_t width = area->x2 - area->x1 + 1;
    const uint32_t height = area->y2 - area->y1 + 1;
    const uint32_t left = mirrorHorizontally ? 239 - area->x2 : area->x1;
    // Reflect each complete rendered row and its destination together. This
    // mirrors text, planes and reticles, including partial redraws, for the prism.
    for (uint32_t row = 0; row < height; ++row) {
        auto *rowPixels = colors + row * width;
        if (mirrorHorizontally) std::reverse(rowPixels, rowPixels + width);
        if (frame) memcpy(frame + (area->y1 + row) * 240 + left, rowPixels, width * 2);
    }
    tft.startWrite();
    tft.setAddrWindow(left, area->y1, width, height);
    tft.pushColors(reinterpret_cast<uint16_t *>(colors), width * height, true);
    tft.endWrite();
    if (lv_disp_flush_is_last(driver)) ++refreshes;
    lv_disp_flush_ready(driver);
}

void settings() {
    Serial.printf("{\"event\":\"settings\",\"span\":%u,\"brightness\":%u,\"rotation\":%u,\"mirror_x\":%s}\n",
                  state.span, brightness, rotation, mirrorHorizontally ? "true" : "false");
}

void printStatus(const char *event = "status") {
    const uint32_t now = millis();
    const int chosen = Radar::selectedIndex(state, now);
    const Radar::Aircraft *a = chosen >= 0 ? &state.aircraft[chosen] : nullptr;
    Serial.printf("{\"event\":\"%s\",\"state\":\"%s\",\"span\":%u,\"count\":%u,\"seq\":%lu,\"updates\":%lu,\"age\":%lu,\"heap\":%lu,\"refreshes\":%lu,\"nearest\":\"%s\",\"distance\":%.2f}\n",
        event, Radar::status(state, now), state.span,
        static_cast<unsigned>(Radar::visibleCount(state, now)),
        static_cast<unsigned long>(state.sequence), static_cast<unsigned long>(state.updates),
        static_cast<unsigned long>(state.hasData ? (now - state.received) / 1000 : 0),
        static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(refreshes),
        a ? (a->flight[0] ? a->flight : a->hex) : "", a ? hypotf(Radar::pose(*a, now).east, Radar::pose(*a, now).north) : 0);
    if (a) {
        const auto point = Radar::pose(*a, now);
        Serial.printf("{\"event\":\"details\",\"flight\":\"%s\",\"model\":\"%s\",\"origin\":\"%s\",\"destination\":\"%s\",\"origin_city\":\"%s\",\"destination_city\":\"%s\",\"east\":%.4f,\"north\":%.4f,\"track\":%.2f,\"speed_kmh\":%.1f,\"has_speed\":%s,\"held\":%s,\"position_age\":%.2f}\n",
            a->flight, a->model, a->origin, a->destination, a->originCity, a->destinationCity,
            point.east, point.north, point.track, a->speed, a->hasSpeed ? "true" : "false",
            Radar::retained(*a, now) ? "true" : "false", Radar::positionAge(*a, now));
    }
}

void zoom(unsigned span) {
    if (span != 25 && span != 50 && span != 100) return;
    state.span = span;
    settings();
}

void nextAircraft() {
    const int selected = Radar::selectedIndex(state, millis());
    for (size_t offset = 1; offset <= state.count; ++offset) {
        const size_t i = (selected + offset + state.count) % state.count;
        if (!Radar::visible(state.aircraft[i], state, millis())) continue;
        strlcpy(state.selected, state.aircraft[i].hex, sizeof(state.selected));
        state.automatic = false;
        return;
    }
}

void screenshot() {
    if (!frame) { Serial.println("{\"event\":\"error\",\"message\":\"No capture buffer\"}"); return; }
    // Force the latest LVGL state into the same buffer sent to the LCD.
    Radar::uiUpdate(state, millis());
    lv_refr_now(nullptr);
    Serial.println("{\"event\":\"screenshot\",\"bytes\":115200,\"width\":240,\"height\":240,\"format\":\"rgb565le\"}");
    for (unsigned row = 0; row < 240; ++row) {
        Serial.write(reinterpret_cast<const uint8_t *>(frame + row * 240), 480);
        delay(1);
    }
    Serial.println();
}

void key(char command) {
    switch (command) {
        case '+': zoom(state.span == 100 ? 50 : 25); break;
        case '-': zoom(state.span == 25 ? 50 : 100); break;
        case 'n': nextAircraft(); break;
        case 'a': state.automatic = true; state.selected[0] = 0; break;
        case '[': brightness = max(16, brightness - 16); ledcWrite(0, brightness); settings(); break;
        case ']': brightness = min(255, brightness + 16); ledcWrite(0, brightness); settings(); break;
        case 'r': rotation = (rotation + 1) % 4; tft.setRotation(rotation); lv_obj_invalidate(lv_scr_act()); settings(); break;
        case 's': printStatus(); break;
        case 'p': screenshot(); break;
        case 'h': Serial.println("{\"event\":\"help\",\"keys\":\"+/- zoom, n next, a nearest, [/] brightness, r rotation, s status, p capture\"}"); break;
        default: break;
    }
}

bool number(JsonVariantConst value) {
    return value.is<double>() && isfinite(value.as<double>());
}

void copySafe(char *destination, size_t size, const char *source, bool city = false) {
    size_t out = 0;
    if (source) {
        for (size_t i = 0; source[i] && out + 1 < size; ++i) {
            const char c = source[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                (city && (c == ' ' || c == '(' || c == ')' || c == '.')))
                destination[out++] = c;
        }
    }
    destination[out] = 0;
}

void packet() {
    input[inputLength]=0;
    if(strstr(input,"\\u0000")) {Serial.println("{\"event\":\"rejected\",\"reason\":\"Invalid string\"}");return;}
    const auto error = deserializeJson(document, input, inputLength);
    if (error) { Serial.println("{\"event\":\"rejected\",\"reason\":\"Invalid JSON\"}"); return; }
    const char *type = document["type"] | "";
    if (!strcmp(type, "command")) {
        const char *name = document["name"] | "";
        if (!strcmp(name, "hello")) { settings(); printStatus("ready"); }
        else if (!strcmp(name, "wifi_info")) {
            StaticJsonDocument<1536> information;Network::info(information);serializeJson(information,Serial);Serial.println();
        }
        else if(!strcmp(name,"configure")) {
            const bool accepted=Network::applyConfiguration(document);
            document.clear();memset(input,0,inputLength);
            Serial.printf("{\"event\":\"configured\",\"ok\":%s}\n",accepted ? "true" : "false");
        }
        else if(!strcmp(name,"reset_config")) {Network::resetConfiguration();Serial.println("{\"event\":\"reset\",\"ok\":true}");}
        else if(!strcmp(name,"open_setup")) {Network::openSetup();Serial.println("{\"event\":\"setup_opened\",\"ok\":true}");}
        else if(!strcmp(name,"configuration_tests")) {const auto c=Configuration::checks();Serial.printf("{\"event\":\"configuration_tests\",\"passed\":%u,\"failed\":%u}\n",c.passed,c.failed);}
        else if (!strcmp(name, "tracking_tests")) {
            const auto checks = Radar::trackingChecks();
            Serial.printf("{\"event\":\"tracking_tests\",\"passed\":%u,\"failed\":%u}\n", checks.passed, checks.failed);
        }
        else if (!strcmp(name, "route_tests")) {
            const auto checks = Routes::checks();
            Serial.printf("{\"event\":\"route_tests\",\"passed\":%u,\"failed\":%u}\n", checks.passed, checks.failed);
        }
        else if (!strcmp(name, "route_probe")) {
            const bool accepted=number(document["lat"]) && number(document["lon"]) &&
                Network::probeRoute(document["callsign"] | "",document["lat"].as<double>(),document["lon"].as<double>());
            Serial.printf("{\"event\":\"route_probe_queued\",\"ok\":%s}\n",accepted ? "true" : "false");
        }
        else if (!strcmp(name, "zoom")) zoom(document["value"] | 50U);
        else if (!strcmp(name, "key")) key((document["value"] | " ")[0]);
        return;
    }
    if (!strcmp(type, "error")) {
        state.feedError = true;
        strlcpy(state.message, "Feed unavailable; retrying", sizeof(state.message));
        printStatus("feed_error");
        return;
    }
    const auto center=Configuration::get();
    if(!center.configured || center.wifi) {Serial.println("{\"event\":\"rejected\",\"reason\":\"Select USB mode in setup\"}");return;}
    if (strcmp(type, "aircraft") || (document["v"] | 0) != 1 || !document["ac"].is<JsonArray>() ||
        !number(document["lat"]) || !number(document["lon"]) ||
        fabs(document["lat"].as<double>() - center.latitude) > 0.00001 ||
        fabs(document["lon"].as<double>() - center.longitude) > 0.00001 ||
        !document["now"].is<uint64_t>()) {
        Serial.println("{\"event\":\"rejected\",\"reason\":\"Wrong packet or location\"}");
        return;
    }
    const uint64_t timestamp = document["now"].as<uint64_t>();
    // Replayed cached data must not reset the freshness timer.
    if (timestamp <= sourceTimestamp) {
        Serial.println("{\"event\":\"rejected\",\"reason\":\"Old timestamp\"}"); return;
    }
    sourceTimestamp = timestamp;
    size_t incomingCount = 0;
    for (JsonObjectConst item : document["ac"].as<JsonArrayConst>()) {
        if (incomingCount >= Radar::capacity) break;
        if (!number(item["lat"]) || !number(item["lon"]) || !number(item["age"])) continue;
        const double lat = item["lat"], lon = item["lon"];
        const float age = item["age"];
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || age < 0 || age >= 30) continue;
        auto &a = incoming[incomingCount];
        a = Radar::Aircraft{};
        copySafe(a.hex, sizeof(a.hex), item["hex"] | "");
        copySafe(a.flight, sizeof(a.flight), item["flight"] | "");
        copySafe(a.callsign, sizeof(a.callsign), item["callsign"] | "");
        copySafe(a.model, sizeof(a.model), item["model"] | "");
        copySafe(a.origin, sizeof(a.origin), item["origin"] | "");
        copySafe(a.destination, sizeof(a.destination), item["destination"] | "");
        copySafe(a.originCity, sizeof(a.originCity), item["origin_city"] | "", true);
        copySafe(a.destinationCity, sizeof(a.destinationCity), item["destination_city"] | "", true);
        if (strlen(a.origin) < 3 || strlen(a.destination) < 3)
            a.origin[0] = a.destination[0] = a.originCity[0] = a.destinationCity[0] = 0;
        if (!a.hex[0]) continue;
        constexpr double radians = 0.0174532925199433;
        a.north = (lat - center.latitude) * radians * 6371.0088;
        a.east = remainder(lon - center.longitude,360.0) * radians * 6371.0088 * cos((lat + center.latitude) * 0.5 * radians);
        a.distance = hypotf(a.north, a.east);
        a.positionAge = age;
        a.hasTrack = number(item["track"]) && item["track"].as<float>() >= 0 && item["track"].as<float>() < 360;
        a.track = item["track"] | 0.0f;
        a.hasSpeed = number(item["speed"]) && item["speed"].as<float>() >= 0 && item["speed"].as<float>() <= 3000;
        a.speed = item["speed"] | 0.0f;
        ++incomingCount;
    }
    state.received = millis();
    Radar::mergeSnapshot(state, incoming, incomingCount, state.received);
    state.sequence = document["seq"] | 0U;
    ++state.updates;
    state.hasData = true; state.feedError = false;
    state.wifi = false;
    strlcpy(state.message, "USB aircraft feed", sizeof(state.message));
    printStatus("feed");
}

void readSerial() {
    // Limit work per frame so large packets cannot monopolise the display loop.
    for (unsigned budget = 0; budget < 2048 && Serial.available(); ++budget) {
        const char c = Serial.read();
        if (c == '\n') {
            if (!discarding && inputLength) packet();
            inputLength = 0; discarding = false;
        } else if (c == '\r') {
            continue;
        } else if (discarding) {
            continue;
        } else if (inputLength == 0 && c != '{') {
            key(c);
        } else if (inputLength + 1 < sizeof(input)) {
            input[inputLength++] = c;
        } else {
            discarding = true;
            Serial.println("{\"event\":\"rejected\",\"reason\":\"Packet too large\"}");
        }
    }
}
}

void setup() {
    Serial.setRxBufferSize(4096);
    Serial.begin(115200);
    delay(250);
    if (psramFound()) frame = static_cast<uint16_t *>(heap_caps_calloc(240 * 240, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    tft.begin();
    tft.setRotation(rotation); tft.invertDisplay(true); tft.fillScreen(TFT_BLACK);
    ledcSetup(0, 5000, 8); ledcAttachPin(TFT_BL, 0); ledcWrite(0, brightness);
    lv_init();
    lv_disp_draw_buf_init(&drawBuffer, pixels, nullptr, 240 * 24);
    lv_disp_drv_init(&displayDriver);
    displayDriver.hor_res = 240; displayDriver.ver_res = 240;
    displayDriver.flush_cb = flushDisplay; displayDriver.draw_buf = &drawBuffer;
    lv_disp_drv_register(&displayDriver);
    Radar::uiInit(); Radar::uiUpdate(state, millis());
    lastTick = millis();
    Serial.printf("{\"event\":\"boot\",\"firmware\":\"Air Radar 3.0.0\",\"flash\":%lu,\"psram\":%lu,\"capture\":%s,\"color_order\":\"BGR\"}\n",
                  static_cast<unsigned long>(ESP.getFlashChipSize()),
                  static_cast<unsigned long>(psramFound() ? esp_spiram_get_size() : 0), frame ? "true" : "false");
    settings();
    Network::begin(key);
}

void loop() {
    const uint32_t tick = millis();
    lv_tick_inc(tick - lastTick); lastTick = tick;
    readSerial();
    if(Network::loop(state,sourceTimestamp)) printStatus("wifi_feed");
    const uint32_t now = millis();
    if (now - lastUi >= 50) { lastUi = now; Radar::updateSelection(state, now); Radar::uiUpdate(state, now); }
    lv_timer_handler();
    if (now - lastLog >= 2000) { lastLog = now; printStatus(); }
    delay(2);
}
