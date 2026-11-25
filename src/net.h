#pragma once
#include <Arduino.h>

// Wi-Fi, clock, phone notifications (ntfy) and a plain-language check of the
// Gemini key/model when a connection fails.
namespace net {

bool connectWifi(uint32_t perNetworkMs = 10000);   // tries Wi-Fi 1, then Wi-Fi 2
bool online();
void syncClock();                                    // NTP; best effort
String localTimeText();                              // e.g. "Tuesday 24 November 2026, 20:49"

// Sends a notification to the user's private ntfy topic. Returns true on success.
bool notify(const String& title, const String& body, const char* tags = nullptr);

// Asks Google whether the key and model are usable. Returns "" if fine,
// otherwise a short human-readable reason.
String diagnoseGemini();

}  // namespace net
