// Screen rendering for the HUB75 64x32 LED matrix. With ENABLE_DISPLAY absent,
// the public functions remain available as no-ops for diagnostics builds.
#pragma once

#include "transit_data.h"

#ifdef ENABLE_DISPLAY
#define HAS_SCREEN 1
#else
#define HAS_SCREEN 0
#endif

// Initialize the panel driver. Returns false if DMA setup failed
// (or when built without a screen backend).
bool displayInit();

// Draw the page for gStops[page] from the current transit state. A line with
// an active alert shows a warning triangle beside the following-train times.
void displayRender(const TransitState &state, const AlertState &alerts,
                   uint8_t page);

// Branded boot screen: TTC pill badge, status line, loading dots. Each call
// advances the animation one step, so call it on a timer while waiting.
void displayShowBoot(const char *status);

// Full-screen warning (yellow triangle + headline + dim detail line), e.g.
// when WiFi can't connect.
void displayShowWarning(const char *line1, const char *line2);

// Neutral three-line info screen (bold headline + two dim lines): the setup
// portal instructions and the post-connect address splash.
void displayShowInfo(const char *line1, const char *line2, const char *line3);

// Blank + power down the screen (night window) or bring it back. Render and
// status calls are ignored while powered off.
void displaySetPower(bool on);
