// TTC Transit Screen — fetch-first firmware.
//
// Polls a prediction feed per configured stop (gStops, NVS-backed via the
// web-config UI: subway via NTAS, streetcar/bus via NextBus) and prints
// arrivals to serial. Rendering targets the HUB75 matrix with ENABLE_DISPLAY;
// without it the firmware remains usable for serial diagnostics.
//
// Networking is a small state machine: stored credentials are tried first
// (StaConnecting); if none exist or they keep failing, the device hosts an
// open setup AP with a captive portal (Portal) where WiFi and stops can be
// configured. The web server runs in every mode.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_system.h> // esp_reset_reason
#include <time.h>

#include "config.h"
#include "display.h"
#include "settings.h"
#include "transit_data.h"
#include "webconfig.h"

static TransitState state;
static AlertState alerts;
static uint32_t feedDueAtMs[MAX_STOPS];
static uint32_t alertsDueAtMs = 0; // set in scheduleFetches (count is runtime)

enum class NetMode : uint8_t { StaConnecting, StaConnected, Portal };
static NetMode netMode = NetMode::Portal;
static String staSsid, staPass;               // from NVS
static uint32_t staDeadlineMs = 0;            // StaConnecting -> portal at
static uint32_t staRetryAtMs = 0;             // next reconnect attempt
static uint32_t staBackoffMs = WIFI_RETRY_BASE_MS;
static uint32_t staLostSinceMs = 0;           // 0 = link is up
static uint32_t statusScreenAtMs = 0;         // boot/setup screen refresh
static bool timeConfigured = false;

static FetchStatus fetchStop(const StopConfig &stop, Departures &dep) {
  return stop.kind == RouteKind::Subway
             ? fetchSubway(stop.stopId, dep)
             : fetchNextBus(stop.route, stop.stopId, dep);
}

// (Re)arm the per-stop fetch timers, staggered so requests don't burst;
// alerts go last. startInMs delays the whole train (e.g. past the IP splash).
static void scheduleFetches(uint32_t startInMs) {
  uint32_t now = millis();
  for (size_t i = 0; i < gStopCount; i++) {
    feedDueAtMs[i] = now + startInMs + i * FEED_STAGGER_MS;
  }
  alertsDueAtMs = now + startInMs + gStopCount * FEED_STAGGER_MS;
}

#if HAS_SCREEN
static uint8_t activePage = 0;
static bool displayOk = false;
static bool screenOn = true;
static uint32_t nightWakeUntilMs = 0; // button-wake deadline inside the window

// Current transit page — or the pointer to the config UI when the stop list
// is empty (fresh device, or everything deleted in the web UI).
static void renderCurrent() {
  if (!displayOk) {
    return;
  }
  if (gStopCount == 0) {
    displayShowInfo("No stops", "add at", MDNS_HOSTNAME ".local");
    return;
  }
  displayRender(state, alerts, activePage);
}

// True while local time is inside the configured screen-off window (gScreen,
// web-editable). Before the first NTP sync the clock reads as 1970, which
// fails the sanity check and keeps the screen on.
static bool inScreenOffWindow() {
  if (!gScreen.sleepEnabled || gScreen.offHour == gScreen.onHour) {
    return false;
  }
  time_t nowT = time(nullptr);
  if (nowT < 1609459200) { // 2021-01-01; clock not synced yet
    return false;
  }
  struct tm t;
  localtime_r(&nowT, &t);
  int h = t.tm_hour;
  if (gScreen.offHour < gScreen.onHour) {
    return h >= gScreen.offHour && h < gScreen.onHour;
  }
  return h >= gScreen.offHour || h < gScreen.onHour;
}

// Turn the screen on/off as the window and button-wake deadline dictate.
static void updateScreenPower() {
  bool wantOn = !inScreenOffWindow() ||
                (int32_t)(millis() - nightWakeUntilMs) < 0;
  if (wantOn != screenOn) {
    screenOn = wantOn;
    Serial.printf("[screen] %s\r\n", wantOn ? "on" : "off (night window)");
    displaySetPower(wantOn);
    if (wantOn) {
      renderCurrent();
    }
  }
}

// Page-toggle button, active low. During the night window the first press
// wakes the screen for gScreen.wakeSecs; presses while awake cycle pages
// and extend the wake deadline. Holding the
// button FACTORY_RESET_HOLD_MS wipes the config (WiFi + stops) and reboots
// into the setup portal.
static void pollPageButton() {
  static bool lastPressed = false;
  static uint32_t lastChangeMs = 0;
  static uint32_t pressedAtMs = 0;
  bool pressed = digitalRead(PAGE_BUTTON_PIN) == LOW;
  if (pressed != lastPressed && millis() - lastChangeMs > 50) {
    lastChangeMs = millis();
    lastPressed = pressed;
    if (pressed) {
      pressedAtMs = millis();
      if (inScreenOffWindow()) {
        nightWakeUntilMs = millis() + gScreen.wakeSecs * 1000UL;
      }
      if (!screenOn) {
        updateScreenPower(); // waking press: turn on, don't cycle
        return;
      }
      // Cycle through the configured stops in list order.
      if (gStopCount > 0) {
        activePage = (uint8_t)((activePage + 1) % gStopCount);
        renderCurrent();
      }
    }
  }
  if (lastPressed && millis() - pressedAtMs > FACTORY_RESET_HOLD_MS) {
    displayShowWarning("Reset", "all config");
    delay(1500); // let the message register before the reboot blanks it
    settingsFactoryReset();
  }
}
#endif

// ---- WiFi state machine -------------------------------------------------------

static void startSta() {
  Serial.printf("[WiFi] connecting to \"%s\"\r\n", staSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  netMode = NetMode::StaConnecting;
  staDeadlineMs = millis() + STA_CONNECT_TIMEOUT_MS;
}

static void startPortal() {
  webconfigStartPortal();
  netMode = NetMode::Portal;
  staRetryAtMs = millis() + PORTAL_STA_RETRY_MS;
  statusScreenAtMs = 0; // draw the setup screen immediately
#if HAS_SCREEN
  // The portal can start inside the night window (WiFi died while asleep);
  // the setup instructions must be visible regardless.
  screenOn = true;
  displaySetPower(true);
#endif
}

static void onStaConnected() {
  netMode = NetMode::StaConnected;
  staLostSinceMs = 0;
  staBackoffMs = WIFI_RETRY_BASE_MS;
  String ip = WiFi.localIP().toString();
  Serial.printf("[WiFi] connected, IP %s, RSSI %d dBm\r\n", ip.c_str(),
                WiFi.RSSI());

  if (!timeConfigured) {
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2); // for the night window
    timeConfigured = true;
  }
  MDNS.end();
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://" MDNS_HOSTNAME ".local/");
  }

#if HAS_SCREEN
  // Straight to the transit pages (first fetch fills them within seconds);
  // a fresh device with no stops shows the pointer to the config UI instead.
  renderCurrent();
#endif
  scheduleFetches(0);
}

// Persist + hot-apply a new stop list from the web UI. Runs on the main
// thread (synchronous server), so no fetch is mid-flight while state resets.
static bool applyStops(const StopConfig *stops, size_t count) {
  if (!settingsSaveStops(stops, count)) {
    return false;
  }
  state = TransitState{};
  alerts = AlertState{};
  scheduleFetches(0);
#if HAS_SCREEN
  activePage = 0;
  renderCurrent();
#endif
  return true;
}

static void printFeedResult(const StopConfig &stop, const Departures &dep,
                            FetchStatus status, uint32_t elapsedMs) {
  Serial.printf("[up %6lus] %4s %-10s: ", millis() / 1000, stop.route,
                stop.dest);
  if (status == FetchStatus::Ok) {
    for (int i = 0; i < 3 && dep.next[i] >= 0; i++) {
      Serial.printf(i > 0 ? ", %d" : "%d", dep.next[i]);
    }
    Serial.print(" min");
  } else {
    Serial.print(fetchStatusName(status));
  }
  Serial.printf("  (%s, %lums)\r\n", fetchStatusName(status), elapsedMs);
}

void setup() {
  Serial.begin(115200);
  // Native USB CDC needs a moment for the host to attach.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {
    delay(10);
  }
  Serial.println();
  Serial.println("=== TTC Transit Screen " FW_VERSION " ===");

  settingsBegin(); // loads gStops from NVS

#if HAS_SCREEN
#ifdef PAGE_BUTTON_GND_PIN
  // Virtual ground for the page button (see config.h).
  pinMode(PAGE_BUTTON_GND_PIN, OUTPUT);
  digitalWrite(PAGE_BUTTON_GND_PIN, LOW);
#endif
  pinMode(PAGE_BUTTON_PIN, INPUT_PULLUP);
  // Cold power-up can catch the 5V rail mid-rise (panel PSU inrush) and the
  // DMA init fails where a warm reset succeeds. Retry, then reboot once —
  // the same cure as pressing RESET, which matters inside a closed
  // enclosure. The reset-reason guard prevents a reboot loop when the panel
  // genuinely isn't there.
  for (int attempt = 0; attempt < 3 && !displayOk; attempt++) {
    if (attempt > 0) {
      delay(300);
    }
    displayOk = displayInit();
  }
  Serial.printf("[display] init %s\r\n", displayOk ? "ok" : "FAILED");
  if (!displayOk && esp_reset_reason() != ESP_RST_SW) {
    Serial.println("[display] rebooting once to retry init on stable power");
    delay(250);
    ESP.restart();
  }
  displayShowBoot("starting");
#endif

  if (settingsGetWifi(staSsid, staPass)) {
    startSta();
  } else {
    Serial.println("[WiFi] no stored credentials, starting setup portal");
    startPortal();
  }
  webconfigBegin(applyStops);
}

void loop() {
  // HTTP + portal DNS + deferred restarts, in every mode — including while
  // the screen sleeps at night.
  webconfigTick();

  uint32_t now = millis();
  switch (netMode) {
  case NetMode::StaConnecting:
    if (WiFi.status() == WL_CONNECTED) {
      onStaConnected();
      break;
    }
    if ((int32_t)(now - staDeadlineMs) >= 0) {
      Serial.println("[WiFi] connect timed out, starting setup portal");
      startPortal();
      break;
    }
#if HAS_SCREEN
    if (now - statusScreenAtMs > 350) { // tick the loading animation
      statusScreenAtMs = now;
      displayShowBoot("connecting");
    }
#endif
    break;

  case NetMode::StaConnected:
    if (WiFi.status() != WL_CONNECTED) {
      if (staLostSinceMs == 0) {
        staLostSinceMs = now;
        staRetryAtMs = now; // first retry immediately
        staBackoffMs = WIFI_RETRY_BASE_MS;
        Serial.println("[WiFi] connection lost");
#if HAS_SCREEN
        displayShowWarning("No WiFi", staSsid.c_str());
#endif
      }
      if (now - staLostSinceMs > STA_LOST_PORTAL_AFTER_MS) {
        Serial.println("[WiFi] down too long, starting setup portal");
        startPortal();
        break;
      }
      if ((int32_t)(now - staRetryAtMs) >= 0) {
        Serial.printf("[WiFi] reconnecting (next retry in %lus)\r\n",
                      staBackoffMs / 1000);
        WiFi.disconnect();
        WiFi.begin(staSsid.c_str(), staPass.c_str());
        staRetryAtMs = now + staBackoffMs;
        staBackoffMs = min(staBackoffMs * 2, (uint32_t)WIFI_RETRY_MAX_MS);
      }
    } else if (staLostSinceMs != 0) {
      staLostSinceMs = 0;
      staBackoffMs = WIFI_RETRY_BASE_MS;
      Serial.printf("[WiFi] reconnected, IP %s\r\n",
                    WiFi.localIP().toString().c_str());
#if HAS_SCREEN
      renderCurrent();
#endif
    }
    break;

  case NetMode::Portal:
    // The background STA retry (below) may have gotten us online; if so the
    // setup AP has done its job.
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] configured network reachable again, closing portal");
      webconfigStopPortal();
      onStaConnected();
      break;
    }
    if (!staSsid.isEmpty() && (int32_t)(now - staRetryAtMs) >= 0) {
      Serial.println("[WiFi] portal: retrying stored credentials");
      WiFi.begin(staSsid.c_str(), staPass.c_str());
      staRetryAtMs = now + PORTAL_STA_RETRY_MS;
    }
#if HAS_SCREEN
    if (now - statusScreenAtMs > 1000) { // static screen; cheap to refresh
      statusScreenAtMs = now;
      displayShowInfo("WiFi Setup", AP_SSID,
                      WiFi.softAPIP().toString().c_str());
    }
#endif
    break;
  }

  if (netMode != NetMode::StaConnected || staLostSinceMs != 0) {
    delay(10);
    return; // status screens own the display; nothing to fetch yet
  }

#if HAS_SCREEN
  updateScreenPower();
  pollPageButton();
  // While the screen sleeps, stop polling too; the stale due times make all
  // feeds refresh immediately on wake. (webconfigTick already ran above.)
  if (!screenOn) {
    delay(10);
    return;
  }
#endif

  now = millis();
  for (size_t i = 0; i < gStopCount; i++) {
    if ((int32_t)(now - feedDueAtMs[i]) >= 0) {
      uint32_t startMs = millis();
      FetchStatus status = fetchStop(gStops[i], state.deps[i]);
      printFeedResult(gStops[i], state.deps[i], status, millis() - startMs);
      feedDueAtMs[i] = now + POLL_INTERVAL_MS;

#if HAS_SCREEN
      renderCurrent();
#endif
    }
  }

  if (gStopCount > 0 && (int32_t)(now - alertsDueAtMs) >= 0) {
    uint32_t startMs = millis();
    FetchStatus status = fetchAlerts(gStops, gStopCount, alerts);
    Serial.printf("[up %6lus] Alerts: ", millis() / 1000);
    for (size_t i = 0; i < gStopCount; i++) {
      Serial.printf(i > 0 ? ", %s %s" : "%s %s", gStops[i].route,
                    alerts.alert[i] ? "ALERT" : "ok");
    }
    Serial.printf("  (%s, %lums)\r\n", fetchStatusName(status),
                  millis() - startMs);
    alertsDueAtMs = now + ALERT_POLL_INTERVAL_MS;
#if HAS_SCREEN
    renderCurrent();
#endif
  }

  delay(10);
}
