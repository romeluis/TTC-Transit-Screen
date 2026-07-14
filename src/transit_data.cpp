#include "transit_data.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "config.h"

const char *fetchStatusName(FetchStatus s) {
  switch (s) {
  case FetchStatus::Ok:
    return "ok";
  case FetchStatus::NoService:
    return "no service";
  case FetchStatus::HttpError:
    return "http error";
  case FetchStatus::ParseError:
    return "parse error";
  }
  return "?";
}

// Both TTC endpoints are public read-only data. Certificate validation is
// intentionally disabled; see the security disclosure in the README.
static bool httpGetJson(const String &url, JsonDocument &doc) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent(HTTP_USER_AGENT);
  if (!http.begin(client, url)) {
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  http GET %s -> %d\r\n", url.c_str(), code);
    http.end();
    return false;
  }
  // Read the full body (getString decodes chunked responses; getStream does
  // not, and ntas.ttc.ca sends chunked). Bodies here are well under 1KB.
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("  json parse failed: %s\r\n", err.c_str());
    return false;
  }
  return true;
}

static void storeArrivals(Departures &dep, const int *minutes, int count) {
  for (int i = 0; i < 3; i++) {
    dep.next[i] = count > i ? minutes[i] : -1;
  }
  dep.noService = count == 0;
  dep.fetchedAtMs = millis();
}

FetchStatus fetchSubway(int stopCode, Departures &dep) {
  JsonDocument doc;
  if (!httpGetJson(String(NTAS_BASE_URL) + stopCode, doc)) {
    return FetchStatus::HttpError;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    return FetchStatus::ParseError;
  }
  // Empty array = no data for this platform (API quirk: still HTTP 200).
  if (arr.size() == 0) {
    storeArrivals(dep, nullptr, 0);
    return FetchStatus::NoService;
  }

  // "nextTrains" is a CSV of minutes, e.g. "1, 4, 6".
  const char *nextTrains = arr[0]["nextTrains"];
  if (nextTrains == nullptr) {
    return FetchStatus::ParseError;
  }
  int minutes[3];
  int count = 0;
  for (const char *p = nextTrains; *p != '\0' && count < 3;) {
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) {
      break;
    }
    minutes[count++] = (int)v;
    p = end;
    while (*p == ',' || *p == ' ') {
      p++;
    }
  }
  storeArrivals(dep, minutes, count);
  return count > 0 ? FetchStatus::Ok : FetchStatus::NoService;
}

// NextBus quirk: single-element lists are emitted as a bare object instead of
// an array. This walks a value either way, calling fn on each element.
template <typename F> static void forEachVariant(JsonVariant v, F fn) {
  if (v.isNull()) {
    return;
  }
  if (v.is<JsonArray>()) {
    for (JsonVariant item : v.as<JsonArray>()) {
      fn(item);
    }
  } else {
    fn(v);
  }
}

FetchStatus fetchNextBus(const char *route, int stopTag, Departures &dep) {
  char url[160];
  snprintf(url, sizeof(url), NEXTBUS_BASE_URL "&r=%s&s=%d", route, stopTag);
  JsonDocument doc;
  if (!httpGetJson(url, doc)) {
    return FetchStatus::HttpError;
  }

  JsonVariant predictions = doc["predictions"];
  if (predictions.isNull()) {
    return FetchStatus::ParseError;
  }
  // No upcoming vehicles: "direction" is absent and
  // "dirTitleBecauseNoPredictions" appears instead.
  JsonVariant directions = predictions["direction"];
  if (directions.isNull()) {
    storeArrivals(dep, nullptr, 0);
    return FetchStatus::NoService;
  }

  // Collect the three soonest predictions across all direction blocks
  // (short-turn branches can show up as separate blocks). NextBus reports
  // both "minutes" and "seconds"; sort by seconds for precision.
  constexpr int kBest = 3;
  int bestSec[kBest] = {INT_MAX, INT_MAX, INT_MAX};
  int bestMin[kBest] = {-1, -1, -1};
  forEachVariant(directions, [&](JsonVariant dir) {
    forEachVariant(dir["prediction"], [&](JsonVariant pred) {
      const char *minStr = pred["minutes"];
      if (minStr == nullptr) {
        return;
      }
      int m = atoi(minStr);
      const char *secStr = pred["seconds"];
      int s = secStr != nullptr ? atoi(secStr) : m * 60;
      for (int i = 0; i < kBest; i++) {
        if (s >= bestSec[i]) {
          continue;
        }
        for (int j = kBest - 1; j > i; j--) {
          bestSec[j] = bestSec[j - 1];
          bestMin[j] = bestMin[j - 1];
        }
        bestSec[i] = s;
        bestMin[i] = m;
        break;
      }
    });
  });

  int minutes[kBest];
  int count = 0;
  for (int i = 0; i < kBest; i++) {
    if (bestSec[i] != INT_MAX) {
      minutes[count++] = bestMin[i];
    }
  }
  storeArrivals(dep, minutes, count);
  return count > 0 ? FetchStatus::Ok : FetchStatus::NoService;
}

FetchStatus fetchAlerts(const StopConfig *stops, size_t count,
                        AlertState &alerts) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  // HTTP/1.0 disables chunked encoding so the ~100KB body can be parsed
  // straight off the stream instead of being buffered in RAM.
  http.useHTTP10(true);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent(HTTP_USER_AGENT);
  if (!http.begin(client, ALERTS_URL)) {
    return FetchStatus::HttpError;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("  http GET alerts -> %d\r\n", code);
    http.end();
    return FetchStatus::HttpError;
  }

  // Keep only the fields we act on; everything else is skipped during parse.
  JsonDocument filter;
  JsonObject f = filter["routes"].add<JsonObject>();
  f["route"] = true;
  f["severity"] = true;
  f["activePeriodGroup"] = true;
  f["elevatorCode"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("  alerts parse failed: %s\r\n", err.c_str());
    return FetchStatus::ParseError;
  }

  bool flagged[MAX_STOPS] = {};
  if (count > MAX_STOPS) {
    count = MAX_STOPS;
  }
  for (JsonObject a : doc["routes"].as<JsonArray>()) {
    const char *route = a["route"];
    const char *severity = a["severity"];
    if (route == nullptr || severity == nullptr) {
      continue;
    }
    // Elevator/escalator outages aren't line disruptions.
    if (!a["elevatorCode"].isNull()) {
      continue;
    }
    // Anything above Minor warrants the warning; Minor is dominated by the
    // ever-present slow-zone notices.
    if (strcmp(severity, "Minor") == 0 || strcmp(severity, "Info") == 0) {
      continue;
    }
    bool current = false;
    for (JsonVariant grp : a["activePeriodGroup"].as<JsonArray>()) {
      if (grp == "Current") {
        current = true;
      }
    }
    if (!current) {
      continue;
    }
    // Both directions of a line share the route string, so both get flagged.
    for (size_t i = 0; i < count; i++) {
      if (strcmp(route, stops[i].route) == 0) {
        flagged[i] = true;
      }
    }
  }
  for (size_t i = 0; i < count; i++) {
    alerts.alert[i] = flagged[i];
  }
  alerts.fetchedAtMs = millis();
  return FetchStatus::Ok;
}
