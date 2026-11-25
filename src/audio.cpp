#include "audio.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>

namespace audio {

// ---------------- capture ----------------
// One big buffer (60 s max) filled chunk by chunk. M5Unified's mic runs in its
// own task; when a chunk is full it calls micReleased(), which immediately
// queues the next chunk. So recording continues smoothly even while the main
// loop is busy (for example during the TLS handshake with Google).
static constexpr uint32_t CAP_MAX_MS = 60000;
static constexpr size_t CAP_MAX_CHUNKS = (MIC_RATE * CAP_MAX_MS / 1000) / CHUNK;

static int16_t* capBuf = nullptr;
static volatile size_t capDone = 0;   // chunks completed
static volatile size_t capNext = 0;   // next chunk index to queue
static volatile size_t capLimit = 0;  // max chunks for this recording
static volatile bool capOn = false;
static bool capActive = false;
static uint8_t micGain = 9;   // 1..9 -> 0..32 dB ES8311 ADC volume
static constexpr size_t SKIP_CHUNKS = 2;  // first 200 ms: the mic power-up click

// automatic level control (applied to recorded chunks before they are sent)
static size_t procUpTo = 0;
static float agcGain = 1.0f;
static int rawPeak = 0;

static volatile int lastChunkPeak = 0;   // raw loudness of the newest chunk, for the meter
static constexpr size_t RAW_CHUNKS = 80; // 8 s untouched copy, for diagnostics
static int16_t* rawBuf = nullptr;
static uint8_t micChan = 0;

static void micReleased(void*, void* data, size_t) {
  size_t idx = ((int16_t*)data - capBuf) / CHUNK;
  {
    const int16_t* p = (const int16_t*)data;
    int pk = 0;
    for (size_t i = 0; i < CHUNK; i++) { int v = p[i] < 0 ? -p[i] : p[i]; if (v > pk) pk = v; }
    lastChunkPeak = pk;
    if (rawBuf && idx < RAW_CHUNKS) memcpy(rawBuf + idx * CHUNK, p, CHUNK * sizeof(int16_t));
  }
  if (idx + 1 > capDone) capDone = idx + 1;
  if (capOn && capNext < capLimit) {
    // inside the callback: use the current-rate overload (never waits)
    if (M5.Mic.record(capBuf + capNext * CHUNK, CHUNK)) capNext = capNext + 1;
  }
}

// ---------------- playback ----------------
// Incoming audio goes into a large ring buffer (FIFO). playService() moves it
// to the speaker in 200 ms blocks using three rotating buffers, as the
// M5Unified docs recommend for audio generated at run time.
static constexpr size_t FIFO_SAMPLES = SPK_RATE * 45;   // 45 s
static constexpr size_t BLOCK = 4800;                   // 200 ms
static constexpr size_t PREBUFFER = 7200;               // 300 ms jitter cushion
static int16_t* fifo = nullptr;
static size_t fHead = 0, fTail = 0, fCount = 0;
static int16_t* blocks[3] = {nullptr, nullptr, nullptr};
static int blockIdx = 0;
static bool started = false;   // playback of the current reply has begun
static bool finished = false;  // server said the reply is complete
static bool spkOn = false;
static uint8_t volume = 150;
static uint32_t playRate = SPK_RATE;
static uint32_t dryCount = 0;
static bool dry = false;

static void* psram(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(bytes);
}

bool begin() {
  capBuf = (int16_t*)psram(CAP_MAX_CHUNKS * CHUNK * sizeof(int16_t));
  fifo = (int16_t*)psram(FIFO_SAMPLES * sizeof(int16_t));
  for (auto& b : blocks) b = (int16_t*)psram(BLOCK * sizeof(int16_t));
  rawBuf = (int16_t*)psram(RAW_CHUNKS * CHUNK * sizeof(int16_t));
  return capBuf && fifo && blocks[0] && blocks[1] && blocks[2];
}

static void speakerOff() {
  // always end: the mic and speaker share the codec pins
  M5.Speaker.stop();
  M5.Speaker.end();
  spkOn = false;
}

static void speakerOn() {
  if (!spkOn) {
    M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume);
    spkOn = true;
  }
}

void setMicGain(uint8_t g) { micGain = g < 1 ? 1 : (g > 9 ? 9 : g); }
void setMicChannel(uint8_t c) { micChan = c > 2 ? 0 : c; }
const int16_t* rawData() { return rawBuf; }
size_t rawSamples() { size_t d = capDone; return (d < RAW_CHUNKS ? d : RAW_CHUNKS) * CHUNK; }

// ES8311 register 0x17 = ADC digital volume: 0xBF is 0 dB, each step +0.5 dB,
// 0xFF = +32 dB (what M5Unified sets for the StickS3, which clips normal speech).
static void applyMicBoost() {
  int db = (micGain - 1) * 4;               // 0, 4, 8 ... 32 dB
  uint8_t reg = (uint8_t)(0xBF + db * 2);
  for (int i = 0; i < 3 && !M5.In_I2C.writeRegister8(0x18, 0x17, reg, 100000); i++) delay(1);
}

void captureStart(uint32_t maxMs) {
  playStop();
  speakerOff();
  M5.Mic.end();
  {
    // M5Unified leaves the StickS3 at magnification 16 (the generic default) on top of
    // the ES8311's maximum ADC volume, which clips speech. Other ES8311 boards use 2.
    auto mc = M5.Mic.config();
    mc.magnification = 2;   // unity: the boost is set in the audio chip instead (applyMicBoost)
    mc.sample_rate = MIC_RATE;  // same rate as record() uses, so it never restarts (which would reset the boost)
    mc.input_channel = (m5::input_channel_t)micChan;
    M5.Mic.config(mc);
  }
  procUpTo = 0;
  agcGain = 4.0f;
  rawPeak = 0;
  if (maxMs > CAP_MAX_MS) maxMs = CAP_MAX_MS;
  capLimit = (MIC_RATE * maxMs / 1000) / CHUNK;
  capDone = 0;
  capNext = 0;
  M5.Mic.setBufferReleaseCallback(nullptr, micReleased);  // only allowed while the mic is stopped
  M5.Mic.begin();
  applyMicBoost();   // after begin(): M5Unified's mic power-up sets the chip to +32 dB
  capOn = true;
  capActive = true;
  // queue two chunks; from then on the release callback keeps the queue full
  for (int i = 0; i < 2 && capNext < capLimit; i++) {
    if (M5.Mic.record(capBuf + capNext * CHUNK, CHUNK, MIC_RATE)) capNext = capNext + 1;
  }
}

void captureStop() {
  if (!capActive) return;
  capOn = false;
  uint32_t t0 = millis();
  while (M5.Mic.isRecording() && millis() - t0 < 400) delay(5);  // let queued chunks finish (<=200 ms)
  M5.Mic.end();
  M5.Mic.setBufferReleaseCallback(nullptr, nullptr);
  capActive = false;
}

bool capturing() { return capActive && capOn; }
size_t capturedSamples() { return capDone * CHUNK; }
const int16_t* captureData() { return capBuf; }
uint32_t capturedMs() { return capDone * CHUNK * 1000 / MIC_RATE; }
bool captureFull() { return capActive && capDone >= capLimit; }

int micLevelRecent() { return capDone > SKIP_CHUNKS ? lastChunkPeak * 100 / 32767 : 0; }

int micLevelPeak() { return rawPeak * 100 / 32767; }

// Level control, chunk by chunk: aim speech peaks at ~55% of full scale, cut the
// gain at once when it gets loud (no clipping), raise it slowly when quiet.
// The first 200 ms is silenced: the mic makes a loud click when it powers up.
void processCaptured(size_t upTo) {
  if (!capBuf) return;
  upTo = upTo / CHUNK * CHUNK;
  const float target = 0.55f * 32767.0f;
  while (procUpTo < upTo) {
    int16_t* p = capBuf + procUpTo;
    int peak = 0;
    for (size_t i = 0; i < CHUNK; i++) {
      int v = p[i] < 0 ? -p[i] : p[i];
      if (v > peak) peak = v;
    }
    bool click = procUpTo < SKIP_CHUNKS * CHUNK;
    if (!click && peak > rawPeak) rawPeak = peak;
    float want = peak > 300 ? target / peak : 8.0f;
    if (want > 8.0f) want = 8.0f;
    if (want < 0.1f) want = 0.1f;
    if (!click) {
      if (want < agcGain) agcGain = want;               // fast attack
      else agcGain += (want - agcGain) * 0.15f;        // slow release
    }
    for (size_t i = 0; i < CHUNK; i++) {
      float g = click ? 0.0f : agcGain;
      int32_t v = (int32_t)(p[i] * g);
      p[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
    }
    procUpTo += CHUNK;
  }
}

void playRecording(uint32_t maxMs) {
  size_t n = capDone * CHUNK;
  size_t lim = (size_t)MIC_RATE * maxMs / 1000;
  if (n > lim) n = lim;
  if (!capBuf || n == 0) return;
  playStop();
  speakerOn();
  M5.Speaker.playRaw(capBuf, n, MIC_RATE, false, 1, 1, true);  // channel 1, separate from replies
}

void playPush(const int16_t* pcm, size_t n) {
  if (!fifo || n == 0) return;
  for (size_t i = 0; i < n; i++) {
    if (fCount >= FIFO_SAMPLES) break;  // buffer full: drop the rest (very long replies only)
    fifo[fHead] = pcm[i];
    fHead = (fHead + 1) % FIFO_SAMPLES;
    fCount++;
  }
  finished = false;
}

void playFinish() { finished = true; }

void setPlayRate(uint32_t hz) { if (hz >= 8000 && hz <= 48000) playRate = hz; }
uint32_t underruns() { return dryCount; }

void playService() {
  if (!fifo) return;
  bool spkBusy = spkOn && M5.Speaker.isPlaying(0) > 0;
  if (fCount == 0) {
    if (finished && !spkBusy) started = false;
    return;
  }
  if (!started) {
    if (fCount < PREBUFFER && !finished) return;  // wait for a small cushion first
    speakerOn();
    started = true;
    dry = false;
  } else if (!spkBusy && !finished && fCount < PREBUFFER) {
    // The speaker ran out while more audio is still coming (slow network):
    // wait for a cushion again instead of playing tiny crackly fragments.
    if (!dry) { dry = true; dryCount++; }
    return;
  }
  dry = false;
  // Refill only while fewer than two blocks are queued, so the block we are
  // about to overwrite has already been played (three rotating buffers).
  // Only whole 200 ms blocks are queued until the reply is complete.
  while (fCount > 0 && M5.Speaker.isPlaying(0) < 2) {
    if (fCount < BLOCK && !finished) break;
    size_t n = fCount < BLOCK ? fCount : BLOCK;
    int16_t* b = blocks[blockIdx];
    for (size_t i = 0; i < n; i++) {
      b[i] = fifo[fTail];
      fTail = (fTail + 1) % FIFO_SAMPLES;
    }
    fCount -= n;
    M5.Speaker.playRaw(b, n, playRate, false, 1, 0, false);
    blockIdx = (blockIdx + 1) % 3;
  }
}

void playStop() {
  if (spkOn) M5.Speaker.stop(0);
  fHead = fTail = fCount = 0;
  started = false;
  finished = false;
  dry = false;
  dryCount = 0;
}

bool playActive() {
  return fCount > 0 || (spkOn && M5.Speaker.isPlaying(0) > 0);
}

void beep(uint16_t hz, uint32_t ms) {
  speakerOn();
  M5.Speaker.tone(hz, ms, 2, true);   // channel 2: separate from replies and playback
}

void setVolume(uint8_t v) {
  volume = v > 190 ? 190 : v;
  if (spkOn) M5.Speaker.setVolume(volume);
}

}  // namespace audio
