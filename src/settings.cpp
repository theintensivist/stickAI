#include "settings.h"
#include <Preferences.h>
#include <esp_random.h>

Settings cfg;

static const char* NS = "assist";
static const char* DEFAULT_MODEL = "gemini-3.8-live";
static const char* DEFAULT_NTFY = "https://ntfy.sh";

String randomTopic() {
  // 20 hex characters = 80 bits of randomness. On ntfy.sh the topic name is
  // the only thing that keeps the notes private, so it must be unguessable.
  static const char* hex = "0123456789abcdef";
  String t = "stick-";
  for (int i = 0; i < 20; i++) t += hex[esp_random() & 0x0F];
  return t;
}

void settingsLoad() {
  Preferences p;
  p.begin(NS, true);
  cfg.ssid1 = p.getString("ssid1", "");
  cfg.pass1 = p.getString("pass1", "");
  cfg.ssid2 = p.getString("ssid2", "");
  cfg.pass2 = p.getString("pass2", "");
  cfg.apiKey = p.getString("key", "");
  cfg.model = p.getString("model", DEFAULT_MODEL);
  cfg.voice = p.getString("voice", "");
  cfg.language = p.getString("lang", "");
  cfg.about = p.getString("about", "");
  cfg.ntfyServer = p.getString("ntfyS", DEFAULT_NTFY);
  cfg.ntfyTopic = p.getString("ntfyT", "");
  cfg.volume = p.getUChar("vol", 150);
  cfg.longReply = p.getUShort("long", 300);
  cfg.sleepMin = p.getUChar("sleep", 5);
  cfg.rotation = p.getUChar("rot", 1);
  cfg.tzHours = p.getChar("tz", 4);
  cfg.phone = p.getBool("phone", false);
  cfg.micGain = p.getUChar("micb2", 9);   // new key: earlier versions defaulted too low
  if (cfg.micGain < 1 || cfg.micGain > 9) cfg.micGain = 9;
  cfg.webPin = p.getString("pin", "");
  cfg.micChan = p.getUChar("mch", 0);
  if (cfg.micChan > 2) cfg.micChan = 0;
  p.end();

  if (cfg.model.isEmpty()) cfg.model = DEFAULT_MODEL;
  if (cfg.ntfyServer.isEmpty()) cfg.ntfyServer = DEFAULT_NTFY;
  if (cfg.volume > 190) cfg.volume = 190;
  if (cfg.rotation != 1 && cfg.rotation != 3) cfg.rotation = 1;
  if (cfg.tzHours < -12 || cfg.tzHours > 14) cfg.tzHours = 4;
  bool dirty = false;
  if (cfg.ntfyTopic.isEmpty()) { cfg.ntfyTopic = randomTopic(); dirty = true; }
  if (cfg.webPin.length() != 6) {
    char pin[8];
    snprintf(pin, sizeof(pin), "%06lu", (unsigned long)(esp_random() % 1000000UL));
    cfg.webPin = pin;
    dirty = true;
  }
  if (dirty) settingsSave();
}

void settingsSave() {
  Preferences p;
  p.begin(NS, false);
  p.putString("ssid1", cfg.ssid1);
  p.putString("pass1", cfg.pass1);
  p.putString("ssid2", cfg.ssid2);
  p.putString("pass2", cfg.pass2);
  p.putString("key", cfg.apiKey);
  p.putString("model", cfg.model);
  p.putString("voice", cfg.voice);
  p.putString("lang", cfg.language);
  p.putString("about", cfg.about);
  p.putString("ntfyS", cfg.ntfyServer);
  p.putString("ntfyT", cfg.ntfyTopic);
  p.putUChar("vol", cfg.volume);
  p.putUShort("long", cfg.longReply);
  p.putUChar("sleep", cfg.sleepMin);
  p.putUChar("rot", cfg.rotation);
  p.putChar("tz", cfg.tzHours);
  p.putBool("phone", cfg.phone);
  p.putUChar("micb2", cfg.micGain);
  p.putString("pin", cfg.webPin);
  p.putUChar("mch", cfg.micChan);
  p.end();
}

void settingsErase() {
  Preferences p;
  p.begin(NS, false);
  p.clear();
  p.end();
}
