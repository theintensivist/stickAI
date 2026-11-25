#include "net.h"
#include "settings.h"
#include "certs.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

namespace net {

static bool tryNetwork(const String& ssid, const String& pass, uint32_t timeoutMs) {
  if (ssid.isEmpty()) return false;
  WiFi.disconnect(true, false);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

bool connectWifi(uint32_t perNetworkMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("stickai");
  WiFi.setAutoReconnect(true);
  if (tryNetwork(cfg.ssid1, cfg.pass1, perNetworkMs)) return true;
  if (tryNetwork(cfg.ssid2, cfg.pass2, perNetworkMs)) return true;
  return false;
}

bool online() { return WiFi.status() == WL_CONNECTED; }

void syncClock() {
  configTime((long)cfg.tzHours * 3600L, 0, "pool.ntp.org", "time.google.com");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 4000) delay(100);
}

String localTimeText() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  struct tm t;
  localtime_r(&now, &t);
  char buf[48];
  strftime(buf, sizeof(buf), "%A %d %B %Y, %H:%M", &t);
  return String(buf);
}

// HTTP headers must be plain ASCII on one line.
static String headerSafe(const String& s, size_t maxLen) {
  String out;
  for (size_t i = 0; i < s.length() && out.length() < maxLen; i++) {
    char c = s[i];
    if (c == '\r' || c == '\n') c = ' ';
    if ((uint8_t)c < 32 || (uint8_t)c > 126) continue;
    out += c;
  }
  return out;
}

bool notify(const String& title, const String& body, const char* tags) {
  if (!online() || cfg.ntfyTopic.isEmpty()) return false;
  String url = cfg.ntfyServer;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/" + cfg.ntfyTopic;

  HTTPClient http;
  WiFiClientSecure secure;
  WiFiClient plain;
  bool ok;
  if (url.startsWith("https://")) {
    secure.setCACert(ROOT_CA_BUNDLE);
    ok = http.begin(secure, url);
  } else {
    ok = http.begin(plain, url);
  }
  if (!ok) return false;
  http.setTimeout(10000);
  http.addHeader("Title", headerSafe(title, 120));
  if (tags) http.addHeader("Tags", tags);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  int code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

String diagnoseGemini() {
  if (!online()) return "No Wi-Fi connection.";
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + cfg.model + "?key=" + cfg.apiKey;
  WiFiClientSecure secure;
  secure.setCACert(ROOT_CA_BUNDLE);
  HTTPClient http;
  if (!http.begin(secure, url)) return "Could not reach Google.";
  http.setTimeout(10000);
  int code = http.GET();
  String body = code > 0 ? http.getString() : "";
  http.end();
  if (code == 200) return "";
  if (code <= 0) return "Could not reach Google (network or clock problem).";

  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc;
  String msg;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
    msg = doc["error"]["message"] | "";
  }
  if (msg.length() > 140) msg = msg.substring(0, 140) + "...";
  switch (code) {
    case 400: return "Google rejected the API key. " + msg;
    case 403: return "This key is not allowed to use Gemini. " + msg;
    case 404: return "Model \"" + cfg.model + "\" not found. Change it on the setup page. " + msg;
    case 429: return "Free daily limit reached. It resets around noon UAE time.";
    default:  return "Google error " + String(code) + ". " + msg;
  }
}

}  // namespace net
