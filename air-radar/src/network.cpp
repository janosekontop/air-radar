#include "network.h"
#include "airport_cities.h"
#include "route_policy.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <math.h>
#include <algorithm>
#include <DNSServer.h>
#include "web_security.h"

extern const uint8_t caBundle[] asm("_binary_data_x509_crt_bundle_start");
extern const char setupPage[] asm("_binary_data_setup_html_start");
extern const char routeCa[] asm("_binary_data_route_ca_pem_start");

namespace Network {
namespace {
Preferences preferences;
WebServer server(80);
DNSServer dns;
KeyHandler handleKey;
SemaphoreHandle_t mutex;
QueueHandle_t results;
QueueHandle_t routeResults;
QueueHandle_t routeProbes;
char ssid[33] = {}, password[65] = {};
char apName[32], apPassword[17], token[33];
bool changePending, apActive, connectedBefore;
uint32_t lastAttempt, connectedSince, lostSince;
unsigned requestedSpan = 50;
uint32_t displayedRevision = 0, configuredAt = 0;
char chosenCallsign[13] = {};
uint32_t wifiUpdates;
int lastHttp;
char lastError[80] = {};
char lastRouteCallsign[13] = {}, lastRouteStatus[32] = "NOT_REQUESTED";
int lastRouteHttp;
uint32_t routeRequests, routeSuccesses, lastRouteMs;

struct PsramAllocator {
    void *allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
    void deallocate(void *p) { heap_caps_free(p); }
    void *reallocate(void *p, size_t size) { return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
using Document = BasicJsonDocument<PsramAllocator>;

struct Result {
    Radar::Aircraft aircraft[Radar::capacity];
    size_t count;
    uint64_t timestamp;
    uint32_t received;
    bool ok;
    uint32_t revision;
};

struct Route : Routes::Metadata {
    double lat1, lon1, lat2, lon2;
    uint32_t expires;
    bool known;
    uint32_t revision;
};
Route *routes;
constexpr size_t routeCapacity = 64;
size_t routeCount;

bool due(uint32_t now, uint32_t when) { return static_cast<int32_t>(now - when) >= 0; }
bool numeric(JsonVariantConst value) { return value.is<double>() && isfinite(value.as<double>()); }

void text(char *out, size_t size, const char *in, bool city = false) {
    size_t n = 0;
    if (in) for (size_t i = 0; in[i] && n + 1 < size; ++i) {
        const unsigned char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            (city && (c == ' ' || c == '.' || c == '(' || c == ')'))) out[n++] = c;
    }
    out[n] = 0;
}

class Body : public Stream {
public:
    char *bytes = nullptr;
    size_t length = 0, capacity = 0, maximum;
    explicit Body(size_t limit) : maximum(limit) {}
    ~Body() { heap_caps_free(bytes); }
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *data, size_t count) override {
        if (length + count > maximum) return 0;
        if (length + count + 1 > capacity) {
            const size_t wanted = std::min(maximum + 1, std::max(length + count + 1, capacity * 2 + 4096));
            auto *next = static_cast<char *>(heap_caps_realloc(bytes, wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!next) return 0;
            bytes = next; capacity = wanted;
        }
        memcpy(bytes + length, data, count); length += count; bytes[length] = 0;
        return count;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

int download(const String &url, Body &body, uint32_t &retry, bool aircraft) {
    retry = aircraft ? 10000 : 15000;
    // A single network worker owns TLS; Arduino 2.x's CA bundle is global.
    WiFiClientSecure client;
    // The route CDN sends a cross-signed GTS R4 chain. Arduino 2.x's
    // compressed-bundle callback only checks the last untrusted issuer;
    // a full, currently trusted R4 root lets mbedTLS anchor that chain early.
    if (aircraft) client.setCACertBundle(caBundle);
    else client.setCACert(routeCa);
    client.setHandshakeTimeout(aircraft ? 10 : 4);
    client.setTimeout(aircraft ? 10 : 4);
    HTTPClient http;
    http.setConnectTimeout(aircraft ? 10000 : 4000);
    http.setTimeout(aircraft ? 10000 : 4000);
    http.useHTTP10(true);
    http.setReuse(false);
    const char *headers[] = {"Retry-After"};
    http.collectHeaders(headers, 1);
    if (!http.begin(client, url)) return -1;
    // HTTPClient deliberately ignores User-Agent in addHeader().
    http.setUserAgent("AirRadar/3.0 (10-second polling)");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    const int code = http.GET();
    if (code == 429) retry = std::max<uint32_t>(60000, std::min<uint32_t>(600000, http.header("Retry-After").toInt() * 1000));
    else if (code == 401 || code == 403) retry = 300000;
    else if (code != 200 && code != 404) retry = 60000;
    int result = code;
    if (code == 200 && (http.getSize() > static_cast<int>(body.maximum) || http.writeToStream(&body) < 0 || !body.length)) result = -2;
    http.end();
    return result;
}

double angle(double lat1, double lon1, double lat2, double lon2) {
    const double a = pow(sin((lat2-lat1)/2), 2) + cos(lat1)*cos(lat2)*pow(sin((lon2-lon1)/2), 2);
    return 2 * asin(sqrt(std::max(0.0, std::min(1.0, a))));
}

bool plausible(const Route &r, const Radar::Aircraft &a, const Configuration::Snapshot &center) {
    constexpr double radians = 0.0174532925199433;
    const double lat1=r.lat1*radians, lon1=r.lon1*radians, lat2=r.lat2*radians, lon2=r.lon2*radians;
    const double lat=center.latitude + a.north / (6371.0088*radians);
    const double lon=center.longitude + a.east / (6371.0088*radians*cos((lat+center.latitude)*0.5*radians));
    auto bearing = [](double p, double x, double q, double y) {
        return atan2(sin(y-x)*cos(q), cos(p)*sin(q)-sin(p)*cos(q)*cos(y-x));
    };
    const double length=angle(lat1,lon1,lat2,lon2)*6371.0088;
    if (length < 1) return false;
    const double tolerance=std::max(92.6,length*0.2), ap=angle(lat1,lon1,lat*radians,lon*radians);
    const double difference=bearing(lat1,lon1,lat*radians,lon*radians)-bearing(lat1,lon1,lat2,lon2);
    const double cross=fabs(asin(std::max(-1.0,std::min(1.0,sin(ap)*sin(difference)))))*6371.0088;
    const double along=atan2(sin(ap)*cos(difference),cos(ap))*6371.0088;
    return cross <= tolerance && along >= -tolerance && along <= length+tolerance;
}

Route *cached(const char *callsign) {
    for (size_t i=0; i<routeCount; ++i) if (!strcmp(routes[i].callsign,callsign)) return &routes[i];
    return nullptr;
}

void enrich(Radar::Aircraft &a, const Configuration::Snapshot &center) {
    const auto *r=cached(a.callsign);
    if (!r || due(millis(),r->expires) || !r->known || !plausible(*r,a,center)) return;
    Routes::apply(a,*r);
}

bool normalize(JsonDocument &raw, unsigned span, Result &result, const Configuration::Snapshot &center) {
    if (!numeric(raw["now"]) || !raw["ac"].is<JsonArray>()) return false;
    const double stamp=raw["now"].as<double>();
    const double feedAge=time(nullptr)-stamp/1000.0;
    if (stamp < 1000000000000.0 || feedAge > 30 || feedAge < -10) return false;
    result.timestamp=static_cast<uint64_t>(stamp); result.count=0;
    for (JsonObjectConst item : raw["ac"].as<JsonArrayConst>()) {
        if (!numeric(item["lat"]) || !numeric(item["lon"]) || !numeric(item["seen_pos"])) continue;
        if (!strcmp(item["alt_baro"] | "", "ground")) continue;
        const double lat=item["lat"], lon=item["lon"];
        const double age=item["seen_pos"].as<double>()+std::max(0.0,feedAge);
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || age < 0 || age >= 30) continue;
        Radar::Aircraft a;
        text(a.hex,sizeof(a.hex),item["hex"] | "");
        text(a.callsign,sizeof(a.callsign),item["flight"] | "");
        text(a.flight,sizeof(a.flight),a.callsign[0] ? a.callsign : item["r"] | "");
        text(a.model,sizeof(a.model),item["t"] | "");
        if (!a.hex[0]) continue;
        constexpr double radians=0.0174532925199433;
        a.north=(lat-center.latitude)*radians*6371.0088;
        a.east=remainder(lon-center.longitude,360.0)*radians*6371.0088*cos((lat+center.latitude)*0.5*radians);
        if (fabs(a.east)>span/2.0 || fabs(a.north)>span/2.0) continue;
        a.distance=hypotf(a.east,a.north); a.positionAge=age;
        a.hasTrack=numeric(item["track"]) && item["track"].as<float>()>=0 && item["track"].as<float>()<360;
        a.track=item["track"] | 0.0f;
        a.hasSpeed=numeric(item["gs"]) && item["gs"].as<float>()>=0 && item["gs"].as<float>()<=1600;
        a.speed=(item["gs"] | 0.0f)*1.852f;
        bool duplicate=false;
        for (size_t i=0;i<result.count;++i) if (!strcmp(result.aircraft[i].hex,a.hex)) duplicate=true;
        if (duplicate) continue;
        size_t at=result.count;
        if (at==Radar::capacity) {
            at=0;
            for(size_t i=1;i<result.count;++i) if(result.aircraft[i].distance>result.aircraft[at].distance) at=i;
            if(a.distance>=result.aircraft[at].distance) continue;
        } else ++result.count;
        enrich(a,center); result.aircraft[at]=a;
    }
    std::sort(result.aircraft,result.aircraft+result.count,[](const Radar::Aircraft &a,const Radar::Aircraft &b){return a.distance<b.distance;});
    return true;
}

void airport(JsonObjectConst a, char *code, char *city) {
    const char *iata=a["iata"] | "", *icao=a["icao"] | "";
    text(code,5,strlen(iata)==3 ? iata : icao);
    city[0]=0;
    for (const auto &known : airportCities) if (!strcmp(code,known.code)) {strlcpy(city,known.city,48);return;}
    text(city,48,a["location"] | "",true);
}

void lookup(const Radar::Aircraft &a, uint32_t &nextRoute, const Configuration::Snapshot &center) {
    if(!Routes::eligible(a.callsign)) {nextRoute=millis()+15000;return;}
    const uint32_t started=millis();
    xSemaphoreTake(mutex,portMAX_DELAY);
    strlcpy(lastRouteCallsign,a.callsign,sizeof(lastRouteCallsign));
    strlcpy(lastRouteStatus,"REQUESTING",sizeof(lastRouteStatus));lastRouteHttp=0;lastRouteMs=0;++routeRequests;
    xSemaphoreGive(mutex);
    Body body(131072);
    uint32_t retry;
    const String url=String("https://vrs-standing-data.adsb.lol/routes/")+String(a.callsign).substring(0,2)+"/"+a.callsign+".json";
    const int code=download(url,body,retry,false);
    nextRoute=millis()+retry;
    Route entry={};
    strlcpy(entry.callsign,a.callsign,sizeof(entry.callsign));entry.revision=center.revision;
    entry.expires=millis()+(code==404 ? 300000 : 60000);
    const char *reason=code==404 ? "NOT_FOUND" : code<0 ? "NETWORK_ERROR" : "HTTP_ERROR";
    if (code==200) {
        Document doc(16384);
        const auto error=deserializeJson(doc,body.bytes,body.length);
        if(error) reason="INVALID_JSON";
        else if(strcmp(doc["callsign"] | "",a.callsign)) reason="CALLSIGN_MISMATCH";
        else if(doc.containsKey("plausible") && doc["plausible"]!=true) reason="SOURCE_REJECTED";
        else if(!doc["_airports"].is<JsonArray>() || doc["_airports"].size()!=2)
            reason=doc["_airports"].size()>2 ? "MULTI_STOP" : "UNKNOWN_ROUTE";
        else {
            reason="INVALID_AIRPORTS";
            auto from=doc["_airports"][0].as<JsonObjectConst>(), to=doc["_airports"][1].as<JsonObjectConst>();
            const String codes=String(from["icao"] | "")+"-"+(to["icao"] | "");
            if(codes==String(doc["airport_codes"] | "") && numeric(from["lat"]) && numeric(from["lon"]) && numeric(to["lat"]) && numeric(to["lon"])) {
                airport(from,entry.origin,entry.originCity); airport(to,entry.destination,entry.destinationCity);
                entry.lat1=from["lat"];entry.lon1=from["lon"];entry.lat2=to["lat"];entry.lon2=to["lon"];
                entry.known=entry.origin[0] && entry.destination[0] && fabs(entry.lat1)<=90 && fabs(entry.lat2)<=90 && fabs(entry.lon1)<=180 && fabs(entry.lon2)<=180 && plausible(entry,a,center);
                entry.expires=millis()+(entry.known ? 900000 : 300000);
                reason=entry.known ? "OK" : "ROUTE_UNRELATED";
            }
        }
    }
    if(center.revision!=Configuration::get().revision) return;
    auto *slot=cached(a.callsign);
    if (!slot) {
        if(routeCount<routeCapacity) slot=&routes[routeCount++];
        else {slot=&routes[0];for(size_t i=1;i<routeCount;++i)if(static_cast<int32_t>(routes[i].expires-slot->expires)<0)slot=&routes[i];}
    }
    *slot=entry;
    // Deliver metadata independently; waiting for the next position poll added
    // another 10+ seconds and could lose the result when the aircraft left view.
    if(entry.known) xQueueSend(routeResults,&entry,0);
    xSemaphoreTake(mutex,portMAX_DELAY);
    lastRouteHttp=code;lastRouteMs=millis()-started;
    strlcpy(lastRouteStatus,reason,sizeof(lastRouteStatus));
    if(entry.known) ++routeSuccesses;
    xSemaphoreGive(mutex);
}

void publish(Result *result) {
    Result *old=nullptr;
    if (xQueueReceive(results,&old,0)==pdTRUE) heap_caps_free(old);
    if (xQueueSend(results,&result,0)!=pdTRUE) heap_caps_free(result);
}

void worker(void *) {
    uint32_t nextFetch=0,nextRoute=0;
    unsigned failures=0;
    uint32_t revision=0;
    auto *last=static_cast<Result *>(heap_caps_calloc(1,sizeof(Result),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    for (;;) {
        const auto center=Configuration::get();
        if(revision!=center.revision) {revision=center.revision;routeCount=0;nextFetch=nextRoute=0;if(last)last->ok=false;}
        if (!center.configured || !center.wifi || WiFi.status()!=WL_CONNECTED || time(nullptr)<1700000000) {vTaskDelay(pdMS_TO_TICKS(250));nextFetch=0;continue;}
        unsigned span;
        char selected[13];
        xSemaphoreTake(mutex,portMAX_DELAY);span=requestedSpan;strlcpy(selected,chosenCallsign,sizeof(selected));xSemaphoreGive(mutex);
        if (due(millis(),nextFetch)) {
            auto *result=static_cast<Result *>(heap_caps_calloc(1,sizeof(Result),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
            if (!result || !last) {heap_caps_free(result);vTaskDelay(pdMS_TO_TICKS(1000));continue;}
            const unsigned radius=ceil(span*0.5*sqrt(2.0)/1.852)+1;
            Body body(2*1024*1024);
            uint32_t retry;
            const String url=String("https://api.adsb.lol/v2/point/")+String(center.latitude,6)+"/"+String(center.longitude,6)+"/"+radius;
            const int code=download(url,body,retry,true);
            bool valid=false;
            if (code==200) {
                Document doc(131072);
                StaticJsonDocument<512> filter;
                filter["now"]=true;
                for (const char *field : {"hex","flight","r","t","lat","lon","seen_pos","gs","track","alt_baro"}) filter["ac"][0][field]=true;
                valid=!deserializeJson(doc,body.bytes,body.length,DeserializationOption::Filter(filter)) && normalize(doc,span,*result,center);
            }
            if(center.revision!=Configuration::get().revision) {heap_caps_free(result);continue;}
            result->revision=center.revision;
            xSemaphoreTake(mutex,portMAX_DELAY);
            lastHttp=code;
            if (valid) {lastError[0]=0;++wifiUpdates;failures=0;}
            else {snprintf(lastError,sizeof(lastError),code==200 ? "Aircraft data is unavailable or expired" : "Internet request failed (HTTP %d)",code);++failures;}
            xSemaphoreGive(mutex);
            result->ok=valid;result->received=millis();
            if(valid) *last=*result;
            if(!valid) retry=std::max<uint32_t>(retry,std::min<uint32_t>(120000,10000U << std::min(failures-1,4U)));
            nextFetch=millis()+retry;
            publish(result);
        }
        // Route requests run between position polls and have shorter timeouts.
        if (last && last->ok && millis()-last->received<Radar::staleMs && due(millis(),nextRoute) && static_cast<int32_t>(nextFetch-millis())>4000) {
            Radar::Aircraft probe;
            if(xQueueReceive(routeProbes,&probe,0)==pdTRUE) lookup(probe,nextRoute,center);
            else {
                bool needed[Radar::capacity]={};
                for (size_t i=0;i<last->count;++i) {
                    const auto &a=last->aircraft[i];
                    auto *r=cached(a.callsign);
                    needed[i]=!r || due(millis(),r->expires);
                }
                const int target=Routes::choose(last->aircraft,last->count,selected,needed);
                if(target>=0) lookup(last->aircraft[target],nextRoute,center);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void securityHeaders() {
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("X-Content-Type-Options","nosniff");
    server.sendHeader("X-Frame-Options","DENY");
    server.sendHeader("Referrer-Policy","no-referrer");
    server.sendHeader("Content-Security-Policy",pageCsp);
}
void apStart() {
    if(apActive)return;
    apActive=WiFi.softAP(apName,apPassword,1,false,2);
    if(apActive)dns.start(53,"*",WiFi.softAPIP());
}
bool localSetup() {
    if(!apActive || server.client().localIP()!=WiFi.softAPIP()) {
        securityHeaders();server.send(403,"application/json","{\"error\":\"Use USB setup or join the radar setup hotspot\"}");return false;
    }
    const String host=server.hostHeader();
    if(host!="192.168.4.1" && host!="192.168.4.1:80") {
        server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");return false;
    }
    return true;
}
void respondInfo() {
    if(!localSetup())return;
    StaticJsonDocument<2048> doc;info(doc);doc.remove("ap_password");doc["token"]=token;doc["transport"]="hotspot";
    String body;serializeJson(doc,body);securityHeaders();server.send(200,"application/json",body);
}
bool request(StaticJsonDocument<1024> &doc) {
    if(!localSetup())return false;
    const String origin=server.header("Origin");
    const String body=server.arg("plain");
    if((origin.length() && origin!="http://192.168.4.1") ||
       !server.header("Content-Type").startsWith("application/json") ||
       !body.length() || body.length()>1024 || body.indexOf("\\u0000")>=0 || deserializeJson(doc,body)) {
        server.send(400,"application/json","{\"error\":\"Invalid setup request\"}");return false;
    }
    const char *received=doc["token"] | "";
    unsigned difference=strlen(received)^strlen(token);
    if(strlen(received)==strlen(token))for(size_t i=0;i<strlen(token);++i)difference|=received[i]^token[i];
    if(difference){server.send(403,"application/json","{\"error\":\"Reload the setup page and try again\"}");return false;}
    securityHeaders();return true;
}
}

bool configure(const char *network, const char *secret) {
    if(!Configuration::credentials(network,secret))return false;
    strlcpy(ssid,network,sizeof(ssid));strlcpy(password,secret,sizeof(password));changePending=true;
    return true;
}

bool applyConfiguration(JsonDocument &doc) {
    const char *mode=doc["mode"] | "";
    if(strcmp(mode,"wifi") && strcmp(mode,"usb"))return false;
    if(!doc["latitude"].is<double>() || !doc["longitude"].is<double>())return false;
    const double lat=doc["latitude"],lon=doc["longitude"];
    if(!Configuration::coordinates(lat,lon))return false;
    if(doc.containsKey("keep_wifi") && !doc["keep_wifi"].is<bool>())return false;
    const bool wifi=!strcmp(mode,"wifi");
    char nextSsid[33],nextPassword[65];
    strlcpy(nextSsid,ssid,sizeof(nextSsid));strlcpy(nextPassword,password,sizeof(nextPassword));
    if(wifi && !(doc["keep_wifi"] | false)) {
        if(!doc["ssid"].is<const char *>() || !doc["password"].is<const char *>())return false;
        if(!Configuration::credentials(doc["ssid"],doc["password"]))return false;
        strlcpy(nextSsid,doc["ssid"],sizeof(nextSsid));strlcpy(nextPassword,doc["password"],sizeof(nextPassword));
    } else if(wifi && !Configuration::credentials(nextSsid,nextPassword))return false;
    StaticJsonDocument<768> saved;
    saved["lat"]=lat;saved["lon"]=lon;saved["wifi"]=wifi;saved["ssid"]=nextSsid;saved["password"]=nextPassword;
    String encoded;serializeJson(saved,encoded);
    // NVS commits the whole settings value. A power loss cannot mix old and new fields.
    const bool written=preferences.putString("settings",encoded)==encoded.length();
    saved.clear();for(size_t i=0;i<encoded.length();++i)encoded.setCharAt(i,0);
    if(!written){memset(nextPassword,0,sizeof(nextPassword));return false;}
    strlcpy(ssid,nextSsid,sizeof(ssid));strlcpy(password,nextPassword,sizeof(password));memset(nextPassword,0,sizeof(nextPassword));
    Configuration::publish(lat,lon,wifi,true);changePending=true;configuredAt=millis();
    return true;
}
void resetConfiguration() {
    preferences.clear();ssid[0]=password[0]=0;
    Configuration::publish(0,0,true,false);changePending=true;
    WiFi.disconnect(false,false);apStart();
}
void openSetup(){apStart();connectedSince=millis();configuredAt=millis();}

bool probeRoute(const char *callsign, double lat, double lon) {
    if(!routeProbes || !Routes::eligible(callsign) || !isfinite(lat) || !isfinite(lon) || fabs(lat)>90 || fabs(lon)>180) return false;
    const auto center=Configuration::get();
    Radar::Aircraft probe;
    strlcpy(probe.callsign,callsign,sizeof(probe.callsign));
    constexpr double radians=0.0174532925199433;
    probe.north=(lat-center.latitude)*radians*6371.0088;
    probe.east=remainder(lon-center.longitude,360.0)*radians*6371.0088*cos((lat+center.latitude)*.5*radians);
    // A diagnostic lookup never inserts an aircraft or alters the position feed.
    return xQueueSend(routeProbes,&probe,0)==pdTRUE;
}

void info(JsonDocument &doc) {
    doc.clear();doc["event"]="network";doc["ssid"]=ssid;
    const auto center=Configuration::get();
    doc["configured"]=center.configured;doc["has_wifi"]=bool(ssid[0]);doc["mode"]=center.wifi ? "wifi" : "usb";
    if(center.configured){doc["latitude"]=center.latitude;doc["longitude"]=center.longitude;}
    doc["firmware"]="Air Radar 3.0.0";doc["connected"]=WiFi.status()==WL_CONNECTED;
    doc["ip"]=WiFi.localIP().toString();doc["ap"]=apName;doc["ap_active"]=apActive;
    doc["ap_password"]=apPassword;doc["ap_ip"]="192.168.4.1";
    if (mutex) {
        xSemaphoreTake(mutex,portMAX_DELAY);doc["wifi_updates"]=wifiUpdates;doc["http"]=lastHttp;doc["error"]=lastError;
        doc["route_callsign"]=lastRouteCallsign;doc["route_http"]=lastRouteHttp;doc["route_status"]=lastRouteStatus;
        doc["route_requests"]=routeRequests;doc["route_successes"]=routeSuccesses;doc["route_ms"]=lastRouteMs;
        xSemaphoreGive(mutex);
    } else doc["error"]="Wi-Fi memory allocation failed";
    doc["https_verified"]=true;
}

void begin(KeyHandler handler) {
    handleKey=handler;mutex=xSemaphoreCreateMutex();results=xQueueCreate(1,sizeof(Result *));routeResults=xQueueCreate(4,sizeof(Route));routeProbes=xQueueCreate(1,sizeof(Radar::Aircraft));
    routes=static_cast<Route *>(heap_caps_calloc(routeCapacity,sizeof(Route),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!mutex || !results || !routeResults || !routeProbes || !routes) {Serial.println("{\"event\":\"error\",\"message\":\"Wi-Fi memory allocation failed\"}");return;}
    preferences.begin("air-radar",false);
    String encoded=preferences.getString("settings","");
    StaticJsonDocument<768> saved;
    bool ready=false,wifi=true;double lat=0,lon=0;
    if(encoded.length() && !deserializeJson(saved,encoded) && saved["lat"].is<double>() && saved["lon"].is<double>() && saved["wifi"].is<bool>()) {
        lat=saved["lat"];lon=saved["lon"];wifi=saved["wifi"];
        strlcpy(ssid,saved["ssid"] | "",sizeof(ssid));strlcpy(password,saved["password"] | "",sizeof(password));
        ready=Configuration::coordinates(lat,lon) && (!wifi || Configuration::credentials(ssid,password));
    } else if(!encoded.length()) {
        // Compatibility with the initial 3.0 development settings on this hardware.
        lat=preferences.getDouble("lat",0);lon=preferences.getDouble("lon",0);wifi=preferences.getBool("wifi",true);
        preferences.getString("ssid",ssid,sizeof(ssid));preferences.getString("password",password,sizeof(password));
        ready=preferences.getBool("ready",false) && Configuration::coordinates(lat,lon) && (!wifi || Configuration::credentials(ssid,password));
    }
    saved.clear();for(size_t i=0;i<encoded.length();++i)encoded.setCharAt(i,0);
    Configuration::publish(lat,lon,wifi,ready);
    preferences.getString("ap-key",apPassword,sizeof(apPassword));
    if(strlen(apPassword)!=16) {snprintf(apPassword,sizeof(apPassword),"%08lx%08lx",static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));preferences.putString("ap-key",apPassword);}
    const uint64_t mac=ESP.getEfuseMac();snprintf(apName,sizeof(apName),"Air-Radar-%06lx",static_cast<unsigned long>(mac>>24)&0xFFFFFF);
    snprintf(token,sizeof(token),"%08lx%08lx%08lx%08lx",static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
    WiFi.persistent(false);WiFi.setHostname("air-radar");WiFi.mode(WIFI_AP_STA);WiFi.setAutoReconnect(true);WiFi.setSleep(false);
    if(!Configuration::get().configured)apStart();
    changePending=true;
    const char *headers[]={"Origin","Content-Type"};server.collectHeaders(headers,2);
    server.on("/",HTTP_GET,[]{if(!localSetup())return;securityHeaders();server.send_P(200,"text/html",setupPage);});
    server.on("/api/info",HTTP_GET,respondInfo);
    server.on("/api/config",HTTP_POST,[]{
        StaticJsonDocument<1024> doc;if(!request(doc))return;
        const bool ok=applyConfiguration(doc);doc.clear();
        server.send(ok ? 200 : 400,"application/json",ok ? "{\"ok\":true}" : "{\"error\":\"Check location, data mode and Wi-Fi details\"}");
    });
    server.on("/api/reset",HTTP_POST,[]{StaticJsonDocument<1024> doc;if(!request(doc))return;resetConfiguration();server.send(200,"application/json","{\"ok\":true}");});
    server.on("/api/key",HTTP_POST,[]{StaticJsonDocument<1024> doc;if(!request(doc))return;const char *key=doc["key"] | "";if(strlen(key)!=1 || !strchr("+-na[]",key[0])){server.send(400,"application/json","{\"error\":\"Unknown control\"}");return;}handleKey(key[0]);server.send(200,"application/json","{\"ok\":true}");});
    server.onNotFound([]{if(!localSetup())return;server.sendHeader("Location","http://192.168.4.1/");server.send(302,"text/plain","");});server.begin();
    if (xTaskCreatePinnedToCore(worker,"radar-net",12288,nullptr,1,nullptr,0)!=pdPASS) {
        strlcpy(lastError,"Unable to start the network worker",sizeof(lastError));
        Serial.println("{\"event\":\"error\",\"message\":\"Unable to start the network worker\"}");
    }
}

bool loop(Radar::State &state, uint64_t &sourceTimestamp) {
    if(!mutex || !results || !routeResults || !routeProbes || !routes) return false;
    server.handleClient();if(apActive)dns.processNextRequest();
    const uint32_t now=millis();
    const auto center=Configuration::get();
    if(displayedRevision!=center.revision) {
        displayedRevision=center.revision;sourceTimestamp=0;state.count=0;state.hasData=false;state.feedError=false;
        state.received=0;state.selected[0]=0;state.automatic=true;state.wifi=center.wifi;state.needsSetup=!center.configured;
        xSemaphoreTake(mutex,portMAX_DELAY);wifiUpdates=0;lastHttp=0;lastError[0]=0;xSemaphoreGive(mutex);
    }
    if(changePending) {
        changePending=false;
        WiFi.disconnect(false,false);if(center.configured && center.wifi && ssid[0])WiFi.begin(ssid,password);lastAttempt=now;connectedSince=0;connectedBefore=false;
    }
    const bool connected=WiFi.status()==WL_CONNECTED;
    if(connected && !connectedBefore) {connectedSince=now;configTime(0,0,"pool.ntp.org","time.cloudflare.com");}
    if(!connected) {
        if(connectedBefore) lostSince=now;
        if(center.configured && center.wifi && ssid[0] && now-lastAttempt>=30000) {WiFi.disconnect(false,false);WiFi.begin(ssid,password);lastAttempt=now;}
        if(!center.configured || center.wifi && (!ssid[0] || now-lastAttempt>=20000 || lostSince && now-lostSince>=30000))apStart();
    } else {
        lostSince=0;
        if(apActive && center.configured && now-connectedSince>=180000) {dns.stop();WiFi.softAPdisconnect(true);apActive=false;}
    }
    connectedBefore=connected;
    if(apActive)snprintf(state.setupHint,sizeof(state.setupHint),"SETUP: %s | KEY: %s | 192.168.4.1",apName,apPassword);
    else state.setupHint[0]=0;
    xSemaphoreTake(mutex,portMAX_DELAY);
    requestedSpan=state.span;
    const int selected=Radar::selectedIndex(state,now);
    strlcpy(chosenCallsign,selected>=0 ? state.aircraft[selected].callsign : "",sizeof(chosenCallsign));
    xSemaphoreGive(mutex);
    Result *result=nullptr;bool updated=false;
    if(xQueueReceive(results,&result,0)==pdTRUE) {
        const bool usbActive = state.hasData && !state.wifi && now - state.received < 15000;
        if(result->revision==center.revision && center.wifi && result->ok && result->timestamp>sourceTimestamp && !usbActive) {
            sourceTimestamp=result->timestamp;
            Radar::mergeSnapshot(state,result->aircraft,result->count,result->received);
            state.received=result->received;state.hasData=true;state.feedError=false;state.wifi=true;++state.sequence;++state.updates;updated=true;
        } else if(result->revision==center.revision && !result->ok)state.feedError=true;
        heap_caps_free(result);
    }
    Route route={};
    if(xQueueReceive(routeResults,&route,0)==pdTRUE && state.wifi && route.known && route.revision==center.revision && !due(now,route.expires)) {
        unsigned applied=0;
        for(size_t i=0;i<state.count;++i)
            if(!strcmp(state.aircraft[i].callsign,route.callsign) && plausible(route,state.aircraft[i],center) && Routes::apply(state.aircraft[i],route)) ++applied;
        Serial.printf("{\"event\":\"route\",\"callsign\":\"%s\",\"origin\":\"%s\",\"destination\":\"%s\",\"applied\":%u}\n",route.callsign,route.origin,route.destination,applied);
    }
    return updated;
}
}
