// Persistent settings (NVS via Preferences) and the runtime stop table.
//
// The rest of the firmware reads gStops/gStopCount instead of the old
// compile-time STOPS[] array; the web-config UI rewrites them at runtime.
#pragma once

#include <Arduino.h>

#include "config.h"

// Live stop table. Mutated only from the main loop (the synchronous web
// server's handlers run there too, so no locking is needed).
extern StopConfig gStops[MAX_STOPS];
extern size_t gStopCount;

// Screen-sleep schedule, edited in the web UI. The main loop reads it every
// pass, so saves take effect immediately.
struct ScreenSettings {
  bool sleepEnabled; // false = screen stays on around the clock
  uint8_t offHour;   // local hour (0-23) the screen turns off
  uint8_t onHour;    // local hour it comes back on
  uint16_t wakeSecs; // how long a button press wakes it during the window
};
extern ScreenSettings gScreen;

// Open NVS and load the stop table (empty on a fresh device).
void settingsBegin();

// Persist `count` stops to NVS and copy them into gStops/gStopCount.
// Returns false if the NVS write failed (gStops is then left unchanged).
bool settingsSaveStops(const StopConfig *stops, size_t count);

// Parse/validate the stops JSON blob ({"v":1,"stops":[{k,r,s,d},...]}) used
// both in NVS and as the web API payload. Strict: any invalid entry rejects
// the whole document. `out` must hold MAX_STOPS entries.
bool settingsParseStops(const String &json, StopConfig *out, size_t &outCount);

// The current stop table serialized in that same JSON shape.
String settingsStopsJson();

// Persist a new screen-sleep schedule and apply it to gScreen.
void settingsSaveScreen(const ScreenSettings &s);

// Stored WiFi credentials. Returns false when none are stored (unprovisioned).
bool settingsGetWifi(String &ssid, String &pass);
void settingsSetWifi(const char *ssid, const char *pass);

// Wipe everything (WiFi + stops) and restart into the provisioning portal.
void settingsFactoryReset();
