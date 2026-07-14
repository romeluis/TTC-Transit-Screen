#include "settings.h"

#include <ArduinoJson.h>
#include <Preferences.h>

StopConfig gStops[MAX_STOPS];
size_t gStopCount = 0;
ScreenSettings gScreen;

// One namespace, three keys: "ssid"/"pass" strings and "stops", a versioned
// JSON blob ({"v":1,"stops":[{"k":0,"r":"1","s":13815,"d":"Vaughan"},...]}).
// JSON keeps the write atomic and matches the web API payload shape.
static Preferences prefs;
static constexpr int STOPS_SCHEMA_V = 1;

bool settingsParseStops(const String &json, StopConfig *out, size_t &outCount) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    return false;
  }
  if (doc["v"].as<int>() != STOPS_SCHEMA_V) {
    return false;
  }
  JsonArray arr = doc["stops"].as<JsonArray>();
  if (arr.isNull() || arr.size() > MAX_STOPS) {
    return false;
  }
  size_t n = 0;
  for (JsonObject s : arr) {
    int kind = s["k"] | -1;
    const char *route = s["r"];
    int stopId = s["s"] | 0;
    const char *dest = s["d"] | "";
    if ((kind != 0 && kind != 1) || route == nullptr || route[0] == '\0' ||
        strlen(route) > ROUTE_MAX_LEN || stopId <= 0 ||
        strlen(dest) > DEST_MAX_LEN) {
      return false;
    }
    out[n].kind = kind == 0 ? RouteKind::Subway : RouteKind::Surface;
    strlcpy(out[n].route, route, sizeof(out[n].route));
    out[n].stopId = stopId;
    strlcpy(out[n].dest, dest, sizeof(out[n].dest));
    n++;
  }
  outCount = n;
  return true;
}

static String serializeStops(const StopConfig *stops, size_t count) {
  JsonDocument doc;
  doc["v"] = STOPS_SCHEMA_V;
  JsonArray arr = doc["stops"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    JsonObject s = arr.add<JsonObject>();
    s["k"] = stops[i].kind == RouteKind::Subway ? 0 : 1;
    s["r"] = stops[i].route;
    s["s"] = stops[i].stopId;
    s["d"] = stops[i].dest;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String settingsStopsJson() { return serializeStops(gStops, gStopCount); }

void settingsBegin() {
  prefs.begin("ttc", false);

  String json = prefs.getString("stops", "");
  if (json.isEmpty() || !settingsParseStops(json, gStops, gStopCount)) {
    if (!json.isEmpty()) {
      Serial.println("[settings] stored stops invalid, ignoring");
    }
    gStopCount = 0; // fresh device: no stops until the web UI adds some
  }
  Serial.printf("[settings] %u stops loaded\r\n", (unsigned)gStopCount);

  gScreen.sleepEnabled = prefs.getBool("slp_en", true);
  gScreen.offHour = prefs.getUChar("slp_off", SCREEN_OFF_START_HOUR);
  gScreen.onHour = prefs.getUChar("slp_on", SCREEN_OFF_END_HOUR);
  gScreen.wakeSecs = prefs.getUShort("wake_s", NIGHT_WAKE_SECS);
}

void settingsSaveScreen(const ScreenSettings &s) {
  prefs.putBool("slp_en", s.sleepEnabled);
  prefs.putUChar("slp_off", s.offHour);
  prefs.putUChar("slp_on", s.onHour);
  prefs.putUShort("wake_s", s.wakeSecs);
  gScreen = s;
}

bool settingsSaveStops(const StopConfig *stops, size_t count) {
  if (count > MAX_STOPS) {
    return false;
  }
  String json = serializeStops(stops, count);
  if (prefs.putString("stops", json) != json.length()) {
    Serial.println("[settings] NVS write failed");
    return false;
  }
  if (stops != gStops) {
    memcpy(gStops, stops, count * sizeof(StopConfig));
  }
  gStopCount = count;
  return true;
}

bool settingsGetWifi(String &ssid, String &pass) {
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  return !ssid.isEmpty();
}

void settingsSetWifi(const char *ssid, const char *pass) {
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
}

void settingsFactoryReset() {
  Serial.println("[settings] factory reset");
  prefs.clear();
  delay(100);
  ESP.restart();
}
