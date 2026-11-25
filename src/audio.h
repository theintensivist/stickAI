#pragma once
#include <Arduino.h>

// Microphone capture (16 kHz mono, 16-bit) and speaker playback (24 kHz mono,
// 16-bit) for the StickS3. The mic and speaker share the ES8311 codec pins, so
// only one of them runs at a time: starting a capture turns the speaker off
// and starting playback turns the mic off.
namespace audio {

constexpr uint32_t MIC_RATE = 16000;   // what Gemini Live expects as input
constexpr uint32_t SPK_RATE = 24000;   // what Gemini Live sends back
constexpr size_t CHUNK = 1600;         // 100 ms of mic audio per chunk

bool begin();                 // allocate buffers in PSRAM; call once
void setMicGain(uint8_t g);   // 1..9 mic boost (applied at the next recording)
void setMicChannel(uint8_t c);  // 0 right, 1 left, 2 both (applied at the next recording)
const int16_t* rawData();     // untouched copy of the first seconds of the last recording
size_t rawSamples();

// ---- capture ----
void captureStart(uint32_t maxMs);   // starts recording immediately
void captureStop();                  // finishes the chunk in progress, then stops
bool capturing();
size_t capturedSamples();            // samples fully recorded so far
const int16_t* captureData();
uint32_t capturedMs();
bool captureFull();                  // hit maxMs
int micLevelRecent();                // 0..100, loudness of the last 100 ms
int micLevelPeak();                  // 0..100, loudest raw moment of this recording (before level control)
void processCaptured(size_t upTo);   // apply automatic level control to samples [.., upTo) before sending
void playRecording(uint32_t maxMs);  // play back the last recording (to hear what the mic picked up)

// ---- playback ----
void playPush(const int16_t* pcm, size_t samples);  // queue reply audio (24 kHz unless setPlayRate)
void setPlayRate(uint32_t hz);                     // sample rate Gemini says it is sending
uint32_t underruns();                              // times playback ran dry and had to wait (this reply)
void playFinish();         // no more audio coming for this reply
void playService();        // call often from loop()
void playStop();           // stop now and drop queued audio
bool playActive();         // still has audio queued or playing
void setVolume(uint8_t v);
void beep(uint16_t hz, uint32_t ms);   // short tone (alarms)

}  // namespace audio
