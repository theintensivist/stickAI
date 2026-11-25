#include "dbg.h"
#include <mbedtls/base64.h>

void dumpAudio(const char* label, const int16_t* pcm, size_t samples, uint32_t rate) {
  if (!pcm || samples == 0) return;
  Serial.printf("AUDIO_BEGIN %s %lu %u\n", label, (unsigned long)rate, (unsigned)samples);
  const uint8_t* p = (const uint8_t*)pcm;
  size_t bytes = samples * 2;
  unsigned char line[80];
  for (size_t off = 0; off < bytes; off += 57) {   // 57 bytes -> 76 base64 chars
    size_t n = bytes - off < 57 ? bytes - off : 57;
    size_t olen = 0;
    mbedtls_base64_encode(line, sizeof(line) - 2, &olen, p + off, n);
    line[olen++] = '\n';
    Serial.write(line, olen);
  }
  Serial.printf("AUDIO_END %s\n", label);
}
