// Mac Monitor: shows live stats from a Mac on the board's screen.
// Board: JC3248W535 (ESP32-S3, 3.5" 320x480 AXS15231B QSPI + capacitive touch)
// Data: mac/mac_monitor.py on the Mac sends one JSON line per second over the USB cable.
// No Wi-Fi is used. The screen dims when the Mac stops sending; tap it to wake.
// Tap a card (CPU / GPU / Memory / Network) for its detail page; tap a detail page to go back.
// Swipe left/right on the main screen to switch to the Claude Code usage page.

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include <U8g2lib.h>  // fonts (must come before Arduino_GFX)
#include <Arduino_GFX_Library.h>
#include "mac.h"

#define FW_VERSION "2.5.0"

// ---- Pins (JC3248W535) ----
#define LCD_CS   45
#define LCD_SCK  47
#define LCD_D0   21
#define LCD_D1   48
#define LCD_D2   40
#define LCD_D3   39
#define LCD_BL   1
#define TP_SDA   4
#define TP_SCL   8
#define TP_ADDR  0x3B

#define W 320
#define H 480

#define BRIGHT    80  // % backlight while data is coming in
#define DIM       8   // % when the Mac has been silent a while
#define DIM_AFTER 60000UL
#define WAKE_FOR  30000UL

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *panel = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, W, H);
Arduino_Canvas *gfx = new Arduino_Canvas(W, H, panel, 0, 0, 0);

// ---- Theme ----
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t BG     = rgb(10, 14, 28);
const uint16_t CARD   = rgb(26, 34, 60);
const uint16_t GRID   = rgb(44, 56, 92);
const uint16_t TXT    = rgb(240, 244, 255);
const uint16_t MUTED  = rgb(140, 152, 185);
const uint16_t ACCENT = rgb(90, 170, 255);
const uint16_t C_UP   = rgb(255, 200, 40);  // upload, warnings
const uint16_t C_BAD  = rgb(240, 50, 50);
const uint16_t CPUC   = rgb(80, 210, 130);
const uint16_t RAMC   = rgb(175, 125, 255);
const uint16_t GPUC   = rgb(255, 140, 60);
const uint16_t CLAUDEC = rgb(217, 119, 87);

const uint16_t LEVEL_COLOR[] = {CPUC, C_UP, C_BAD};
const uint16_t LEVEL_FILL[] = {rgb(24, 70, 50), rgb(90, 70, 20), rgb(90, 24, 24)};
static const char *LEVEL_NAME[] = {"NORMAL", "WARNING", "CRITICAL"};

// ---- Pages ----
enum Page { PG_OVERVIEW, PG_CPU, PG_GPU, PG_MEM, PG_NET, PG_CLAUDE };
Page page = PG_OVERVIEW;
bool shotActive = false;  // a screenshot is being pulled: no data arrives meanwhile, so do not show "No signal"
bool live() { return macLive() || shotActive; }

// Overview card positions (also the tap targets)
#define CPU_Y 54
#define CPU_H 80
#define GPU_Y 138
#define GPU_H 80
#define MEM_Y 222
#define MEM_H 112
#define NET_Y 338
#define NET_H 64

// ---- Text ----
enum Align { LEFT, CENTER, RIGHT };

int16_t textWidth(const uint8_t *font, const char *s) {
  gfx->setFont(font);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

// y is the text baseline
void text(const uint8_t *font, uint16_t color, int x, int y, const char *s, Align a = LEFT) {
  if (a != LEFT) {
    int w = textWidth(font, s);
    x -= (a == CENTER) ? w / 2 : w;
  }
  gfx->setFont(font);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static const char *WD3[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *MON3[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// ---- Drawing helpers ----
// Line graph of the newest `span` samples of a ring buffer (newest on the right), scaled to 0..maxV.
void graph(int x, int y, int w, int h, const float *hist, int span, float maxV, uint16_t c, uint16_t fill) {
  int n = min((int)MAC.count, span);
  if (n < 2 || maxV <= 0) return;
  for (int pass = fill ? 0 : 1; pass < 2; pass++) {  // area fill first, line on top
    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
      float v = hist[(MAC.head + MAC_HIST - n + i) % MAC_HIST];
      int gx = x + w - 1 - (n - 1 - i) * (w - 1) / (span - 1);
      int gy = y + h - 1 - (int)(constrain(v / maxV, 0.0f, 1.0f) * (h - 1));
      if (i && pass == 0) {
        gfx->fillTriangle(px, py, gx, gy, px, y + h - 1, fill);
        gfx->fillTriangle(gx, gy, gx, y + h - 1, px, y + h - 1, fill);
      } else if (i) {
        gfx->drawLine(px, py, gx, gy, c);
        gfx->drawLine(px, py - 1, gx, gy - 1, c);
      }
      px = gx;
      py = gy;
    }
  }
}

// Largest of the newest `span` samples
float histPeak(const float *hist, int span) {
  int n = min((int)MAC.count, span);
  float m = 0;
  for (int i = 0; i < n; i++) m = max(m, hist[(MAC.head + MAC_HIST - 1 - i) % MAC_HIST]);
  return m;
}

void gridLines(int x, int y, int w, int h) {
  for (int i = 1; i < 4; i++)
    for (int gx = x; gx < x + w; gx += 6) gfx->drawPixel(gx, y + h * i / 4, GRID);
}

void fmtRate(char *buf, size_t n, float bps) {
  if (bps >= 1e6f) snprintf(buf, n, "%.1f MB/s", bps / 1e6f);
  else if (bps >= 1e3f) snprintf(buf, n, "%.0f KB/s", bps / 1e3f);
  else snprintf(buf, n, "%.0f B/s", bps);
}

void fmtGB(char *buf, size_t n, float gb) {
  if (gb >= 1000) snprintf(buf, n, "%.2f TB", gb / 1000);
  else if (gb >= 10) snprintf(buf, n, "%.0f GB", gb);
  else if (gb >= 1) snprintf(buf, n, "%.1f GB", gb);
  else snprintf(buf, n, "%.0f MB", gb * 1024);
}

void arrow(int x, int y, bool up, uint16_t c) {
  if (up) gfx->fillTriangle(x, y - 10, x - 5, y - 2, x + 5, y - 2, c);
  else gfx->fillTriangle(x, y, x - 5, y - 8, x + 5, y - 8, c);
}

// Small ">" telling the user a card opens a detail page
void chevron(int x, int y, uint16_t c) {
  for (int d = 0; d < 2; d++) {
    gfx->drawLine(x + d, y - 5, x + 4 + d, y, c);
    gfx->drawLine(x + 4 + d, y, x + d, y + 5, c);
  }
}

// Overview card: title left, value right, chevron; returns the graph's top y.
int card(int y, int h, const char *title, const char *value, uint16_t vc) {
  gfx->fillRoundRect(12, y, 296, h, 12, CARD);
  text(u8g2_font_helvB10_tf, MUTED, 24, y + 20, title);
  if (value) text(u8g2_font_helvB14_tf, vc, 284, y + 22, value, RIGHT);
  chevron(292, y + 15, MUTED);
  return y + 30;
}

void tile(int x, const char *label, const char *value, uint16_t vc, const char *sub) {
  gfx->fillRoundRect(x, 406, 96, 68, 12, CARD);
  text(u8g2_font_helvR08_tf, MUTED, x + 48, 421, label, CENTER);
  text(u8g2_font_helvB18_tf, vc, x + 48, 449, value, CENTER);
  text(u8g2_font_helvR08_tf, MUTED, x + 48, 466, sub, CENTER);
}

// Detail page header: "< Title" + big value on the right
void detailHeader(const char *title, const char *value, uint16_t vc) {
  for (int d = 0; d < 3; d++) {
    gfx->drawLine(22 + d, 16, 14 + d, 25, TXT);
    gfx->drawLine(14 + d, 25, 22 + d, 34, TXT);
  }
  text(u8g2_font_helvB18_tf, TXT, 34, 34, title);
  if (value) text(u8g2_font_helvB18_tf, vc, 306, 34, value, RIGHT);
  if (!live()) text(u8g2_font_helvR08_tf, C_BAD, 34, 50, "No signal from the Mac");
}

// Card holding a large 2.5-minute graph; returns the graph rect via gy/gh.
void historyCard(int y, int h, const char *label, const char *scale, int &gy, int &gh) {
  gfx->fillRoundRect(12, y, 296, h, 12, CARD);
  text(u8g2_font_helvR08_tf, MUTED, 24, y + 17, label);
  if (scale) text(u8g2_font_helvR08_tf, MUTED, 296, y + 17, scale, RIGHT);
  gy = y + 24;
  gh = h - 34;
  gridLines(22, gy, 276, gh);
  // time axis ticks every 30 s
  for (int s = 30; s < MAC_HIST; s += 30) {
    int x = 22 + 275 - s * 275 / (MAC_HIST - 1);
    gfx->drawFastVLine(x, gy + gh, 3, GRID);
  }
}

// Label above, value below; used in rows of small stats
void stat(int cx, int y, const char *label, const char *value, uint16_t vc) {
  text(u8g2_font_helvR08_tf, MUTED, cx, y + 16, label, CENTER);
  text(u8g2_font_helvB12_tf, vc, cx, y + 38, value, CENTER);
}

// "Top processes" list: name left, value right, with a faint bar showing the share
void topList(int y, int h, const char *title, const TopProc *rows, int n, const char *fmt, float full, uint16_t c,
             uint16_t barC) {
  gfx->fillRoundRect(12, y, 296, h, 12, CARD);
  text(u8g2_font_helvR08_tf, MUTED, 24, y + 17, title);
  char buf[24];
  for (int i = 0; i < n; i++) {
    int ry = y + 26 + i * 19;
    int bw = (int)(constrain(rows[i].value / full, 0.0f, 1.0f) * 272);
    if (bw > 0) gfx->fillRoundRect(24, ry, bw, 16, 4, barC);
    text(u8g2_font_helvR10_tf, TXT, 30, ry + 12, rows[i].name);
    snprintf(buf, sizeof(buf), fmt, rows[i].value);
    text(u8g2_font_helvB10_tf, c, 292, ry + 12, buf, RIGHT);
  }
  if (!n) text(u8g2_font_helvR10_tf, MUTED, 24, y + 40, "(waiting for data)");
}

// ---- Screens ----
void drawWaiting() {
  text(u8g2_font_helvB18_tf, TXT, W / 2, 150, "Mac Monitor", CENTER);
  text(u8g2_font_helvR10_tf, MUTED, W / 2, 176, "v" FW_VERSION, CENTER);
  const char *lines[] = {"1. Plug this screen into your Mac", "    with the USB-C cable", "2. On the Mac, run:",
                         "mac/run.sh", "3. Stats appear here in a second"};
  for (int i = 0; i < 5; i++)
    text(i == 3 ? u8g2_font_helvB12_tf : u8g2_font_helvR12_tf, i == 3 ? ACCENT : MUTED, 36, 230 + i * 30, lines[i]);
}

// Header of the two swipe pages: host + status on the left, time + date on the right, page dots in the middle
void drawHeader() {
  bool isLive = live();
  char buf[32];
  text(u8g2_font_helvB12_tf, TXT, 14, 26, MAC.host);
  gfx->fillCircle(18, 42, 4, isLive ? CPUC : C_BAD);
  text(u8g2_font_helvR08_tf, MUTED, 28, 46, isLive ? "USB live" : "No signal - is mac/run.sh running?");
  if (MAC.timeSet) {
    time_t t = time(nullptr) + MAC.tzOffset;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    text(u8g2_font_helvB14_tf, TXT, 306, 26, buf, RIGHT);
    if (isLive) {
      snprintf(buf, sizeof(buf), "%s %d %s", WD3[tm.tm_wday], tm.tm_mday, MON3[tm.tm_mon]);
      text(u8g2_font_helvR08_tf, MUTED, 306, 46, buf, RIGHT);
    }
  }
  for (int i = 0; i < 2; i++) {
    bool on = (i == 0) == (page == PG_OVERVIEW);
    gfx->fillCircle(W / 2 - 6 + i * 12, 42, 3, on ? TXT : GRID);
  }
}

void drawOverview() {
  char buf[72], sub[32];
  drawHeader();

  // CPU
  snprintf(buf, sizeof(buf), "%.0f%%", MAC.cpu);
  int gy = card(CPU_Y, CPU_H, "CPU", buf, CPUC);
  gridLines(22, gy, 276, 42);
  graph(22, gy, 276, 42, MAC.hCpu, OVERVIEW, 100, CPUC, rgb(24, 70, 50));

  // GPU
  if (MAC.gpu >= 0) snprintf(buf, sizeof(buf), "%.0f%%", MAC.gpu);
  else strcpy(buf, "--");
  gy = card(GPU_Y, GPU_H, "GPU", buf, GPUC);
  snprintf(sub, sizeof(sub), "%.1f GB in use", MAC.gpuMem);
  if (MAC.gpu >= 0) text(u8g2_font_helvR10_tf, MUTED, 104, GPU_Y + 20, sub);
  gridLines(22, gy, 276, 42);
  graph(22, gy, 276, 42, MAC.hGpu, OVERVIEW, 100, GPUC, rgb(90, 50, 24));

  // Memory: pressure (big number + graph, coloured like Activity Monitor), usage, swap, breakdown
  int lvl = constrain(MAC.memLevel, 0, 2);
  bool hasMp = MAC.memPressure >= 0;
  snprintf(buf, sizeof(buf), "%d%%", hasMp ? MAC.memPressure : (int)MAC.ram);
  card(MEM_Y, MEM_H, "Memory", buf, hasMp ? LEVEL_COLOR[lvl] : RAMC);
  if (hasMp) {
    gfx->fillRoundRect(86, MEM_Y + 7, 64, 17, 8, LEVEL_COLOR[lvl]);
    text(u8g2_font_helvB08_tf, BG, 118, MEM_Y + 20, LEVEL_NAME[lvl], CENTER);
    text(u8g2_font_helvR08_tf, MUTED, 228, MEM_Y + 20, "Pressure", RIGHT);
  }
  float used = MAC.memApp >= 0 ? MAC.memApp + MAC.memWired + MAC.memComp : MAC.ramUsed;
  snprintf(sub, sizeof(sub), "Used %.1f / %.0f GB", used, MAC.ramTotal);
  text(u8g2_font_helvR10_tf, TXT, 24, MEM_Y + 38, sub);
  snprintf(sub, sizeof(sub), "Swap %.1f GB", MAC.swap);
  text(u8g2_font_helvR10_tf, MAC.swap >= 4 ? C_UP : MUTED, 296, MEM_Y + 38, sub, RIGHT);
  if (MAC.memApp >= 0) {
    snprintf(buf, sizeof(buf), "App %.1f  Wired %.1f  Compressed %.1f  Cache %.1f", MAC.memApp, MAC.memWired,
             MAC.memComp, MAC.memCached);
    text(u8g2_font_helvR08_tf, MUTED, 24, MEM_Y + 53, buf);
  }
  gridLines(22, MEM_Y + 60, 276, 44);
  graph(22, MEM_Y + 60, 276, 44, MAC.hMp, OVERVIEW, 100, hasMp ? LEVEL_COLOR[lvl] : RAMC,
        hasMp ? LEVEL_FILL[lvl] : rgb(60, 45, 100));

  // Network: download (blue) + upload (yellow), auto-scaled
  gy = card(NET_Y, NET_H, "Network", nullptr, TXT);
  arrow(118, NET_Y + 20, false, ACCENT);
  fmtRate(buf, sizeof(buf), MAC.rx);
  text(u8g2_font_helvB10_tf, ACCENT, 127, NET_Y + 20, buf);
  arrow(206, NET_Y + 20, true, C_UP);
  fmtRate(buf, sizeof(buf), MAC.tx);
  text(u8g2_font_helvB10_tf, C_UP, 215, NET_Y + 20, buf);
  float peak = max(50e3f, max(histPeak(MAC.hRx, OVERVIEW), histPeak(MAC.hTx, OVERVIEW))) * 1.1f;
  gridLines(22, gy, 276, 28);
  graph(22, gy, 276, 28, MAC.hRx, OVERVIEW, peak, ACCENT, 0);
  graph(22, gy, 276, 28, MAC.hTx, OVERVIEW, peak, C_UP, 0);

  // Temperature / battery / disk
  if (MAC.temp > 0) {
    snprintf(buf, sizeof(buf), "%.0f\xC2\xB0" "C", MAC.temp);
    uint16_t tc = MAC.temp < 60 ? CPUC : MAC.temp < 80 ? C_UP : C_BAD;
    tile(12, "CPU Temp", buf, tc, MAC.temp < 60 ? "Cool" : MAC.temp < 80 ? "Warm" : "Hot");
  } else {
    tile(12, "CPU Temp", "--", MUTED, "n/a");
  }
  if (MAC.bat >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", MAC.bat);
    uint16_t bc = MAC.charging ? CPUC : MAC.bat > 20 ? TXT : C_BAD;
    tile(112, "Battery", buf, bc, MAC.charging ? "Charging" : MAC.onAC ? "On power" : "On battery");
  } else {
    tile(112, "Power", "AC", TXT, "No battery");
  }
  float pct = MAC.diskTotal > 0 ? MAC.diskUsed * 100 / MAC.diskTotal : 0;
  snprintf(buf, sizeof(buf), "%.0f%%", pct);
  snprintf(sub, sizeof(sub), "%.0f GB free", MAC.diskTotal - MAC.diskUsed);
  tile(212, "Disk used", buf, pct < 90 ? TXT : C_BAD, sub);
}

// ---- Detail: CPU ----
void drawCpuDetail() {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.0f%%", MAC.cpu);
  detailHeader("CPU", buf, CPUC);

  int gy, gh;
  historyCard(56, 120, "Total load - last 2.5 min", "100%", gy, gh);
  graph(22, gy, 276, gh, MAC.hCpu, MAC_HIST, 100, CPUC, rgb(24, 70, 50));

  // User / System / Load / Temp
  gfx->fillRoundRect(12, 182, 296, 50, 12, CARD);
  snprintf(buf, sizeof(buf), "%.0f%%", MAC.cpuUser);
  stat(49, 182, "User", buf, CPUC);
  snprintf(buf, sizeof(buf), "%.0f%%", MAC.cpuSys);
  stat(123, 182, "System", buf, C_BAD);
  snprintf(buf, sizeof(buf), "%.1f", MAC.load[0]);
  stat(197, 182, "Load 1 min", buf, TXT);
  if (MAC.temp > 0) snprintf(buf, sizeof(buf), "%.0f\xC2\xB0" "C", MAC.temp);
  else strcpy(buf, "--");
  stat(271, 182, "Temp", buf, MAC.temp < 60 ? CPUC : MAC.temp < 80 ? C_UP : C_BAD);

  // Per-core bars: performance cores green, efficiency cores blue
  gfx->fillRoundRect(12, 238, 296, 110, 12, CARD);
  int n = MAC.nCores, p = min((int)MAC.pCores, n);
  snprintf(buf, sizeof(buf), "Cores: %d Performance, %d Efficiency", p, n - p);
  text(u8g2_font_helvR08_tf, MUTED, 24, 255, p ? buf : "Cores");
  if (n) {
    int gap = 4, bw = (272 - gap * (n - 1)) / n, bx = 24 + (272 - (bw * n + gap * (n - 1))) / 2;
    for (int i = 0; i < n; i++) {
      int x = bx + i * (bw + gap), top = 264, bh = 62;
      int fh = MAC.cores[i] * bh / 100;
      bool perf = i < p;
      gfx->fillRoundRect(x, top, bw, bh, 3, GRID);
      if (fh > 0) gfx->fillRoundRect(x, top + bh - fh, bw, fh, 3, perf ? CPUC : ACCENT);
      snprintf(buf, sizeof(buf), "%c%d", perf ? 'P' : 'E', perf ? i + 1 : i - p + 1);
      text(u8g2_font_helvR08_tf, MUTED, x + bw / 2, 340, buf, CENTER);
    }
  }

  // Top processes by CPU (100% = one full core)
  topList(354, 120, "Top apps by CPU (100% = one core)", MAC.topCpu, MAC.nTopCpu, "%.0f%%",
          max(100.0f, MAC.nCores * 100.0f), CPUC, rgb(24, 60, 44));
}

// ---- Detail: GPU ----
void drawGpuDetail() {
  char buf[32];
  if (MAC.gpu >= 0) snprintf(buf, sizeof(buf), "%.0f%%", MAC.gpu);
  else strcpy(buf, "--");
  detailHeader("GPU", buf, GPUC);

  int gy, gh;
  historyCard(56, 170, "GPU busy - last 2.5 min", "100%", gy, gh);
  graph(22, gy, 276, gh, MAC.hGpu, MAC_HIST, 100, GPUC, rgb(90, 50, 24));

  gfx->fillRoundRect(12, 232, 296, 50, 12, CARD);
  if (MAC.gpuRender >= 0) snprintf(buf, sizeof(buf), "%.0f%%", MAC.gpuRender);
  else strcpy(buf, "--");
  stat(49, 232, "Renderer", buf, GPUC);
  if (MAC.gpuTiler >= 0) snprintf(buf, sizeof(buf), "%.0f%%", MAC.gpuTiler);
  else strcpy(buf, "--");
  stat(123, 232, "Tiler", buf, GPUC);
  fmtGB(buf, sizeof(buf), MAC.gpuMem);
  stat(197, 232, "Mem in use", buf, TXT);
  fmtGB(buf, sizeof(buf), MAC.gpuAlloc);
  stat(271, 232, "Allocated", buf, MUTED);

  float top = max(1.0f, histPeak(MAC.hGpuMem, MAC_HIST) * 1.2f);
  char scale[16];
  snprintf(scale, sizeof(scale), "%.1f GB", top);
  historyCard(288, 186, "GPU memory in use - last 2.5 min", scale, gy, gh);
  graph(22, gy, 276, gh, MAC.hGpuMem, MAC_HIST, top, rgb(255, 190, 120), rgb(70, 45, 25));
}

// ---- Detail: Memory ----
void drawMemDetail() {
  char buf[48];
  int lvl = constrain(MAC.memLevel, 0, 2);
  bool hasMp = MAC.memPressure >= 0;
  snprintf(buf, sizeof(buf), "%d%%", hasMp ? MAC.memPressure : (int)MAC.ram);
  detailHeader("Memory", buf, hasMp ? LEVEL_COLOR[lvl] : RAMC);
  if (hasMp) {
    gfx->fillRoundRect(140, 16, 70, 20, 10, LEVEL_COLOR[lvl]);
    text(u8g2_font_helvB08_tf, BG, 175, 30, LEVEL_NAME[lvl], CENTER);
  }

  int gy, gh;
  historyCard(56, 110, "Memory pressure - last 2.5 min", "100%", gy, gh);
  graph(22, gy, 276, gh, MAC.hMp, MAC_HIST, 100, hasMp ? LEVEL_COLOR[lvl] : RAMC,
        hasMp ? LEVEL_FILL[lvl] : rgb(60, 45, 100));

  // Breakdown: stacked bar + legend
  gfx->fillRoundRect(12, 172, 296, 112, 12, CARD);
  float total = max(MAC.ramTotal, 1.0f);
  bool split = MAC.memApp >= 0;
  float used = split ? MAC.memApp + MAC.memWired + MAC.memComp : MAC.ramUsed;
  snprintf(buf, sizeof(buf), "Used %.1f of %.0f GB", used, MAC.ramTotal);
  text(u8g2_font_helvB10_tf, TXT, 24, 190, buf);
  const char *names[] = {"App", "Wired", "Compressed", "Cache", "Free"};
  const uint16_t cols[] = {RAMC, GPUC, C_UP, ACCENT, GRID};
  float vals[5] = {split ? MAC.memApp : MAC.ramUsed, MAC.memWired, MAC.memComp, MAC.memCached, 0};
  if (!split) vals[1] = vals[2] = vals[3] = 0;
  vals[4] = max(0.0f, total - vals[0] - vals[1] - vals[2] - vals[3]);
  gfx->fillRoundRect(24, 198, 272, 22, 6, GRID);
  int x = 24;
  for (int i = 0; i < 4; i++) {
    int w = (int)(vals[i] / total * 272 + 0.5f);
    w = min(w, 296 - x);
    if (w > 0) gfx->fillRect(x, 198, w, 22, cols[i]);
    x += w;
  }
  for (int i = 0; i < 5; i++) {
    int lx = i < 3 ? 24 : 168, ly = 236 + (i < 3 ? i : i - 3) * 16;
    gfx->fillRoundRect(lx, ly - 9, 10, 10, 2, cols[i]);
    char v[16];
    fmtGB(v, sizeof(v), vals[i]);
    snprintf(buf, sizeof(buf), "%s  %s", names[i], v);
    text(u8g2_font_helvR10_tf, i == 4 ? MUTED : TXT, lx + 16, ly, buf);
  }
  text(u8g2_font_helvR08_tf, MUTED, 168, 270, "Cache is freed on demand");

  // Swap
  gfx->fillRoundRect(12, 290, 296, 50, 12, CARD);
  text(u8g2_font_helvB10_tf, MUTED, 24, 308, "Swap");
  snprintf(buf, sizeof(buf), "%.1f of %.1f GB", MAC.swap, MAC.swapTotal);
  text(u8g2_font_helvB10_tf, MAC.swap >= 4 ? C_UP : TXT, 296, 308, buf, RIGHT);
  gfx->fillRoundRect(24, 318, 272, 10, 5, GRID);
  if (MAC.swapTotal > 0) {
    int w = (int)(constrain(MAC.swap / MAC.swapTotal, 0.0f, 1.0f) * 272);
    if (w > 0) gfx->fillRoundRect(24, 318, w, 10, 5, MAC.swap >= 4 ? C_UP : RAMC);
  }

  topList(346, 128, "Top apps by memory", MAC.topMem, MAC.nTopMem, "%.2f GB", total, RAMC, rgb(55, 42, 90));
}

// ---- Detail: Network ----
void drawNetDetail() {
  char buf[48], peakBuf[24];
  detailHeader("Network", nullptr, TXT);

  int gy, gh;
  float pRx = histPeak(MAC.hRx, MAC_HIST), pTx = histPeak(MAC.hTx, MAC_HIST);

  fmtRate(peakBuf, sizeof(peakBuf), max(pRx, 50e3f) * 1.1f);
  historyCard(56, 130, "", peakBuf, gy, gh);
  arrow(30, 73, false, ACCENT);
  fmtRate(buf, sizeof(buf), MAC.rx);
  text(u8g2_font_helvB10_tf, ACCENT, 40, 73, "Download");
  text(u8g2_font_helvB12_tf, ACCENT, 200, 74, buf, RIGHT);
  graph(22, gy, 276, gh, MAC.hRx, MAC_HIST, max(pRx, 50e3f) * 1.1f, ACCENT, rgb(25, 50, 85));

  fmtRate(peakBuf, sizeof(peakBuf), max(pTx, 50e3f) * 1.1f);
  historyCard(192, 130, "", peakBuf, gy, gh);
  arrow(30, 209, true, C_UP);
  fmtRate(buf, sizeof(buf), MAC.tx);
  text(u8g2_font_helvB10_tf, C_UP, 40, 209, "Upload");
  text(u8g2_font_helvB12_tf, C_UP, 200, 210, buf, RIGHT);
  graph(22, gy, 276, gh, MAC.hTx, MAC_HIST, max(pTx, 50e3f) * 1.1f, C_UP, rgb(80, 64, 20));

  // Connection info
  gfx->fillRoundRect(12, 328, 296, 146, 12, CARD);
  char a[24], b[24];
  const char *labels[] = {"Connection", "Interface", "IP address", "Received since boot", "Sent since boot",
                          "Peak (2.5 min)"};
  for (int i = 0; i < 6; i++) {
    int y = 350 + i * 22;
    text(u8g2_font_helvR10_tf, MUTED, 24, y, labels[i]);
    const char *v = buf;
    switch (i) {
      case 0: v = MAC.ifType[0] ? MAC.ifType : "--"; break;
      case 1: v = MAC.ifName[0] ? MAC.ifName : "--"; break;
      case 2: v = MAC.ip[0] ? MAC.ip : "--"; break;
      case 3: fmtGB(buf, sizeof(buf), MAC.rxTotal); break;
      case 4: fmtGB(buf, sizeof(buf), MAC.txTotal); break;
      case 5:
        fmtRate(a, sizeof(a), pRx);
        fmtRate(b, sizeof(b), pTx);
        snprintf(buf, sizeof(buf), "%s / %s", a, b);
        break;
    }
    text(u8g2_font_helvB10_tf, TXT, 296, y, v, RIGHT);
  }
}

// ---- Page: Claude Code usage (swipe from the main screen) ----
void fmtTok(char *buf, size_t n, float v) {
  if (v >= 1e9f) snprintf(buf, n, "%.1fB", v / 1e9f);
  else if (v >= 1e6f) snprintf(buf, n, "%.1fM", v / 1e6f);
  else if (v >= 1e4f) snprintf(buf, n, "%.0fK", v / 1e3f);
  else if (v >= 1e3f) snprintf(buf, n, "%.1fK", v / 1e3f);
  else snprintf(buf, n, "%.0f", v);
}

void fmtClock(char *buf, size_t n, long unixTime) {
  time_t t = unixTime + MAC.tzOffset;
  struct tm tm;
  gmtime_r(&t, &tm);
  snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void drawClaudePage() {
  drawHeader();
  const ClaudeStats &c = MAC.claude;
  char buf[48], a[16];
  if (!c.any) {
    text(u8g2_font_helvB14_tf, CLAUDEC, W / 2, 200, "Claude Code usage", CENTER);
    text(u8g2_font_helvR10_tf, MUTED, W / 2, 230, "Waiting for data from the Mac...", CENTER);
    text(u8g2_font_helvR10_tf, MUTED, W / 2, 252, "(updates every 10 seconds)", CENTER);
    return;
  }

  // Today
  gfx->fillRoundRect(12, 54, 296, 112, 12, CARD);
  text(u8g2_font_helvB10_tf, MUTED, 24, 74, "Claude Code - today");
  snprintf(buf, sizeof(buf), "%d requests", c.tdReq);
  text(u8g2_font_helvR10_tf, MUTED, 296, 74, buf, RIGHT);
  fmtTok(buf, sizeof(buf), c.tdOut);
  text(u8g2_font_helvB24_tf, CLAUDEC, 24, 110, buf);
  text(u8g2_font_helvR10_tf, MUTED, 32 + textWidth(u8g2_font_helvB24_tf, buf), 110, "output tokens");
  fmtTok(buf, sizeof(buf), c.tdIn + c.tdCacheWrite);
  stat(61, 118, "Input + cache write", buf, TXT);
  fmtTok(buf, sizeof(buf), c.tdCacheRead);
  stat(160, 118, "Cache read", buf, MUTED);
  fmtTok(buf, sizeof(buf), c.tdIn + c.tdOut + c.tdCacheRead + c.tdCacheWrite);
  stat(259, 118, "Total", buf, TXT);

  // Current 5-hour window
  gfx->fillRoundRect(12, 172, 296, 100, 12, CARD);
  text(u8g2_font_helvB10_tf, MUTED, 24, 192, "5-hour window");
  long now = time(nullptr);
  if (c.bkActive && MAC.timeSet) {
    fmtClock(a, sizeof(a), c.bkEnd);
    snprintf(buf, sizeof(buf), "resets %s", a);
    text(u8g2_font_helvB10_tf, TXT, 296, 192, buf, RIGHT);
    float frac = constrain((float)(now - c.bkStart) / max(1L, c.bkEnd - c.bkStart), 0.0f, 1.0f);
    gfx->fillRoundRect(24, 200, 272, 10, 5, GRID);
    gfx->fillRoundRect(24, 200, max(10, (int)(272 * frac)), 10, 5, CLAUDEC);
    long left = max(0L, c.bkEnd - now) / 60;
    fmtClock(a, sizeof(a), c.bkStart);
    snprintf(buf, sizeof(buf), "Started %s  -  %ldh %02ldm left", a, left / 60, left % 60);
    text(u8g2_font_helvR08_tf, MUTED, 24, 224, buf);
    fmtTok(buf, sizeof(buf), c.bkOut);
    stat(61, 228, "Output tokens", buf, CLAUDEC);
    snprintf(buf, sizeof(buf), "%d", c.bkReq);
    stat(160, 228, "Requests", buf, TXT);
    fmtTok(a, sizeof(a), c.burn);
    snprintf(buf, sizeof(buf), "%s/min", a);
    stat(259, 228, "Output rate", buf, TXT);
  } else {
    text(u8g2_font_helvR10_tf, MUTED, 24, 224, "No active window.");
    text(u8g2_font_helvR10_tf, MUTED, 24, 244, "The next request starts a new one.");
  }

  // Output tokens per minute, last hour (bars)
  uint32_t peak = 1000;
  for (int i = 0; i < 60; i++) peak = max(peak, c.perMin[i]);
  fmtTok(a, sizeof(a), peak);
  gfx->fillRoundRect(12, 278, 296, 100, 12, CARD);
  text(u8g2_font_helvR08_tf, MUTED, 24, 295, "Output tokens per minute - last hour");
  text(u8g2_font_helvR08_tf, MUTED, 296, 295, a, RIGHT);
  int gy = 302, gh = 66;
  gridLines(22, gy, 276, gh);
  for (int i = 0; i < 60; i++) {
    int bh = (int)((float)c.perMin[i] / peak * gh);
    int x = 22 + i * 276 / 60;
    if (bh > 0) gfx->fillRect(x, gy + gh - bh, 3, bh, i == 59 ? TXT : CLAUDEC);
  }
  for (int m = 15; m < 60; m += 15) gfx->drawFastVLine(22 + (60 - m) * 276 / 60, gy + gh, 3, GRID);

  // Models + projects today
  gfx->fillRoundRect(12, 384, 296, 90, 12, CARD);
  text(u8g2_font_helvR08_tf, MUTED, 24, 400, "Models (output)");
  text(u8g2_font_helvR08_tf, MUTED, 168, 400, "Projects (tokens)");
  for (int i = 0; i < c.nModels; i++) {
    text(u8g2_font_helvR08_tf, TXT, 24, 418 + i * 16, c.models[i].name);
    fmtTok(buf, sizeof(buf), c.models[i].out);
    text(u8g2_font_helvB08_tf, CLAUDEC, 152, 418 + i * 16, buf, RIGHT);
  }
  for (int i = 0; i < c.nProjects; i++) {
    char name[16];
    strlcpy(name, c.projects[i].name, sizeof(name));
    text(u8g2_font_helvR08_tf, TXT, 168, 418 + i * 16, name);
    fmtTok(buf, sizeof(buf), c.projects[i].tokens);
    text(u8g2_font_helvB08_tf, TXT, 296, 418 + i * 16, buf, RIGHT);
  }
}

void render() {
  gfx->fillScreen(BG);
  if (!MAC.any) drawWaiting();
  else switch (page) {
      case PG_OVERVIEW: drawOverview(); break;
      case PG_CPU: drawCpuDetail(); break;
      case PG_GPU: drawGpuDetail(); break;
      case PG_MEM: drawMemDetail(); break;
      case PG_NET: drawNetDetail(); break;
      case PG_CLAUDE: drawClaudePage(); break;
    }
  gfx->flush();
}

// ---- Backlight: bright while data flows, dim when the Mac goes quiet ----
uint32_t wokenAt = 0;
int duty = -1;
bool dimmed = false;

void applyBacklight() {
  uint32_t now = millis();
  bool active = (MAC.any ? now - MAC.lastMs < DIM_AFTER : now < DIM_AFTER) ||
                (wokenAt && now - wokenAt < WAKE_FOR);
  dimmed = !active;
  int target = (active ? BRIGHT : DIM) * 255 / 100;
  if (duty == target) return;
  if (duty < 0) duty = target;
  else {  // smooth fade
    int step = (target - duty) / 6;
    if (step == 0) step = target > duty ? 1 : -1;
    duty += step;
  }
  ledcWrite(0, duty);
}

// ---- Touch ----
bool readTouch(int &x, int &y) {
  static const uint8_t cmd[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0, 0, 0, 0x08, 0, 0, 0};
  Wire.beginTransmission(TP_ADDR);
  Wire.write(cmd, sizeof(cmd));
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(TP_ADDR, 8) != 8) return false;
  uint8_t b[8];
  for (int i = 0; i < 8; i++) b[i] = Wire.read();
  if (b[1] == 0 || b[1] > 2) return false;
  x = ((b[2] & 0x0F) << 8) | b[3];
  y = ((b[4] & 0x0F) << 8) | b[5];
  return x < W && y < H;
}

void handleTap(int x, int y) {
  if (!MAC.any) return;
  if (page == PG_CLAUDE) {
    return;
  } else if (page != PG_OVERVIEW) {  // anywhere on a detail page goes back
    page = PG_OVERVIEW;
  } else if (y >= CPU_Y && y < CPU_Y + CPU_H) {
    page = PG_CPU;
  } else if (y >= GPU_Y && y < GPU_Y + GPU_H) {
    page = PG_GPU;
  } else if (y >= MEM_Y && y < MEM_Y + MEM_H) {
    page = PG_MEM;
  } else if (y >= NET_Y && y < NET_Y + NET_H) {
    page = PG_NET;
  } else if (y >= 406 && x < 108) {  // CPU temp tile
    page = PG_CPU;
  } else {
    return;
  }
  render();
}

void pollTouch() {
  static bool down = false, wakeOnly = false;
  static int sx, sy, lx, ly;
  int x, y;
  if (readTouch(x, y)) {
    if (!down) {
      down = true;
      wakeOnly = dimmed;  // first touch on a dimmed screen only wakes it
      sx = lx = x;
      sy = ly = y;
    }
    lx = x;
    ly = y;
    wokenAt = millis();
  } else if (down) {
    down = false;
    int dx = lx - sx, dy = ly - sy;
    if (wakeOnly) return;
    if (abs(dx) > 60 && abs(dx) > abs(dy) && (page == PG_OVERVIEW || page == PG_CLAUDE) && MAC.any) {
      page = page == PG_OVERVIEW ? PG_CLAUDE : PG_OVERVIEW;  // swipe between the two main pages
      render();
    } else if (abs(dx) < 40 && abs(dy) < 40) {
      handleTap(sx, sy);
    }
  }
}

// ---- Screenshots (driven by mac/screenshot.sh through the sender) ----
// "SHOT p" renders page p and holds it on screen; the Mac then pulls it in small blocks with
// "ROWS y n" (reply: "ROWS y n <byte sum>\n" + raw RGB565 little-endian) so the USB buffer never
// overflows, and ends with "DONE". Normal redraws pause meanwhile (max 10 s).
Page shotSaved;
uint32_t shotAt = 0;

void shotService() {
  if (shotRequest >= 0) {
    if (!shotActive) shotSaved = page;
    shotActive = true;
    page = (Page)constrain((int)shotRequest, 0, (int)PG_CLAUDE);
    shotRequest = -1;
    render();
    Serial.printf("SHOT %d %d\n", W, H);
    shotAt = millis();
  }
  if (rowsY >= 0) {
    int y = constrain((int)rowsY, 0, H - 1), n = constrain((int)rowsN, 1, H - y);
    rowsY = -1;
    const uint8_t *p = (const uint8_t *)(gfx->getFramebuffer() + y * W);
    size_t bytes = (size_t)n * W * 2;
    uint32_t sum = 0;
    for (size_t i = 0; i < bytes; i++) sum += p[i];
    Serial.printf("ROWS %d %d %u\n", y, n, sum);
    // Copy through internal RAM in small pieces and flush after each so the USB-serial FIFO keeps moving.
    static uint8_t chunk[64];
    size_t left = bytes;
    while (left) {
      size_t k = min(left, sizeof(chunk));
      memcpy(chunk, p, k);
      size_t done = 0;
      uint32_t t0 = millis();
      while (done < k && millis() - t0 < 1000) {
        size_t w = Serial.write(chunk + done, k - done);
        done += w;
        if (!w) delay(1);
      }
      p += k;
      left -= k;
    }
    shotAt = millis();
  }
  if (shotActive && (shotDone || millis() - shotAt > 10000)) {
    shotActive = false;
    page = shotSaved;
    render();
  }
  shotDone = false;
}

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF);  // not needed: everything comes over USB
  ledcSetup(0, 5000, 8);
  ledcAttachPin(LCD_BL, 0);
  applyBacklight();
  if (!gfx->begin()) Serial.println("gfx->begin() failed");
  gfx->setUTF8Print(true);
  Wire.begin(TP_SDA, TP_SCL, 400000);
  render();
  Serial.printf("Mac Monitor v%s ready\n", FW_VERSION);
}

void loop() {
  bool fresh = macPoll();
  shotService();
  if (shotActive) return;  // hold the captured page still until the Mac has it

  static uint32_t lastTouchPoll = 0;
  if (millis() - lastTouchPoll >= 20) {
    lastTouchPoll = millis();
    pollTouch();
  }
  applyBacklight();

  static uint32_t lastSec = 0;
  uint32_t s = millis() / 1000;
  if (fresh || s != lastSec) render();  // redraw on new data, and each second for the clock / status
  lastSec = s;
  delay(5);
}
