// Fetching and parsing of TTC arrival predictions.
#pragma once

#include <Arduino.h>

#include "config.h" // MAX_STOPS, StopConfig

// Result of one fetch attempt (for logging).
enum class FetchStatus : uint8_t {
  Ok,        // parsed at least one arrival time
  NoService, // API answered but reports no upcoming vehicles
  HttpError, // non-200 / connection failure
  ParseError // 200 but body didn't match the expected shape
};

struct Departures {
  int next[3] = {-1, -1, -1}; // minutes to next arrivals; -1 = unknown
  uint32_t fetchedAtMs = 0;   // millis() of last successful fetch
  bool noService = false;

  bool isStale(uint32_t nowMs, uint32_t staleAfterMs) const {
    return fetchedAtMs == 0 || nowMs - fetchedAtMs > staleAfterMs;
  }
};

struct TransitState {
  Departures deps[MAX_STOPS]; // parallel to STOPS
};

// Per-stop "is something wrong" flags from the TTC live-alerts feed.
struct AlertState {
  bool alert[MAX_STOPS] = {}; // parallel to STOPS
  uint32_t fetchedAtMs = 0;
};

// Fetch next-train times for one subway platform from the NTAS API.
// On Ok/NoService `dep` is updated (including fetchedAtMs); on errors it is
// left untouched so the display can keep showing the last good data.
FetchStatus fetchSubway(int stopCode, Departures &dep);

// Fetch streetcar/bus predictions for one stop from the NextBus/UMO feed.
FetchStatus fetchNextBus(const char *route, int stopTag, Departures &dep);

// Fetch TTC live alerts and flag each configured stop whose route has a
// current alert above Minor severity (Minor = the ever-present slow zones;
// elevator/escalator outages are also ignored).
FetchStatus fetchAlerts(const StopConfig *stops, size_t count,
                        AlertState &alerts);

const char *fetchStatusName(FetchStatus s);
