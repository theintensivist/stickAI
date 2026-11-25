#pragma once
#include <Arduino.h>

// Diagnostic log over the USB cable (read with the stickai-tool script on a PC).
// Writes never wait: if nothing on the PC is reading, the text is dropped.
#define LOG(fmt, ...) Serial.printf("[%7lu] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

// Sends audio to the PC log as base64 lines between AUDIO_BEGIN / AUDIO_END.
void dumpAudio(const char* label, const int16_t* pcm, size_t samples, uint32_t rate);
