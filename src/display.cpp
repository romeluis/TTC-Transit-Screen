#include "display.h"

#if !HAS_SCREEN

bool displayInit() { return false; }
void displayRender(const TransitState &, const AlertState &, uint8_t) {}
void displayShowBoot(const char *) {}
void displayShowWarning(const char *, const char *) {}
void displayShowInfo(const char *, const char *, const char *) {}
void displaySetPower(bool) {}

#else

#include <Adafruit_GFX.h>

#include "config.h"
#include "settings.h"

// ---- Colors (RGB565) --------------------------------------------------------

static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static constexpr uint16_t COL_BLACK = 0;
static constexpr uint16_t COL_TTC_RED = rgb565(0xDA, 0x25, 0x1D);
static constexpr uint16_t COL_WHITE = rgb565(0xFF, 0xFF, 0xFF);
// Official TTC line/route branding colors.
static constexpr uint16_t COL_L1_YELLOW = rgb565(0xF8, 0xC3, 0x00);
static constexpr uint16_t COL_L2_GREEN = rgb565(0x00, 0xA5, 0x4F);
static constexpr uint16_t COL_L3_BLUE = rgb565(0x00, 0x82, 0xC9);
static constexpr uint16_t COL_L4_PURPLE = rgb565(0xA2, 0x1A, 0x68);
static constexpr uint16_t COL_L5_ORANGE = rgb565(0xF5, 0x7C, 0x1E);
static constexpr uint16_t COL_L6_GREY = rgb565(0xA5, 0xA7, 0xA9);
static constexpr uint16_t COL_NIGHT_BLUE = rgb565(0x00, 0x41, 0x7B);
static constexpr uint16_t COL_EXPRESS_GREEN = rgb565(0x00, 0x92, 0x3F);
static constexpr uint16_t COL_DIM = rgb565(0x60, 0x60, 0x60);   // labels/stale
static constexpr uint16_t COL_AMBER = rgb565(0xFF, 0xB0, 0x00); // fresh times
static constexpr uint16_t COL_YELLOW = rgb565(0xFF, 0xD5, 0x00); // alerts

// ---- Shared drawing ---------------------------------------------------------
// Everything below draws on an Adafruit_GFX with a 64x32 coordinate space.

// A 1px empty margin is kept on all four panel edges; content lives in
// (1,1)..(62,30).

// Poor man's bold: print twice, offset 1px right, for 2px vertical strokes.
// Glyphs grow to 6px wide against the 6px advance, so letters just touch.
static void printBold(Adafruit_GFX &g, int16_t x, int16_t y, const char *s) {
  g.setCursor(x, y);
  g.print(s);
  g.setCursor(x + 1, y);
  g.print(s);
}

// ---- Route badge (header glyph) ----------------------------------------------
// Subway lines get a colored disc, surface routes a pill sized to the route
// number; both use the same digit font, so all badges share one look.

// 13x13 disc drawn by hand: GFX fillCircle at small radii tapers to a single
// pixel at the poles and reads as a diamond.
static const uint8_t DISC_13X13[] = {
    0x0F, 0x80, // ....#####....
    0x3F, 0xE0, // ..#########..
    0x7F, 0xF0, // .###########.
    0x7F, 0xF0, // .###########.
    0xFF, 0xF8, // #############
    0xFF, 0xF8, // #############
    0xFF, 0xF8, // #############
    0xFF, 0xF8, // #############
    0xFF, 0xF8, // #############
    0x7F, 0xF0, // .###########.
    0x7F, 0xF0, // .###########.
    0x3F, 0xE0, // ..#########..
    0x0F, 0x80, // ....#####....
};

// Hand-drawn route-number digits, 7px tall with 2px strokes for weight.
// Proportional: "1" keeps its serifs at 4px, the rest are 5px, with a 1px gap
// between digits ("511" composes pixel-identically to the original hand-drawn
// 15x7 bitmap). Row bits are MSB-first.
struct DigitGlyph {
  uint8_t width;
  uint8_t rows[7];
};

static const DigitGlyph DIGITS[10] = {
    {5, {0xF8, 0xD8, 0xD8, 0xD8, 0xD8, 0xD8, 0xF8}}, // 0
    {4, {0x60, 0xE0, 0x60, 0x60, 0x60, 0x60, 0xF0}}, // 1
    {5, {0xF8, 0x18, 0x18, 0xF8, 0xC0, 0xC0, 0xF8}}, // 2
    {5, {0xF8, 0x18, 0x18, 0xF8, 0x18, 0x18, 0xF8}}, // 3
    {5, {0xD8, 0xD8, 0xD8, 0xF8, 0x18, 0x18, 0x18}}, // 4
    {5, {0xF8, 0xC0, 0xC0, 0xF8, 0x18, 0x18, 0xF8}}, // 5
    {5, {0xF8, 0xC0, 0xC0, 0xF8, 0xD8, 0xD8, 0xF8}}, // 6
    {5, {0xF8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}}, // 7
    {5, {0xF8, 0xD8, 0xD8, 0xF8, 0xD8, 0xD8, 0xF8}}, // 8
    {5, {0xF8, 0xD8, 0xD8, 0xF8, 0x18, 0x18, 0xF8}}, // 9
};

static int16_t routeTextWidth(const char *route) {
  int16_t w = 0;
  for (const char *p = route; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') {
      continue;
    }
    w += (w > 0 ? 1 : 0) + DIGITS[*p - '0'].width;
  }
  return w;
}

static void drawRouteText(Adafruit_GFX &g, int16_t x, int16_t y,
                          const char *route, uint16_t color) {
  for (const char *p = route; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') {
      continue;
    }
    const DigitGlyph &d = DIGITS[*p - '0'];
    // Width 8 = the full row byte; drawBitmap only touches set bits, so the
    // padding bits past d.width are harmless.
    g.drawBitmap(x, y, d.rows, 8, 7, color);
    x += d.width + 1;
  }
}

// Rounded-rect pill, 13px tall at any width — stepped corners matching the
// disc's curvature (rows 0/12 inset 3px, rows 1/11 inset 1px).
static void drawPill(Adafruit_GFX &g, int16_t x, int16_t y, int16_t w,
                     uint16_t color) {
  for (int16_t row = 0; row < 13; row++) {
    int16_t inset = (row == 0 || row == 12) ? 3 : (row == 1 || row == 11) ? 1 : 0;
    g.drawFastHLine(x + inset, y + row, w - 2 * inset, color);
  }
}

// TTC branding colors per route: subway discs use the line color; surface
// pills are red, except Blue Night (300s) and Express (900s). All route
// numbers are white for consistent legibility.
static void routeColors(const StopConfig &stop, uint16_t &bg, uint16_t &fg) {
  int n = atoi(stop.route);
  fg = COL_WHITE;
  if (stop.kind == RouteKind::Subway) {
    switch (n) {
    case 1:
      bg = COL_L1_YELLOW;
      break;
    case 2:
      bg = COL_L2_GREEN;
      break;
    case 3:
      bg = COL_L3_BLUE;
      break;
    case 4:
      bg = COL_L4_PURPLE;
      break;
    case 5:
      bg = COL_L5_ORANGE;
      break;
    case 6:
      bg = COL_L6_GREY;
      break;
    default:
      bg = COL_TTC_RED;
      break;
    }
  } else {
    bg = n >= 300 && n < 400 ? COL_NIGHT_BLUE
         : n >= 900          ? COL_EXPRESS_GREEN
                             : COL_TTC_RED;
  }
}

// Draw the route badge at the top-left; returns its width so the destination
// text can follow it.
static int16_t drawRouteBadge(Adafruit_GFX &g, const StopConfig &stop) {
  uint16_t bg, fg;
  routeColors(stop, bg, fg);
  int16_t textW = routeTextWidth(stop.route);
  if (stop.kind == RouteKind::Subway) {
    g.drawBitmap(1, 1, DISC_13X13, 13, 13, bg);
    drawRouteText(g, 1 + (13 - textW) / 2, 4, stop.route, fg);
    return 13;
  }
  int16_t w = textW + 8; // 4px pad each side; a 1-digit route makes 13x13
  drawPill(g, 1, 1, w, bg);
  drawRouteText(g, 1 + (w - textW) / 2, 4, stop.route, fg);
  return w;
}

// Hand-drawn warning triangle, 11x10: pointed 1px tip, then 2-row slope
// steps that read as smooth edges (GFX fillTriangle looks ragged here).
static const uint8_t TRIANGLE_11X10[] = {
    0x04, 0x00, // .....#.....
    0x0E, 0x00, // ....###....
    0x0E, 0x00, // ....###....
    0x1F, 0x00, // ...#####...
    0x1F, 0x00, // ...#####...
    0x3F, 0x80, // ..#######..
    0x3F, 0x80, // ..#######..
    0x7F, 0xC0, // .#########.
    0xFF, 0xE0, // ###########
    0xFF, 0xE0, // ###########
};

// Yellow warning triangle with a black "!", shown beside the following-train
// times when the line has an active alert.
static void drawWarningTriangle(Adafruit_GFX &g, int16_t x, int16_t y) {
  g.drawBitmap(x, y, TRIANGLE_11X10, 11, 10, COL_YELLOW);
  g.drawFastVLine(x + 5, y + 3, 3, COL_BLACK);
  g.drawPixel(x + 5, y + 7, COL_BLACK);
}

// Barcelona-style single-direction page:
//   [badge] DESTINATION          <- header
//   12 min                  +19  <- next train large, following trains small
// 0 minutes renders as "Due"; an active alert shows a warning triangle to
// the left of the following-train times.
static void drawDirectionPage(Adafruit_GFX &g, const StopConfig &stop,
                              const Departures &dep, bool alert) {
  g.setTextSize(1);
  g.setTextWrap(false); // over-long destinations clip at the edge
  g.setTextColor(COL_WHITE);
  int16_t badgeW = drawRouteBadge(g, stop);
  printBold(g, 1 + badgeW + 3, 4, stop.dest);

  bool stale = dep.isStale(millis(), STALE_AFTER_MS);
  uint16_t timeColor = stale ? COL_DIM : COL_AMBER;

  // Next train, large. "Due" when it's basically here — except in alert
  // mode, where plain "0" keeps the row compact.
  char big[6];
  bool showMinLabel = false;
  if (dep.noService || dep.next[0] < 0) {
    strcpy(big, "--");
  } else if (dep.next[0] == 0 && !alert) {
    strcpy(big, "Due");
  } else {
    snprintf(big, sizeof(big), "%d", dep.next[0]);
    // The "min" label makes way for the warning triangle in alert mode.
    showMinLabel = !alert;
  }
  // Size 2 is the 5x7 font pixel-doubled, keeping one font family across
  // the whole display.
  g.setTextSize(2);
  g.setTextColor(dep.noService ? COL_DIM : timeColor);
  g.setCursor(1, 17);
  g.print(big);
  if (showMinLabel) {
    g.setTextSize(1);
    g.setTextColor(COL_DIM);
    g.setCursor(1 + (int16_t)strlen(big) * 12 + 2, 24);
    g.print("min");
  }

  // Trains after the next, small, right-aligned flush with the edge padding
  // (the 6px char advance includes a trailing blank column, so the last glyph
  // pixel of an n-char string printed at PANEL_WIDTH - n*6 lands on x=62).
  // With two they stack with a 2px gap; with one it sits in the
  // bottom-right corner.
  int16_t smallsLeft = PANEL_WIDTH; // leftmost lit column of the small times
  auto drawSmall = [&](int minutes, int16_t y) {
    char small[6];
    if (minutes == 0 && !alert) {
      strcpy(small, "Due");
    } else {
      snprintf(small, sizeof(small), "+%d", minutes);
    }
    int16_t x = PANEL_WIDTH - (int16_t)strlen(small) * 6;
    smallsLeft = min(smallsLeft, x);
    g.setTextSize(1);
    g.setTextColor(stale ? COL_DIM : COL_WHITE);
    g.setCursor(x, y);
    g.print(small);
  };
  if (!dep.noService && dep.next[1] >= 0) {
    if (dep.next[2] >= 0) {
      drawSmall(dep.next[1], 15);
      drawSmall(dep.next[2], 24);
    } else {
      drawSmall(dep.next[1], 24);
    }
  }

  // Alert: warning triangle 1px left of the widest following-train time,
  // vertically centered on the stacked pair (the hidden "min" leaves room).
  if (alert) {
    drawWarningTriangle(g, smallsLeft - 12, 18);
  }
}

// ---- Boot & warning screens ---------------------------------------------------

static void printCentered(Adafruit_GFX &g, int16_t y, const char *s,
                          bool bold) {
  int16_t x = (PANEL_WIDTH - (int16_t)strlen(s) * 6) / 2;
  if (bold) {
    printBold(g, x, y, s);
  } else {
    g.setCursor(x, y);
    g.print(s);
  }
}

// Boot screen in the product's own design language: the red TTC pill as a
// badge up top, a dim status line, and a three-dot loading animation that
// advances one step per call.
static void drawBootScreen(Adafruit_GFX &g, const char *status,
                           uint8_t animFrame) {
  g.setTextSize(1);
  g.setTextWrap(false);
  // 27px pill = bold "TTC" (19px) + the route pills' 4px side padding.
  drawPill(g, (PANEL_WIDTH - 27) / 2, 2, 27, COL_TTC_RED);
  g.setTextColor(COL_WHITE);
  printBold(g, (PANEL_WIDTH - 19) / 2, 5, "TTC");
  g.setTextColor(COL_DIM);
  printCentered(g, 18, status, false);
  for (int16_t i = 0; i < 3; i++) {
    g.fillRect(PANEL_WIDTH / 2 - 6 + i * 5, 27, 2, 2,
               animFrame % 3 == i ? COL_WHITE : COL_DIM);
  }
}

// Warning screen (WiFi loss etc.): the alert triangle centered up top, a bold
// headline, and a dim detail line.
static void drawWarningScreen(Adafruit_GFX &g, const char *line1,
                              const char *line2) {
  g.setTextSize(1);
  g.setTextWrap(false);
  drawWarningTriangle(g, (PANEL_WIDTH - 11) / 2, 1);
  g.setTextColor(COL_WHITE);
  printCentered(g, 13, line1, true);
  g.setTextColor(COL_DIM);
  printCentered(g, 23, line2, false);
}

// Info screen (setup portal / address splash): bold headline, two dim lines.
// The three 8px text rows fill the panel, so no glyph up top.
static void drawInfoScreen(Adafruit_GFX &g, const char *line1,
                           const char *line2, const char *line3) {
  g.setTextSize(1);
  g.setTextWrap(false);
  g.setTextColor(COL_WHITE);
  printCentered(g, 2, line1, true);
  g.setTextColor(COL_AMBER);
  printCentered(g, 13, line2, false);
  g.setTextColor(COL_DIM);
  printCentered(g, 23, line3, false);
}

// ---- Backend: HUB75 LED matrix ----------------------------------------------

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

static MatrixPanel_I2S_DMA *panel = nullptr;

static bool backendInit() {
  HUB75_I2S_CFG::i2s_pins pins = {
      HUB75_PIN_R1, HUB75_PIN_G1, HUB75_PIN_B1, HUB75_PIN_R2, HUB75_PIN_G2,
      HUB75_PIN_B2, HUB75_PIN_A,  HUB75_PIN_B,  HUB75_PIN_C,  HUB75_PIN_D,
      HUB75_PIN_E,  HUB75_PIN_LAT, HUB75_PIN_OE, HUB75_PIN_CLK};
  HUB75_I2S_CFG cfg(PANEL_WIDTH, PANEL_HEIGHT, 1, pins);
  panel = new MatrixPanel_I2S_DMA(cfg);
  if (!panel->begin()) {
    delete panel;
    panel = nullptr;
    return false;
  }
  panel->setBrightness8(PANEL_BRIGHTNESS);
  panel->clearScreen();
  return true;
}

// Panel writes are live DMA memory; drawing on it is already "presented".
static Adafruit_GFX &frame() { return *panel; }
static void present() {}

static void backendSetPower(bool on) {
  if (on) {
    panel->setBrightness8(PANEL_BRIGHTNESS);
  } else {
    panel->clearScreen();
    panel->setBrightness8(0);
  }
}

// ---- Public API --------------------------------------------------------------

static bool ready = false;
static bool powered = true;

bool displayInit() {
  ready = backendInit();
  return ready;
}

void displaySetPower(bool on) {
  if (!ready || powered == on) {
    return;
  }
  powered = on;
  backendSetPower(on);
}

void displayRender(const TransitState &state, const AlertState &alerts,
                   uint8_t page) {
  if (!ready || !powered || page >= gStopCount) {
    return;
  }
  Adafruit_GFX &g = frame();
  g.fillScreen(0);
  drawDirectionPage(g, gStops[page], state.deps[page], alerts.alert[page]);
  present();
}

void displayShowBoot(const char *status) {
  if (!ready || !powered) {
    return;
  }
  static uint8_t animFrame = 0;
  Adafruit_GFX &g = frame();
  g.fillScreen(0);
  drawBootScreen(g, status, animFrame++);
  present();
}

void displayShowWarning(const char *line1, const char *line2) {
  if (!ready || !powered) {
    return;
  }
  Adafruit_GFX &g = frame();
  g.fillScreen(0);
  drawWarningScreen(g, line1, line2);
  present();
}

void displayShowInfo(const char *line1, const char *line2, const char *line3) {
  if (!ready || !powered) {
    return;
  }
  Adafruit_GFX &g = frame();
  g.fillScreen(0);
  drawInfoScreen(g, line1, line2, line3);
  present();
}

#endif // HAS_SCREEN
