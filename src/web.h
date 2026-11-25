#pragma once
#include <Arduino.h>

// Phone page on your home Wi-Fi: see and cancel timers, change settings, and
// download the last recording. Open http://stickai.local (or the address the
// stick shows) from a phone on the same Wi-Fi. Protected by a 6-digit PIN.
namespace web {

void begin();      // call once Wi-Fi is connected
void loop();       // call every loop()
String address();  // e.g. "http://192.168.1.23/"

}  // namespace web
