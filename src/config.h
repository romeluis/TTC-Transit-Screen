// Central configuration: endpoints, timing, display geometry, and wiring.
#pragma once

#include <stdint.h>

// Shown in the web UI; bump before an OTA build so the update is verifiable.
#define FW_VERSION "1.1.0"

// ---- TTC data sources -------------------------------------------------------

// Subway NTAS API: https://ntas.ttc.ca/api/ntas/get-next-train-time/{stopCode}
#define NTAS_BASE_URL "https://ntas.ttc.ca/api/ntas/get-next-train-time/"

// NextBus/UMO public feed; route and stop tag are appended per configured stop.
#define NEXTBUS_BASE_URL                                                       \
  "https://retro.umoiq.com/service/publicJSONFeed"                            \
  "?command=predictions&a=ttc"

// ---- Stops shown on the screen -----------------------------------------------
// One page per entry, cycled by the button in list order. The route number
// picks the header glyph (subway disc / surface pill) and its TTC colors, and
// is matched against the live-alerts feed.
//
// The live list is gStops/gStopCount (settings.h), loaded from NVS. Nothing
// is hardcoded: a device with no saved config shows the "No stops" pointer
// until stops are added in the web UI.

enum class RouteKind : uint8_t {
  Subway, // NTAS; stopId is a platform-level stop code from TTC GTFS stops.txt
  Surface // NextBus (streetcar & bus); stopId is a NextBus stop tag
};

#define ROUTE_MAX_LEN 7 // route string, e.g. "511"
#define DEST_MAX_LEN 15 // header label; ~7 chars fits beside a 3-digit pill

struct StopConfig {
  RouteKind kind;
  char route[ROUTE_MAX_LEN + 1]; // "2", "511", "7", ...
  int stopId;
  char dest[DEST_MAX_LEN + 1];
};

// MAX_STOPS sizes the runtime state arrays.
#define MAX_STOPS 8

// Live service alerts (warning indicator for active line issues).
// ~100KB payload; fetched with a streaming filter, so poll sparingly.
#define ALERTS_URL "https://alerts.ttc.ca/api/alerts/live-alerts"
#define ALERT_POLL_INTERVAL_MS 120000UL

#define HTTP_USER_AGENT "ttc-transit-screen/1.0 (esp32-s3)"
#define HTTP_TIMEOUT_MS 10000

// ---- Timing ----------------------------------------------------------------

#define POLL_INTERVAL_MS 25000UL  // per feed
#define FEED_STAGGER_MS 3000UL    // offset between the three feeds
#define STALE_AFTER_MS 90000UL    // dim/flag data older than this
#define WIFI_RETRY_BASE_MS 5000UL // reconnect backoff start (doubles, caps at 60s)
#define WIFI_RETRY_MAX_MS 60000UL

// ---- WiFi provisioning & web config ------------------------------------------

// Open setup network hosted while unprovisioned. Must fit the 64px screen
// (10 chars max) so the setup instructions can show it.
#define AP_SSID "TTC-Setup"
#define MDNS_HOSTNAME "ttc" // http://ttc.local/
// Give stored credentials this long before falling back to the setup AP.
#define STA_CONNECT_TIMEOUT_MS 45000UL
// In portal mode with stored credentials, retry the configured network this often.
#define PORTAL_STA_RETRY_MS 120000UL
// A connection that stays down this long also falls back to the portal.
#define STA_LOST_PORTAL_AFTER_MS 300000UL
// Hold the page button this long (any time after boot) to wipe WiFi + stop
// config. Can't be a hold-through-power-on gesture: GPIO0 low at reset straps
// the chip into the ROM bootloader before the firmware ever runs.
#define FACTORY_RESET_HOLD_MS 10000UL

// ---- Clock & night dimming ---------------------------------------------------

#define TZ_INFO "EST5EDT,M3.2.0,M11.1.0" // America/Toronto
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"

// Defaults for the screen-sleep schedule (the live values are NVS-backed —
// gScreen in settings.h — and edited in the web UI's Display tab). The
// screen and polling sleep between these local hours; a button press wakes
// the screen for the configured number of seconds.
#define SCREEN_OFF_START_HOUR 21 // 9 PM
#define SCREEN_OFF_END_HOUR 6    // 6 AM
#define NIGHT_WAKE_SECS 30

// ---- Display (HUB75, only used when ENABLE_DISPLAY is defined) --------------

// 64x32 1/16-scan panel; E address line unused (tie to GND).
#define PANEL_WIDTH 64
#define PANEL_HEIGHT 32
#define PANEL_BRIGHTNESS 96 // 0-255, comfortable indoor level

// Library-default signal timing works with all ground pins tied together.

// HUB75 pin map for the supported 44-pin ESP32-S3 board. Sanity-check against
// the board silkscreen before wiring; clone boards can vary.
// E is unused on the 1/16-scan 64x32 panel (tie the panel's E pad to GND).
// The map mirrors the HUB75 connector's two columns onto the two header rails so the ribbon
// wires run straight without crossing:
//   connector column 1 -> LEFT rail,  eight consecutive pins top-to-bottom
//   connector column 2 -> RIGHT rail, five consecutive pins top-to-bottom
//   (E and the connector GNDs go straight to GND)
// Keep-outs honored: GPIO 35/36/37 (octal PSRAM bus on the N16R8), 19/20
// (USB), 43/44 (UART bridge), and 0/3/45/46 (strapping pins).
//
// Left rail, physical order 4,5,6,7,15,16,17,18:
#define HUB75_PIN_R1 4
#define HUB75_PIN_B1 5
#define HUB75_PIN_R2 6
#define HUB75_PIN_B2 7
#define HUB75_PIN_A 15
#define HUB75_PIN_C 16
#define HUB75_PIN_CLK 17
#define HUB75_PIN_OE 18
// Right rail, physical order 1,2,42,41,40 (just below the TX/RX pins):
#define HUB75_PIN_G1 1
#define HUB75_PIN_G2 2
#define HUB75_PIN_B 42
#define HUB75_PIN_D 41
#define HUB75_PIN_LAT 40
#define HUB75_PIN_E -1 // unused on 1/16-scan 32-row panels

// Page button, active low with the internal pull-up. On the devkit the tact
// button bridges two physically adjacent right-rail pins: GPIO21 (sense) and
// GPIO47 (driven LOW as a virtual ground — all real GND pins are taken by
// the panel). Fine for a button's microamps; never power a load this way.
#define PAGE_BUTTON_PIN 21
#define PAGE_BUTTON_GND_PIN 47
