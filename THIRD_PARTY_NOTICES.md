# Third-party notices

stickAI's own code is released under the [MIT License](LICENSE). It is built on the open-source components below, which keep their own licenses.

| Component | Used for | License |
|---|---|---|
| [arduinoWebSockets](https://github.com/Links2004/arduinoWebSockets) by Markus Sattler, commit `1c77130` (included in `lib/arduinoWebSockets`) | Secure WebSocket connection to Gemini | LGPL-2.1, see `lib/arduinoWebSockets/LICENSE`. One small change, described in `lib/arduinoWebSockets/STICKS3_PATCH.txt`: the server's close reason is passed to the app. |
| libb64 and libsha1 (inside arduinoWebSockets) | Base64 and SHA-1 for the WebSocket handshake | Public domain |
| [M5Unified](https://github.com/m5stack/M5Unified) and [M5GFX](https://github.com/m5stack/M5GFX) 0.3.0 | Screen, buttons, power, microphone and speaker | MIT |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) 7 | Reading and writing Gemini's JSON messages | MIT |
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) 2.0.17 and [ESP-IDF](https://github.com/espressif/esp-idf) | Wi-Fi, TLS, file system, web server | LGPL-2.1 (Arduino core), Apache-2.0 (ESP-IDF) |
| [DejaVu Sans](https://dejavu-fonts.github.io/) | The smooth English and Arabic screen font in `src/font_ar.h`, converted with `tools/make_vlw.py` | Bitstream Vera license (DejaVu changes are public domain), see `licenses/DejaVu-Fonts-LICENSE.txt` |
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) | The one-click web installer page (loaded from unpkg when you open it) | Apache-2.0 |
| Root certificates in `src/certs.h` (Google Trust Services, GlobalSign, ISRG / Let's Encrypt) | Checking that the stick is really talking to Google | Public certificates |

Because the firmware binaries include LGPL-2.1 code, the complete source needed to rebuild them is in this repository, and you can rebuild them with PlatformIO (see the README).

"Gemini" is a trademark of Google LLC and "M5Stack" and "StickS3" are trademarks of M5Stack Technology Co., Ltd. stickAI is an independent project and is not affiliated with or endorsed by either company.
