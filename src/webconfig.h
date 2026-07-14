// Web config: HTTP server (config UI + JSON API + OTA upload) and the
// provisioning captive portal (softAP + wildcard DNS).
//
// The server is the synchronous WebServer — handlers run inside
// webconfigTick() on the main loop, on the same thread as the fetch/render
// code, so they can touch gStops and call back into main.cpp without locks.
#pragma once

#include <Arduino.h>
#include <functional>

#include "config.h"

// `onStopsChanged` is invoked (from a request handler) with a validated new
// stop list; it must persist + apply it and return false on failure.
void webconfigBegin(std::function<bool(const StopConfig *, size_t)> onStopsChanged);

// Service the HTTP server, portal DNS, and any pending deferred restart.
// Call every loop iteration, in every network mode.
void webconfigTick();

// Bring the setup AP (open network AP_SSID + captive-portal DNS) up/down.
// Uses WIFI_AP_STA so main.cpp can keep retrying the configured network.
void webconfigStartPortal();
void webconfigStopPortal();
bool webconfigPortalActive();
