#include "web.h"
#include "settings.h"
#include "timers.h"
#include "audio.h"
#include "portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

namespace web {

static WebServer server(80);
static bool started = false;
static uint32_t restartAt = 0;

static String esc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

// Every page needs the PIN shown on the stick (username: stickai).
static bool authed() {
  if (server.authenticate("stickai", cfg.webPin.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "stickAI", "Enter the PIN shown on the stick");
  return false;
}

static const char* STYLE =
    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<style>body{font-family:system-ui,sans-serif;max-width:560px;margin:auto;padding:12px;background:#f6f6f4;color:#222}"
    "h1{font-size:22px;margin:6px 0}h1 b{color:#0a9e96}nav a{margin-right:14px}"
    ".n{background:#fff;border:1px solid #ddd;border-radius:10px;padding:10px 12px;margin:10px 0}"
    ".t{color:#777;font-size:13px;margin-bottom:4px}.n form{display:inline}"
    "button{border:0;background:#eee;border-radius:8px;padding:6px 12px;font-size:14px}"
    ".f{text-align:center;color:#999;font-size:13px;margin-top:26px}</style>";

static void handleHome() {
  if (!authed()) return;
  String h;
  h.reserve(3000);
  h += "<!DOCTYPE html><html><head><title>stickAI</title><meta http-equiv=\"refresh\" content=\"30\">";
  h += STYLE;
  h += "</head><body><h1>stick<b>AI</b></h1><nav><a href=\"/settings\">Settings</a>"
       "<a href=\"/mic.wav\">Last recording</a></nav><h3>Timers and reminders</h3>";
  size_t n = timers::count();
  if (n == 0) h += "<p>Nothing set. Hold A on the stick and say, for example, \"Set a timer for 10 minutes\".</p>";
  for (size_t i = 0; i < n; i++) {
    timers::Item it = timers::get(i);
    String when = it.reminder ? "at " + timers::dueClock(it) : timers::remainingText(it) + " left (" + timers::dueClock(it) + ")";
    h += "<div class=\"n\"><div class=\"t\">" + String(it.reminder ? "Reminder" : "Timer") + " &middot; " + esc(when) +
         "</div>" + esc(it.label) +
         "<div style=\"text-align:right;margin-top:6px\"><form method=\"POST\" action=\"/cancel\">"
         "<input type=\"hidden\" name=\"id\" value=\"" + String(it.id) + "\"><button>Cancel</button></form></div></div>";
  }
  h += "<p class=\"f\">stickAI by Dr Abhijeet Anand</p></body></html>";
  server.send(200, "text/html", h);
}

static void handleCancel() {
  if (!authed()) return;
  long id = server.arg("id").toInt();
  if (id > 0) timers::cancel((uint32_t)id);
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

static void handleSettings() {
  if (!authed()) return;
  server.send(200, "text/html", settingsPageHtml(true));
}

static void handleSave() {
  if (!authed()) return;
  server.send(200, "text/html", saveSettingsFrom(server));
  restartAt = millis() + 1500;   // restart shortly, after the reply has been sent
}

// Last recording as a WAV file (16 kHz mono) - to hear what the mic picked up.
static void handleWav() {
  if (!authed()) return;
  size_t samples = audio::capturedSamples();
  const int16_t* data = audio::captureData();
  if (!data || samples == 0) {
    server.send(404, "text/plain", "No recording yet. Hold A or B on the stick and speak first.");
    return;
  }
  uint32_t bytes = samples * 2;
  uint8_t hdr[44];
  auto w32 = [&](int o, uint32_t v) { hdr[o] = v; hdr[o + 1] = v >> 8; hdr[o + 2] = v >> 16; hdr[o + 3] = v >> 24; };
  auto w16 = [&](int o, uint16_t v) { hdr[o] = v; hdr[o + 1] = v >> 8; };
  memcpy(hdr, "RIFF", 4); w32(4, 36 + bytes); memcpy(hdr + 8, "WAVEfmt ", 8);
  w32(16, 16); w16(20, 1); w16(22, 1); w32(24, audio::MIC_RATE); w32(28, audio::MIC_RATE * 2);
  w16(32, 2); w16(34, 16); memcpy(hdr + 36, "data", 4); w32(40, bytes);
  server.sendHeader("Content-Disposition", "attachment; filename=\"stickai-recording.wav\"");
  server.setContentLength(44 + bytes);
  server.send(200, "audio/wav", "");
  WiFiClient client = server.client();
  client.write(hdr, 44);
  const uint8_t* p = (const uint8_t*)data;
  size_t left = bytes;
  while (left > 0 && client.connected()) {
    size_t n = left > 4096 ? 4096 : left;
    size_t sent = client.write(p, n);
    if (sent == 0) break;
    p += sent;
    left -= sent;
  }
}

void begin() {
  if (started) return;
  MDNS.begin("stickai");
  MDNS.addService("http", "tcp", 80);
  server.on("/", HTTP_GET, handleHome);
  server.on("/cancel", HTTP_POST, handleCancel);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/mic.wav", HTTP_GET, handleWav);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  started = true;
}

void loop() {
  if (!started) return;
  server.handleClient();
  if (restartAt && (int32_t)(millis() - restartAt) > 0) ESP.restart();
}

String address() { return "http://" + WiFi.localIP().toString() + "/"; }

}  // namespace web
