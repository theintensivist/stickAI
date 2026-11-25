#pragma once
#include <Arduino.h>
class WebServer;

// Setup page. The stick creates its own password-protected Wi-Fi network
// ("stickAI-Setup-XXXX"); join it from your phone and a settings page opens.
// Saving restarts the stick. This never returns.
void runSetupPortal();

// Shared with the phone web page (web.cpp)
String settingsPageHtml(bool fromHome);
String saveSettingsFrom(WebServer& srv);
