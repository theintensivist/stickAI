#pragma once
#include <Arduino.h>

// Screen layout (240 x 135, landscape):
//   top 18 px : status bar (state, Wi-Fi, battery)
//   rest      : conversation captions, newest at the bottom
namespace ui {

enum class Role : uint8_t { User, Assistant, System };

void begin(uint8_t rotation);
void status(const char* text, uint16_t color);   // left side of the status bar
void setClockText(const String& t);              // replaces the clock (e.g. timer countdown); "" = clock
void clearChat();
int addLine(Role role, const String& text);      // new paragraph; returns its id
void setText(int id, const String& text);        // replace a paragraph's text (streaming captions)
void scrollUp();                                 // one page back in history
void scrollToEnd();
void render();                                   // redraw if anything changed (call from loop)
void forceRender();

// Animated start-up screen: "stickAI by Dr Abhijeet Anand" with the logo
// (a chip whose heartbeat line turns into a voice wave). ~1.6 s.
void splash();
void splashStatus(const String& text);   // small line under the logo, e.g. "Connecting to Wi-Fi..."

void message(const char* title, const String& body, uint16_t color = 0xFFFF);
// Simple list menu: highlighted row = selected. Footer shows the controls.
void menu(const char* title, const char* const* items, int count, int selected);
// Full-screen page of text starting at a given wrapped line. Returns the total
// number of wrapped lines, so the caller can page through long text.
int page(const String& title, const String& body, int startLine, const String& footer);
constexpr int PAGE_LINES = 6;
void qr(const String& url, const String& caption);
void dim(bool on);
void off();

}  // namespace ui
