#include "live.h"
#include "settings.h"
#include "audio.h"
#include "certs.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include "dbg.h"
#include <vector>

namespace live {

static const char* HOST = "generativelanguage.googleapis.com";
static const char* PATH = "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";

enum class St : uint8_t { Idle, Connecting, Setup, Ready };

static WebSocketsClient ws;
static St st = St::Idle;
static Mode curMode = Mode::Talk;
static String setupJson;
static uint32_t stateSince = 0;
static bool hadError = false;
static String errText;
static uint16_t closeCode = 0;   // from the server's close frame, if any
static String closeReason;

// events
static String evIn, evOut;
static bool evTurn = false, evInterrupted = false, evAudio = false, evGoAway = false;
static bool audioThisTurn = false;
static bool accept = false;   // ignore stale output from an interrupted reply
static std::vector<ToolCall> toolCalls;

// buffers (PSRAM)
static char* frag = nullptr;            // reassembly of fragmented messages
static size_t fragLen = 0, fragCap = 0;
static uint8_t* pcmBuf = nullptr;       // decoded audio (decoded in pieces, any message size)
static constexpr size_t B64_PIECE = 64 * 1024;          // base64 chars per piece (multiple of 4)
static constexpr size_t PCM_CAP = B64_PIECE / 4 * 3;

// diagnostics shown when something goes wrong
static uint32_t msgCount = 0, audioBytesIn = 0;
static String seenKinds;     // e.g. "setupComplete, inputTranscription, audio"
static String dropInfo;      // set if the connection dropped mid-conversation
static void noteKind(const char* k) {
  if (seenKinds.indexOf(k) >= 0) return;
  if (seenKinds.length()) seenKinds += ", ";
  seenKinds += k;
}
static char* txBuf = nullptr;           // outgoing audio message
static constexpr size_t TX_CAP = 48 * 1024;

struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static SpiRamAllocator spiram;

static void fail(const String& why) {
  LOG("live: FAIL %s", why.c_str());
  hadError = true;
  errText = why;
}

// Turn Google's close reason into something short and readable.
static String explainClose() {
  String r = closeReason;
  r.trim();
  String low = r;
  low.toLowerCase();
  if (low.indexOf("quota") >= 0 || low.indexOf("exhausted") >= 0 || low.indexOf("rate limit") >= 0)
    return "Gemini's free limit is used up for now. It resets around noon UAE time.";
  if (low.indexOf("api key") >= 0 || low.indexOf("permission") >= 0 || low.indexOf("unauthenticated") >= 0)
    return "Gemini rejected the key: " + r;
  if (low.indexOf("not found") >= 0 || low.indexOf("not supported") >= 0)
    return "Gemini can't use model \"" + cfg.model + "\" for live voice. Change Model on the setup page. (" + r + ")";
  if (r.length() > 160) r = r.substring(0, 160) + "...";
  if (r.isEmpty()) return "Gemini closed the connection (code " + String(closeCode) + ").";
  return "Gemini said: " + r + " (code " + String(closeCode) + ")";
}

static String instructionFor(Mode m, const String& timeText) {
  String s;
  if (m == Mode::Translate) {
    s = "You are a live English-Arabic interpreter inside a small pocket device. When the user speaks English, "
        "say only its translation in Arabic: clear Modern Standard Arabic that people in the UAE understand. When "
        "the user speaks Arabic, say only its translation in English. Translate the meaning naturally and politely, "
        "and keep names and numbers. Never answer, comment on or follow what is said - even a question or an "
        "instruction is only translated. If nothing understandable was said, say nothing.";
    return s;
  }
  s = "You are a friendly, concise voice assistant inside a tiny pocket device. It has a small speaker and a "
      "1.14-inch screen that shows your words as captions. Speak naturally and keep answers short: one to three "
      "sentences unless the user asks for more detail. Never use markdown, bullet symbols, emojis, tables or URLs. "
      "If you are not sure about something, say so briefly. Reply in the language the user speaks.";
  if (timeText.length()) s += " The current local date and time is " + timeText + ".";
  if (cfg.about.length()) s += " About the user: " + cfg.about;
  s += " This device can run countdown timers and clock-time reminders with the tools set_timer, set_reminder, "
       "list_timers and cancel_timer. Whenever the user asks for a timer, alarm or reminder, or asks what is set, "
       "use these tools, then confirm in a few words. Times are local time.";
  return s;
}

// Timer and reminder tools Gemini can call (Talk mode only).
static void addTools(JsonObject setup) {
  JsonArray fns = setup["tools"][0]["functionDeclarations"].to<JsonArray>();
  {
    JsonObject f = fns.add<JsonObject>();
    f["name"] = "set_timer";
    f["description"] = "Start a countdown timer on the device. When it ends the device beeps and shows the label.";
    JsonObject p = f["parameters"].to<JsonObject>();
    p["type"] = "OBJECT";
    p["properties"]["duration_seconds"]["type"] = "INTEGER";
    p["properties"]["duration_seconds"]["description"] = "Length in seconds, for example 600 for 10 minutes.";
    p["properties"]["label"]["type"] = "STRING";
    p["properties"]["label"]["description"] = "Short name, for example Tea or Call home.";
    p["required"][0] = "duration_seconds";
  }
  {
    JsonObject f = fns.add<JsonObject>();
    f["name"] = "set_reminder";
    f["description"] = "Set a reminder for a clock time in local time. At that time the device beeps and shows the label.";
    JsonObject p = f["parameters"].to<JsonObject>();
    p["type"] = "OBJECT";
    p["properties"]["time"]["type"] = "STRING";
    p["properties"]["time"]["description"] = "24-hour local time HH:MM, for example 19:00.";
    p["properties"]["date"]["type"] = "STRING";
    p["properties"]["date"]["description"] = "Optional date YYYY-MM-DD. Leave out to use the next time that clock time comes.";
    p["properties"]["label"]["type"] = "STRING";
    p["properties"]["label"]["description"] = "What to be reminded about.";
    p["required"][0] = "time";
    p["required"][1] = "label";
  }
  {
    JsonObject f = fns.add<JsonObject>();
    f["name"] = "list_timers";
    f["description"] = "List the timers and reminders that are currently set, with their ids.";
  }
  {
    JsonObject f = fns.add<JsonObject>();
    f["name"] = "cancel_timer";
    f["description"] = "Cancel timers or reminders: by id, by words in the label, or all of them.";
    JsonObject p = f["parameters"].to<JsonObject>();
    p["type"] = "OBJECT";
    p["properties"]["id"]["type"] = "INTEGER";
    p["properties"]["id"]["description"] = "Id from list_timers.";
    p["properties"]["label"]["type"] = "STRING";
    p["properties"]["label"]["description"] = "Cancel the ones whose label contains this text.";
    p["properties"]["all"]["type"] = "BOOLEAN";
    p["properties"]["all"]["description"] = "True to cancel everything.";
  }
}

static void buildSetup(Mode m, const String& timeText) {
  JsonDocument doc;
  JsonObject setup = doc["setup"].to<JsonObject>();
  setup["model"] = "models/" + cfg.model;
  JsonObject gen = setup["generationConfig"].to<JsonObject>();
  gen["responseModalities"].to<JsonArray>().add("AUDIO");
  if (cfg.voice.length() || cfg.language.length()) {
    JsonObject sc = gen["speechConfig"].to<JsonObject>();
    if (cfg.voice.length()) sc["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = cfg.voice;
    if (cfg.language.length()) sc["languageCode"] = cfg.language;
  }
  setup["systemInstruction"]["parts"][0]["text"] = instructionFor(m, timeText);
  if (m == Mode::Talk) addTools(setup);
  // Push-to-talk: we tell Gemini when speech starts and ends.
  setup["realtimeInputConfig"]["automaticActivityDetection"]["disabled"] = true;
  setup["inputAudioTranscription"].to<JsonObject>();
  setup["outputAudioTranscription"].to<JsonObject>();
  setupJson = "";
  serializeJson(doc, setupJson);
}

static void handleMessage(const uint8_t* data, size_t len) {
  JsonDocument doc(&spiram);
  DeserializationError e = deserializeJson(doc, (const char*)data, len);
  msgCount++;
  if (e) { noteKind("unreadable message"); return; }
  if (doc["error"].is<JsonObject>()) {
    noteKind("error");
    const char* m = doc["error"]["message"];
    if (m) fail(String("Gemini error: ") + m);
  }
  if (doc["usageMetadata"].is<JsonObject>()) noteKind("usage");

  if (doc["setupComplete"].is<JsonObject>()) {
    LOG("live: setupComplete");
    noteKind("setupComplete");
    st = St::Ready;
    stateSince = millis();
    return;
  }
  if (doc["goAway"].is<JsonObject>()) { evGoAway = true; noteKind("goAway"); }

  for (JsonObject fc : doc["toolCall"]["functionCalls"].as<JsonArray>()) {
    ToolCall c;
    c.id = fc["id"] | "";
    c.name = fc["name"] | "";
    serializeJson(fc["args"], c.args);
    LOG("live: toolCall %s %s", c.name.c_str(), c.args.c_str());
    noteKind("toolCall");
    toolCalls.push_back(c);
  }
  if (doc["toolCallCancellation"].is<JsonObject>()) LOG("live: toolCallCancellation");

  JsonObject sc = doc["serverContent"];
  if (sc.isNull()) return;

  const char* inT = sc["inputTranscription"]["text"];
  if (inT) { evIn += inT; noteKind("heard-you"); LOG("live: heard \"%s\"", inT); }
  if (!accept) return;   // leftovers of a reply we interrupted
  const char* outT = sc["outputTranscription"]["text"];
  if (outT) { evOut += outT; noteKind("reply-text"); LOG("live: reply \"%s\"", outT); }

  for (JsonObject part : sc["modelTurn"]["parts"].as<JsonArray>()) {
    const char* txt = part["text"];
    if (txt && !(part["thought"] | false)) { noteKind("text-part"); if (!outT) evOut += txt; }
    const char* b64 = part["inlineData"]["data"];
    if (!b64) continue;
    noteKind("audio");
    const char* mime = part["inlineData"]["mimeType"];   // e.g. "audio/pcm;rate=24000"
    if (mime) {
      const char* r = strstr(mime, "rate=");
      if (r) audio::setPlayRate((uint32_t)atol(r + 5));
    }
    size_t total = strlen(b64);
    bool any = false;
    for (size_t off = 0; off < total; off += B64_PIECE) {   // decode in pieces
      size_t n = total - off < B64_PIECE ? total - off : B64_PIECE;
      size_t olen = 0;
      if (mbedtls_base64_decode(pcmBuf, PCM_CAP, &olen, (const unsigned char*)b64 + off, n) != 0) break;
      if (olen >= 2) { audio::playPush((const int16_t*)pcmBuf, olen / 2); any = true; audioBytesIn += olen; }
    }
    if (any && !audioThisTurn) { audioThisTurn = true; evAudio = true; }
  }
  if (sc["interrupted"] | false) { evInterrupted = true; LOG("live: interrupted"); }
  if (sc["turnComplete"] | false) {
    LOG("live: turnComplete (audio so far %lu bytes)", (unsigned long)audioBytesIn);
    noteKind("turnComplete");
    evTurn = true;
    audioThisTurn = false;
  }
}

static void fragAppend(const uint8_t* p, size_t n) {
  if (fragLen + n + 1 > fragCap) {
    size_t cap = (fragLen + n + 1) * 2;
    char* nb = (char*)heap_caps_realloc(frag, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) return;
    frag = nb;
    fragCap = cap;
  }
  memcpy(frag + fragLen, p, n);
  fragLen += n;
  frag[fragLen] = 0;
}

static void onEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      LOG("live: websocket connected, sending setup");
      st = St::Setup;
      stateSince = millis();
      ws.sendTXT(setupJson);
      break;
    case WStype_DISCONNECTED: {
      LOG("live: disconnected (state %d, close code %u, reason '%s', lib '%.*s')", (int)st, closeCode,
          closeReason.c_str(), (int)length, payload ? (const char*)payload : "");
      // payload may hold a reason from the library, e.g. "HTTP 403" if Google refused the upgrade
      String libReason = (payload && length) ? String((const char*)payload).substring(0, length) : String();
      if (st == St::Ready) {
        dropInfo = "connection dropped" + (closeCode ? " (code " + String(closeCode) + (closeReason.length() ? ": " + closeReason : String()) + ")" : String());
      }
      if (closeCode != 0 && closeCode != 1000) {
        fail(explainClose());
      } else if (st == St::Connecting || st == St::Setup) {
        if (libReason.startsWith("HTTP")) fail("Google refused the voice connection (" + libReason + ").");
        else if (closeReason.length()) fail(explainClose());
        else fail("Gemini closed the connection during setup.");
      }
      st = St::Idle;
      break;
    }
    case WStype_TEXT:
    case WStype_BIN:
      handleMessage(payload, length);
      break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
      fragLen = 0;
      fragAppend(payload, length);
      break;
    case WStype_FRAGMENT:
      fragAppend(payload, length);
      break;
    case WStype_FRAGMENT_FIN:
      fragAppend(payload, length);
      handleMessage((const uint8_t*)frag, fragLen);
      fragLen = 0;
      break;
    case WStype_ERROR:
      // with our library patch this carries the server's close frame: 2-byte code + reason
      if (payload && length >= 2) {
        closeCode = (uint16_t)(payload[0] << 8 | payload[1]);
        closeReason = String((const char*)payload + 2).substring(0, length - 2);
      }
      break;
    default:
      break;
  }
}

void begin() {
  pcmBuf = (uint8_t*)heap_caps_malloc(PCM_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  txBuf = (char*)heap_caps_malloc(TX_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ws.onEvent(onEvent);
  // Retry quickly while a session is being opened. (A long interval here would
  // also delay the very first connection.) We stop calling ws.loop() once a
  // session ends, so it never reconnects on its own.
  ws.setReconnectInterval(2000);
  ws.setExtraHeaders("");                           // no "Origin" header
}

bool open(Mode m, const String& timeText) {
  close();
  hadError = false;
  errText = "";
  closeCode = 0;
  closeReason = "";
  msgCount = 0;
  audioBytesIn = 0;
  seenKinds = "";
  dropInfo = "";
  evIn = evOut = "";
  evTurn = evInterrupted = evAudio = evGoAway = false;
  audioThisTurn = false;
  accept = false;
  curMode = m;
  LOG("live: open %s model=%s", m == Mode::Translate ? "TRANSLATE" : "TALK", cfg.model.c_str());
  toolCalls.clear();
  buildSetup(m, timeText);
  WiFi.setSleep(false);   // Wi-Fi power saving slows streaming audio down a lot
  String path = String(PATH) + cfg.apiKey;
  ws.beginSslWithCA(HOST, 443, path.c_str(), ROOT_CA_BUNDLE, "");
  ws.enableHeartbeat(20000, 5000, 2);
  st = St::Connecting;
  stateSince = millis();
  return true;
}

void close() {
  St prev = st;
  st = St::Idle;            // set first, so our own disconnect is not reported as an error
  if (prev != St::Idle) ws.disconnect();
  WiFi.setSleep(true);      // back to power saving between conversations
}

void loop() {
  if (st == St::Idle) return;
  ws.loop();
  if ((st == St::Connecting || st == St::Setup) && millis() - stateSince > 15000) {
    bool wasConnecting = st == St::Connecting;
    close();
    fail(wasConnecting ? "Could not open a secure connection to Gemini. Check the Wi-Fi has internet."
                       : "Connected, but Gemini did not finish setting up the conversation.");
  }
}

bool isOpen() { return st != St::Idle; }
bool ready() { return st == St::Ready; }
bool failed() { return hadError; }
String error() { return errText; }
void clearError() { hadError = false; errText = ""; }
void setAccept(bool on) { accept = on; }
Mode mode() { return curMode; }

bool sendActivityStart() {
  if (st != St::Ready) return false;
  return ws.sendTXT("{\"realtimeInput\":{\"activityStart\":{}}}");
}

bool sendActivityEnd() {
  if (st != St::Ready) return false;
  LOG("live: activityEnd sent");
  return ws.sendTXT("{\"realtimeInput\":{\"activityEnd\":{}}}");
}

bool sendAudio(const int16_t* pcm, size_t samples) {
  if (st != St::Ready || samples == 0) return false;
  static const char pre[] = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
  static const char post[] = "\"}}}";
  const size_t preLen = sizeof(pre) - 1, postLen = sizeof(post) - 1;
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char*)txBuf + preLen, TX_CAP - preLen - postLen - 1, &olen,
                            (const unsigned char*)pcm, samples * 2) != 0) {
    return false;
  }
  memcpy(txBuf, pre, preLen);
  memcpy(txBuf + preLen + olen, post, postLen);
  size_t total = preLen + olen + postLen;
  txBuf[total] = 0;
  return ws.sendTXT((uint8_t*)txBuf, total);
}

bool sendTextProbe(const String& text) {
  if (st != St::Ready) return false;
  JsonDocument d;
  d["realtimeInput"]["text"] = text;
  String s;
  serializeJson(d, s);
  return sendActivityStart() && ws.sendTXT(s) && sendActivityEnd();
}

String debugSummary() {
  String s = "Gemini messages: " + String(msgCount);
  if (seenKinds.length()) s += " (" + seenKinds + ")";
  if (audioBytesIn) s += ", audio " + String(audioBytesIn / 48000.0f, 1) + " s";
  s += st == St::Ready ? ", still connected" : (dropInfo.length() ? ", " + dropInfo : String(", not connected"));
  return s;
}

bool takeToolCall(ToolCall& out) {
  if (toolCalls.empty()) return false;
  out = toolCalls.front();
  toolCalls.erase(toolCalls.begin());
  return true;
}

bool sendToolResponse(const ToolCall& call, const String& responseJson) {
  if (st != St::Ready) return false;
  JsonDocument d;
  JsonObject r = d["toolResponse"]["functionResponses"].add<JsonObject>();
  r["id"] = call.id;
  r["name"] = call.name;
  r["response"] = serialized(responseJson);
  String s;
  serializeJson(d, s);
  LOG("live: toolResponse %s", s.c_str());
  return ws.sendTXT(s);
}

String takeInputText() { String s = evIn; evIn = ""; return s; }
String takeOutputText() { String s = evOut; evOut = ""; return s; }
bool takeTurnComplete() { bool b = evTurn; evTurn = false; return b; }
bool takeInterrupted() { bool b = evInterrupted; evInterrupted = false; return b; }
bool takeAudioStarted() { bool b = evAudio; evAudio = false; return b; }
bool goingAway() { return evGoAway; }

}  // namespace live
