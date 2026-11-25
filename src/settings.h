#pragma once
#include <Arduino.h>

// Everything the user can change on the setup page. Stored in the stick's
// non-volatile storage (NVS) under the "assist" namespace.
struct Settings {
  String ssid1, pass1;   // main Wi-Fi (e.g. home)
  String ssid2, pass2;   // backup Wi-Fi (e.g. phone hotspot)
  String apiKey;         // Gemini API key from Google AI Studio
  String model;          // Live API model id, e.g. "gemini-3.8-live"
  String voice;          // optional prebuilt voice name; empty = model default
  String language;       // optional language code, e.g. "en-IN"; empty = auto
  String about;          // optional short note about the user, added to the instructions
  String ntfyServer;     // e.g. "https://ntfy.sh"
  String ntfyTopic;      // private, hard-to-guess topic name
  uint8_t volume = 150;      // 0..190 (kept below ~75% for battery stability)
  uint16_t longReply = 300;  // replies longer than this (chars) are also sent to the phone; 0 = off
  uint8_t sleepMin = 5;      // minutes idle before deep sleep; 0 = never
  uint8_t rotation = 1;      // screen rotation: 1 or 3 (flipped)
  int8_t tzHours = 4;        // local time offset from UTC in hours (UAE = +4)
  uint8_t micGain = 9;       // mic boost level 1..9 = 0..32 dB in the ES8311 audio chip (9 = M5Unified default; the mic is quiet)
  String webPin;             // 6-digit PIN for the phone web page
  uint8_t micChan = 0;       // which I2S slot carries the mic: 0 right (M5Unified default), 1 left, 2 both
  bool phone = false;        // optional ntfy phone notifications (not shown on the settings page); off = standalone

  bool configured() const { return ssid1.length() > 0 && apiKey.length() > 0; }
};

extern Settings cfg;

void settingsLoad();
void settingsSave();
void settingsErase();
String randomTopic();
