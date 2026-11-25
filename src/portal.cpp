#include "portal.h"
#include "settings.h"
#include "ui.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_random.h>

static WebServer server(80);
static DNSServer dns;
static String scanned;          // <option> list of nearby networks
static bool saved = false;

static String esc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static String field(const char* label, const char* name, const String& value, const char* type = "text",
                    const char* extra = "", const char* hint = nullptr) {
  String h = "<label>" + String(label) + "<input type=\"" + type + "\" name=\"" + name + "\" value=\"" + esc(value) +
             "\" " + extra + "></label>";
  if (hint) h += "<small>" + String(hint) + "</small>";
  return h;
}

static String voiceOption(const char* v) {
  String sel = cfg.voice == v ? " selected" : "";
  String label = strlen(v) ? v : "Default";
  return "<option value=\"" + String(v) + "\"" + sel + ">" + label + "</option>";
}

// Settings page HTML. Used by the setup Wi-Fi and by the phone page at home.
String settingsPageHtml(bool fromHome) {
  String h;
  h.reserve(9000);
  h += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
         "<title>stickAI setup</title><style>"
         "body{font-family:system-ui,sans-serif;max-width:520px;margin:auto;padding:12px;background:#f6f6f4;color:#222}"
         "h1{font-size:20px}h2{font-size:16px;margin:22px 0 6px;border-bottom:1px solid #ccc}"
         "label{display:block;margin:10px 0 2px;font-weight:600}input,select,textarea{width:100%;box-sizing:border-box;"
         "padding:9px;font-size:16px;border:1px solid #bbb;border-radius:8px;margin-top:4px;background:#fff}"
         "small{color:#666;display:block;margin-top:3px}button{width:100%;padding:13px;font-size:17px;margin-top:22px;"
         "border:0;border-radius:10px;background:#1a73e8;color:#fff}.ok{color:#188038}</style></head><body>"
         "<h1>stickAI <small style=\"display:inline;font-weight:400\">by Dr Abhijeet Anand</small></h1>"
         "<form method=\"POST\" action=\"/save\">");
  if (fromHome) {
    h += F("<p><a href=\"/\">&larr; Timers</a> &nbsp; <a href=\"/mic.wav\">Last recording</a></p>"
           "<p><small>Saving restarts the stick.</small></p>");
  }
  h += "<datalist id=\"nets\">" + scanned + "</datalist>";

  h += F("<h2>Wi-Fi</h2>");
  h += field("Main Wi-Fi name", "ssid1", cfg.ssid1, "text", "list=\"nets\" autocomplete=\"off\"");
  h += field("Main Wi-Fi password", "pass1", "", "password", "autocomplete=\"new-password\"",
             cfg.pass1.length() ? "Saved. Leave empty to keep it." : nullptr);
  h += field("Backup Wi-Fi name (optional)", "ssid2", cfg.ssid2, "text", "list=\"nets\" autocomplete=\"off\"",
             "For example your phone's hotspot, for use outside home.");
  h += field("Backup Wi-Fi password", "pass2", "", "password", "autocomplete=\"new-password\"",
             cfg.pass2.length() ? "Saved. Leave empty to keep it." : nullptr);

  h += F("<h2>Gemini</h2>");
  h += field("Gemini API key", "key", "", "password", "autocomplete=\"off\"",
             cfg.apiKey.length() ? "<span class=ok>A key is saved.</span> Leave empty to keep it."
                                 : "Free from aistudio.google.com &rarr; Get API key.");
  h += field("Model", "model", cfg.model, "text", "autocomplete=\"off\"",
             "Default gemini-3.8-live. Change only if Google renames it.");
  h += F("<label>Voice<select name=\"voice\">");
  for (const char* v : {"", "Kore", "Puck", "Charon", "Aoede", "Fenrir", "Leda", "Orus", "Zephyr"}) h += voiceOption(v);
  if (cfg.voice.length() && String("|Kore|Puck|Charon|Aoede|Fenrir|Leda|Orus|Zephyr|").indexOf("|" + cfg.voice + "|") < 0)
    h += voiceOption(cfg.voice.c_str());
  h += F("</select></label><small>If replies fail after choosing a voice, set it back to Default.</small>");
  h += field("Language code (optional)", "lang", cfg.language, "text", "placeholder=\"auto\"",
             "Leave empty to reply in whatever language you speak. Captions show English text best.");
  h += "<label>About you (optional)<textarea name=\"about\" rows=\"3\" maxlength=\"300\">" + esc(cfg.about) +
       "</textarea></label><small>A short note the assistant should know, e.g. your name and city. "
       "Do not put patient or other sensitive details here.</small>";

  h += F("<h2>Device</h2>");
  h += field("Speaker volume (0-190)", "vol", String(cfg.volume), "number", "min=\"0\" max=\"190\"");
  h += field("Mic boost (1-9)", "micg", String(cfg.micGain), "number", "min=\"1\" max=\"9\"",
             "Default 9 (the StickS3 mic is quiet). Use Menu &rarr; Mic test on the stick to check: "
             "lower it only if your voice sounds distorted.");
  h += "<label>Mic channel<select name=\"mch\">";
  {
    const char* names[3] = {"Right (default)", "Left", "Both"};
    for (int i = 0; i < 3; i++)
      h += "<option value=\"" + String(i) + "\"" + String(cfg.micChan == i ? " selected" : "") + ">" + names[i] + "</option>";
  }
  h += F("</select></label><small>Only change this if Mic lab shows your voice is on another channel.</small>");
  h += field("Sleep after idle (minutes)", "sleep", String(cfg.sleepMin), "number", "min=\"0\" max=\"120\"",
             "0 = never. Press button A to wake.");
  h += field("Time zone (hours from UTC)", "tz", String(cfg.tzHours), "number", "min=\"-12\" max=\"14\"", "UAE = 4.");
  h += "<label>Screen<select name=\"rot\"><option value=\"1\"" + String(cfg.rotation == 1 ? " selected" : "") +
       ">Normal</option><option value=\"3\"" + String(cfg.rotation == 3 ? " selected" : "") +
       ">Upside down</option></select></label>";
  h += F("<label><input type=\"checkbox\" name=\"erase\" style=\"width:auto\"> Erase all settings (factory reset)</label>"
         "<button type=\"submit\">Save and restart</button></form>"
         "<p style=\"text-align:center;color:#888;margin-top:24px\">stickAI by Dr Abhijeet Anand</p></body></html>");
  return h;
}

static void handleRoot() { server.send(200, "text/html", settingsPageHtml(false)); }

// Reads the submitted settings form, saves it, and returns the reply page.
String saveSettingsFrom(WebServer& srv) {
  if (srv.hasArg("erase")) {
    settingsErase();
    return "<meta charset=utf-8><h2>Erased. The stick is restarting.</h2>";
  }
  auto arg = [&srv](const char* n) { String v = srv.arg(n); v.trim(); return v; };
  cfg.ssid1 = arg("ssid1");
  cfg.ssid2 = arg("ssid2");
  if (srv.arg("pass1").length()) cfg.pass1 = srv.arg("pass1");
  if (srv.arg("pass2").length()) cfg.pass2 = srv.arg("pass2");
  if (cfg.ssid2.isEmpty()) cfg.pass2 = "";
  if (arg("key").length()) cfg.apiKey = arg("key");
  cfg.model = arg("model");
  if (cfg.model.startsWith("models/")) cfg.model = cfg.model.substring(7);
  cfg.voice = arg("voice");
  cfg.language = arg("lang");
  cfg.about = arg("about").substring(0, 300);
  cfg.volume = constrain(arg("vol").toInt(), 0, 190);
  cfg.sleepMin = constrain(arg("sleep").toInt(), 0, 120);
  cfg.tzHours = constrain(arg("tz").toInt(), -12, 14);
  cfg.rotation = arg("rot") == "3" ? 3 : 1;
  cfg.micGain = constrain(arg("micg").toInt(), 1, 9);
  cfg.micChan = constrain(arg("mch").toInt(), 0, 2);
  settingsSave();
  settingsLoad();  // apply defaults for anything left empty
  String page = "<meta charset=utf-8><meta name=viewport content='width=device-width'>"
                "<h2>Saved. The stick is restarting.</h2>";
  page += "<p>Hold A to talk to the assistant. Hold B for the English &harr; Arabic interpreter.</p>";
  return page;
}

static void handleSave() {
  server.send(200, "text/html", saveSettingsFrom(server));
  saved = true;
}

static void redirectHome() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void runSetupPortal() {
  ui::message("Setup", "Scanning for Wi-Fi networks...", TFT_YELLOW);
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 20; i++) scanned += "<option value=\"" + esc(WiFi.SSID(i)) + "\">";

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char apName[32];
  snprintf(apName, sizeof(apName), "stickAI-Setup-%02X%02X", mac[4], mac[5]);
  char apPass[12];
  snprintf(apPass, sizeof(apPass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
  WiFi.softAP(apName, apPass);
  delay(200);
  dns.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/generate_204", HTTP_GET, redirectHome);         // Android captive check
  server.on("/hotspot-detect.html", HTTP_GET, redirectHome);  // iPhone captive check
  server.onNotFound(redirectHome);
  server.begin();

  String join = String("WIFI:T:WPA;S:") + apName + ";P:" + apPass + ";;";
  ui::qr(join, String("1. Scan to join\n") + apName + "\nPassword " + apPass +
                   "\n2. Open 192.168.4.1\nB: cancel");

  uint32_t t0 = millis();
  while (true) {
    dns.processNextRequest();
    server.handleClient();
    M5.update();
    if (saved) {
      delay(1500);
      ESP.restart();
    }
    if (M5.BtnB.wasPressed() && cfg.configured()) ESP.restart();
    if (millis() - t0 > 15UL * 60UL * 1000UL) ESP.restart();  // give up after 15 minutes
    delay(2);
  }
}
