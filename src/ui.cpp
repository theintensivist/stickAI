#include "ui.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <vector>
#include "arabic.h"
#include "font_ar.h"
#include <time.h>

namespace ui {

static constexpr int W = 240, H = 135, BAR = 18;
static constexpr int LINE_H = 15;     // pages and menus (DejaVu12)
static int chatLineH = 17;            // conversation (smooth English+Arabic font), set in begin()
static String clockOverride;           // e.g. next timer countdown, shown instead of the clock
static constexpr int PAD = 3;
static constexpr size_t MAX_PARAS = 24;

static const uint16_t COL_USER = 0x5E7F;    // light blue
static const uint16_t COL_AI = 0xFFFF;      // white
static const uint16_t COL_SYS = 0x9CF3;     // grey
static const uint16_t COL_BAR = 0x18E3;     // dark grey

struct Para { int id; Role role; String text; };
static int nextId = 1;
static std::vector<Para> paras;
static String statusText = "";
static uint16_t statusColor = 0xFFFF;
static int scroll = 0;            // lines scrolled up from the bottom
static bool dirty = true;
static bool fullscreen = false;   // a message/QR screen is showing
static uint32_t lastBarDraw = 0;
static M5Canvas* canvas = nullptr;

static uint16_t roleColor(Role r) {
  return r == Role::User ? COL_USER : r == Role::Assistant ? COL_AI : COL_SYS;
}

// Word-wrap text to the canvas width with the current font.
static void wrap(const String& text, int width, std::vector<String>& out, lgfx::LGFXBase* g = nullptr) {
  if (!g) g = &M5.Display;   // pages/messages are drawn on the display with DejaVu12
  String line, word;
  auto flushWord = [&]() {
    if (word.isEmpty()) return;
    String trial = line.isEmpty() ? word : line + " " + word;
    if (g->textWidth(trial.c_str()) <= width) {
      line = trial;
    } else {
      if (!line.isEmpty()) out.push_back(line);
      // break very long words
      while (g->textWidth(word.c_str()) > width && word.length() > 1) {
        int n = word.length() - 1;
        while (n > 1 && g->textWidth(word.substring(0, n).c_str()) > width) n--;
        out.push_back(word.substring(0, n));
        word = word.substring(n);
      }
      line = word;
    }
    word = "";
  };
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '\n') { flushWord(); out.push_back(line); line = ""; }
    else if (c == ' ' || c == '\t' || c == '\r') flushWord();
    else word += c;
  }
  flushWord();
  if (!line.isEmpty()) out.push_back(line);
}

static void drawBar() {
  auto& d = M5.Display;
  d.fillRect(0, 0, W, BAR, COL_BAR);
  d.setFont(&fonts::DejaVu12);
  d.setTextDatum(middle_left);
  d.setTextColor(statusColor, COL_BAR);
  d.drawString(statusText, 4, BAR / 2);

  int bat = M5.Power.getBatteryLevel();
  String b = (bat >= 0 ? String(bat) + "%" : String("--"));
  if (M5.Power.isCharging()) b = "+" + b;
  d.setTextDatum(middle_right);
  d.setTextColor(bat >= 0 && bat < 20 ? TFT_RED : TFT_WHITE, COL_BAR);
  d.drawString(b, W - 4, BAR / 2);

  // Wi-Fi dot: green connected, red not
  d.fillCircle(W - 44, BAR / 2, 3, WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED);

  // clock (once the time is known), or the next timer's countdown
  time_t now = time(nullptr);
  if (clockOverride.length()) {
    d.setTextColor(TFT_YELLOW, COL_BAR);
    d.drawString(clockOverride, W - 52, BAR / 2);
  } else if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    char hm[8];
    strftime(hm, sizeof(hm), "%H:%M", &t);
    d.setTextColor(TFT_WHITE, COL_BAR);
    d.drawString(hm, W - 52, BAR / 2);
  }
  lastBarDraw = millis();
}

void begin(uint8_t rotation) {
  M5.Display.setRotation(rotation);
  M5.Display.setBrightness(110);
  M5.Display.fillScreen(TFT_BLACK);
  canvas = new M5Canvas(&M5.Display);
  canvas->setPsram(true);
  canvas->setColorDepth(16);
  canvas->createSprite(W, H - BAR);
  // Smooth font with English and Arabic letters for the conversation captions
  if (!canvas->loadFont(FONT_AR)) canvas->setFont(&fonts::DejaVu12);
  canvas->setTextWrap(false);
  chatLineH = canvas->fontHeight() + 1;
  if (chatLineH < 15) chatLineH = 15;
}

void setClockText(const String& t) {
  if (t == clockOverride) return;
  clockOverride = t;
  if (!fullscreen) drawBar();
}

void status(const char* text, uint16_t color) {
  statusText = text;
  statusColor = color;
  if (!fullscreen) drawBar();
}

void clearChat() {
  paras.clear();
  scroll = 0;
  dirty = true;
}

int addLine(Role role, const String& text) {
  int id = nextId++;
  paras.push_back({id, role, text});
  while (paras.size() > MAX_PARAS) paras.erase(paras.begin());
  scroll = 0;
  dirty = true;
  return id;
}

void setText(int id, const String& text) {
  for (auto& p : paras) {
    if (p.id == id) {
      if (p.text != text) { p.text = text; scroll = 0; dirty = true; }
      return;
    }
  }
}

void scrollUp() {
  scroll += (H - BAR) / chatLineH - 1;
  dirty = true;  // clamped in render()
}

void scrollToEnd() { scroll = 0; dirty = true; }

void forceRender() { dirty = true; fullscreen = false; render(); }

void render() {
  if (fullscreen) return;
  if (millis() - lastBarDraw > 10000) drawBar();  // refresh battery/Wi-Fi now and then
  if (!dirty || !canvas) return;
  dirty = false;

  const int width = W - 2 * PAD;
  struct Ln { String s; uint16_t c; bool rtl; };
  std::vector<Ln> lines;
  for (auto& p : paras) {
    std::vector<String> w;
    std::string t = p.text.c_str();
    if (arabic::hasArabic(t)) {
      // Arabic: join the letters, wrap in reading order, then lay each line out right-to-left
      wrap(String(arabic::shape(t).c_str()), width, w, canvas);
      for (auto& s : w) lines.push_back({String(arabic::toVisual(s.c_str()).c_str()), roleColor(p.role), true});
    } else {
      wrap(p.text, width, w, canvas);
      for (auto& s : w) lines.push_back({s, roleColor(p.role), false});
    }
    lines.push_back({"", 0, false});  // gap between paragraphs
  }
  if (!lines.empty()) lines.pop_back();

  const int visible = (H - BAR) / chatLineH;
  int maxScroll = (int)lines.size() > visible ? (int)lines.size() - visible : 0;
  if (scroll > maxScroll) scroll = 0;  // wrapped past the top: back to newest
  int first = (int)lines.size() - visible - scroll;
  if (first < 0) first = 0;

  canvas->fillScreen(TFT_BLACK);
  canvas->setTextDatum(top_left);
  int y = 1;
  for (int i = first; i < (int)lines.size() && y + chatLineH <= H - BAR + 2; i++) {
    canvas->setTextColor(lines[i].c ? lines[i].c : TFT_BLACK, TFT_BLACK);
    int x = lines[i].rtl ? W - PAD - canvas->textWidth(lines[i].s) : PAD;
    canvas->drawString(lines[i].s, x, y);
    y += chatLineH;
  }
  if (scroll > 0) {  // small marker when looking at history
    canvas->fillTriangle(W - 8, 4, W - 2, 4, W - 5, 0, COL_SYS);
  }
  canvas->pushSprite(0, BAR);
}

// ---------------------------------------------------------------- start-up screen
// Logo: an AI chip (rounded square with pins) holding a heartbeat trace that
// turns into voice bars - medicine meets a talking assistant.
static constexpr int LX = 16, LY = 30, LS = 66;       // chip box
static constexpr int LCX = LX + LS / 2, LCY = LY + LS / 2;
static constexpr int TX = 96;                          // name text starts here

static void drawSplashFrame(M5Canvas& c, int f, const String& status) {
  const uint16_t CY = c.color565(0, 210, 200);
  const uint16_t CYD = c.color565(0, 95, 92);
  const uint16_t GREY = c.color565(150, 150, 150);
  c.fillScreen(TFT_BLACK);

  // chip outline (2 px) and pins
  c.drawRoundRect(LX, LY, LS, LS, 12, CY);
  c.drawRoundRect(LX + 1, LY + 1, LS - 2, LS - 2, 11, CY);
  for (int i = 0; i < 3; i++) {
    int o = LS / 4 * (i + 1);
    c.fillRect(LX - 7, LY + o - 1, 6, 3, CYD);        // left
    c.fillRect(LX + LS + 1, LY + o - 1, 6, 3, CYD);   // right
    c.fillRect(LX + o - 1, LY - 7, 3, 6, CYD);        // top
    c.fillRect(LX + o - 1, LY + LS + 1, 3, 6, CYD);   // bottom
  }

  // heartbeat trace, drawn left to right, then voice bars
  static const int16_t px[] = {-26, -18, -15, -12, -9, -7, -4, -1, 2, 5};
  static const int16_t py[] = {0, 0, -4, 0, 0, 4, -19, 13, 0, 0};
  static const int16_t bx[] = {9, 14, 19, 24};
  static const int16_t bh[] = {10, 20, 28, 14};
  float t = f >= 20 ? 1.0f : f / 20.0f;
  float xLim = -26 + t * 54;                            // -26 .. 28
  for (int i = 0; i + 1 < (int)(sizeof(px) / sizeof(px[0])); i++) {
    float x0 = px[i], y0 = py[i], x1 = px[i + 1], y1 = py[i + 1];
    if (x0 > xLim) break;
    if (x1 > xLim) { y1 = y0 + (y1 - y0) * (xLim - x0) / (x1 - x0); x1 = xLim; }
    for (int d = 0; d < 2; d++)
      c.drawLine(LCX + x0, LCY + y0 + d, LCX + x1, LCY + y1 + d, TFT_WHITE);
  }
  for (int i = 0; i < 4; i++) {
    if (xLim >= bx[i]) c.fillRoundRect(LCX + bx[i] - 1, LCY - bh[i] / 2, 3, bh[i], 1, CY);
  }

  // one pulse ring around the chip
  if (f >= 30 && f < 38) {
    int r = LS / 2 + 6 + (f - 30) * 2;
    c.drawCircle(LCX, LCY, r, CYD);
  }

  // name
  c.setTextDatum(baseline_left);
  c.setFont(&fonts::FreeSansBold18pt7b);
  int wStick = c.textWidth("stick");
  if (f >= 16) { c.setTextColor(TFT_WHITE); c.drawString("stick", TX, 62); }
  if (f >= 21) { c.setTextColor(CY); c.drawString("AI", TX + wStick + 1, 62); }
  if (f >= 25) {
    int w = c.textWidth("stickAI") + 2;
    int grow = (f - 25) * 14;
    c.fillRect(TX, 69, grow < w ? grow : w, 2, CYD);
  }
  if (f >= 28) {
    c.setTextDatum(top_left);
    c.setFont(&fonts::DejaVu12);
    c.setTextColor(GREY);
    c.drawString("by", TX, 75);
    // third line: the name, shrunk if it would not fit the screen
    const char* name = "Dr Abhijeet Anand";
    c.setFont(&fonts::DejaVu12);
    if (TX + c.textWidth(name) > W - 3) c.setFont(&fonts::DejaVu9);
    c.setTextColor(TFT_WHITE);
    c.drawString(name, TX, 90);
  }
  if (status.length()) {
    c.setFont(&fonts::DejaVu12);
    c.setTextDatum(middle_center);
    c.setTextColor(GREY);
    c.drawString(status, W / 2, H - 9);
  }
}

static M5Canvas* splashCanvas() {
  static M5Canvas* sc = nullptr;
  if (!sc) {
    sc = new M5Canvas(&M5.Display);
    sc->setPsram(true);
    sc->setColorDepth(16);
    if (!sc->createSprite(W, H)) { delete sc; sc = nullptr; }
  }
  return sc;
}

static String splashLast;

void splash() {
  fullscreen = true;
  M5Canvas* c = splashCanvas();
  if (!c) { message("stickAI", "by Dr Abhijeet Anand", TFT_CYAN); return; }
  for (int f = 0; f <= 44; f++) {
    drawSplashFrame(*c, f, "");
    c->pushSprite(0, 0);
    delay(35);
  }
}

void splashStatus(const String& text) {
  fullscreen = true;
  splashLast = text;
  M5Canvas* c = splashCanvas();
  if (!c) return;
  drawSplashFrame(*c, 44, text);
  c->pushSprite(0, 0);
}

void message(const char* title, const String& body, uint16_t color) {
  fullscreen = true;
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::DejaVu18);
  d.setTextDatum(top_left);
  d.setTextColor(color, TFT_BLACK);
  d.drawString(title, PAD, 4);
  d.setFont(&fonts::DejaVu12);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  std::vector<String> w;
  wrap(body, W - 2 * PAD, w);
  int y = 28;
  for (auto& s : w) {
    if (y + LINE_H > H) break;
    d.drawString(s, PAD, y);
    y += LINE_H;
  }
}

int page(const String& title, const String& body, int startLine, const String& footer) {
  fullscreen = true;
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.fillRect(0, 0, W, BAR, COL_BAR);
  d.setFont(&fonts::DejaVu12);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_YELLOW, COL_BAR);
  d.drawString(title, 4, BAR / 2);

  std::vector<String> w;
  wrap(body, W - 2 * PAD, w);
  d.setTextDatum(top_left);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  int y = BAR + 2;
  for (int i = startLine; i < (int)w.size() && i < startLine + PAGE_LINES; i++) {
    d.drawString(w[i], PAD, y);
    y += LINE_H;
  }
  d.setTextColor(COL_SYS, TFT_BLACK);
  d.drawString(footer, PAD, H - LINE_H);
  return (int)w.size();
}

void menu(const char* title, const char* const* items, int count, int selected) {
  fullscreen = true;
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.fillRect(0, 0, W, BAR, COL_BAR);
  d.setFont(&fonts::DejaVu12);
  d.setTextDatum(middle_left);
  d.setTextColor(TFT_YELLOW, COL_BAR);
  d.drawString(title, 4, BAR / 2);
  const int rowH = count > 4 ? 19 : 21;
  for (int i = 0; i < count; i++) {
    int y = BAR + 3 + i * rowH;
    bool sel = i == selected;
    if (sel) d.fillRoundRect(2, y, W - 4, rowH - 2, 4, d.color565(0, 95, 92));
    d.setTextColor(sel ? TFT_WHITE : COL_SYS, sel ? d.color565(0, 95, 92) : TFT_BLACK);
    d.drawString(items[i], 10, y + (rowH - 2) / 2);
  }
  d.setTextDatum(top_left);
  d.setTextColor(COL_SYS, TFT_BLACK);
  d.drawString("A: next   B: open", PAD, H - LINE_H);
}

void qr(const String& url, const String& caption) {
  fullscreen = true;
  auto& d = M5.Display;
  d.fillScreen(TFT_WHITE);
  d.qrcode(url, 2, 2, H - 4, 1);
  d.setFont(&fonts::DejaVu12);
  d.setTextColor(TFT_BLACK, TFT_WHITE);
  d.setTextDatum(top_left);
  std::vector<String> w;
  wrap(caption, W - H - 4, w);
  int y = 6;
  for (auto& s : w) {
    if (y + LINE_H > H) break;
    d.drawString(s, H + 2, y);
    y += LINE_H;
  }
}

void dim(bool on) { M5.Display.setBrightness(on ? 20 : 110); }

void off() {
  M5.Display.sleep();
  M5.Display.waitDisplay();
}

}  // namespace ui
