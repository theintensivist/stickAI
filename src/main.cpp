// stickAI by Dr Abhijeet Anand - Gemini voice assistant for the M5Stack StickS3
//
//   Hold A ........ ask the assistant (also: "set a timer for 10 minutes", "remind me at 7 pm to...").
//                   Press A while it speaks to interrupt.
//   Hold B ........ English <-> Arabic interpreter: speak, let go, hear the translation.
//   Tap B ......... scroll back through the conversation.
//   Double-tap B .. menu: timers and reminders, phone page, mic test.
//   Hold B while switching on .. setup page.
//   When a timer or reminder rings: A = snooze 5 minutes, B = stop.

#include <M5Unified.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "settings.h"
#include "ui.h"
#include "audio.h"
#include "live.h"
#include "net.h"
#include "portal.h"
#include "web.h"
#include "timers.h"
#include "dbg.h"

static const char* FW_VERSION = "stickAI 0.11 2026-11-25";

enum class St : uint8_t { Idle, Listen, Think, Speak, Probe, Info, Menu, MicTest, Timers };
static St st = St::Idle;

static constexpr uint32_t TALK_MAX_MS = 30000;       // longest question
static constexpr uint32_t HOLD_MS = 450;             // hold B this long to start translating
static constexpr uint32_t KEEP_SESSION_MS = 45000;   // keep the connection for follow-ups
static constexpr uint32_t REPLY_TIMEOUT_MS = 20000;
static constexpr uint32_t PROBE_TIMEOUT_MS = 15000;
static constexpr uint32_t DIM_AFTER_MS = 30000;
static constexpr size_t SEND_BATCH = audio::CHUNK * 4;  // up to 400 ms per message

// Milliseconds since t. Safe when t was set after the caller read the clock.
static inline uint32_t since(uint32_t t) {
  int32_t d = (int32_t)(millis() - t);
  return d > 0 ? (uint32_t)d : 0;
}

// ---- current turn
static live::Mode turnMode = live::Mode::Talk;
static size_t sentSamples = 0;
static bool actStarted = false, actEnded = false, endPending = false, turnDone = false;
static bool gotAudio = false;
static uint32_t endAt = 0;
static String turnUser, turnReply;
static int userId = -1, replyId = -1;

// ---- buttons, screen, power
static uint32_t lastUse = 0, sessionIdleSince = 0, bDownAt = 0, lastBTap = 0, lastMeter = 0, lastTick = 0;
static bool dimmed = false, bHold = false, bPreCapture = false;
static bool aLocked = false, bLocked = false;   // ignore a button until it is released

// ---- "no reply" diagnosis
static uint32_t probeAt = 0;
static String probeInfo;
static bool probeGotReply = false;

// ---------------------------------------------------------------- helpers
static const char* modeWord() { return turnMode == live::Mode::Translate ? "Translating" : "Listening"; }

static void setState(St s) {
  st = s;
  switch (s) {
    case St::Idle:   ui::status("Hold A to talk", TFT_WHITE); break;
    case St::Listen:
      if (turnMode == live::Mode::Translate) ui::status("Interpreter: speak", TFT_MAGENTA);
      else ui::status("Listening...", TFT_GREEN);
      break;
    case St::Think:  ui::status("Thinking...", TFT_YELLOW); break;
    case St::Speak:  ui::status("Speaking  (A: stop)", TFT_CYAN); break;
    case St::Probe:  ui::status("Testing connection...", TFT_YELLOW); break;
    default: break;
  }
}

static void touch() {
  lastUse = millis();
  if (dimmed) { ui::dim(false); dimmed = false; }
}

static void lockHeldButtons() {
  aLocked = M5.BtnA.isPressed();
  bLocked = M5.BtnB.isPressed();
}

static void backToChat() {
  lockHeldButtons();
  ui::forceRender();
  setState(St::Idle);
  lastUse = millis();
}

static void resetTurn() {
  sentSamples = 0;
  actStarted = actEnded = endPending = turnDone = false;
  gotAudio = false;
  endAt = 0;
  turnUser = turnReply = "";
  replyId = -1;
}

// ---------------------------------------------------------------- timers and reminders
static String fmtDuration(uint32_t s) {
  if (s < 60) return String(s) + " s";
  if (s < 3600) return String(s / 60) + " min" + (s % 60 ? " " + String(s % 60) + " s" : String());
  return String(s / 3600) + " h " + String((s % 3600) / 60) + " min";
}

// A number from Gemini's arguments, whether sent as 600, 600.0 or "600".
static double numArg(JsonVariantConst v, double dflt = 0) {
  if (v.is<double>() || v.is<long>()) return v.as<double>();
  if (v.is<const char*>()) return String(v.as<const char*>()).toFloat();
  return dflt;
}

// Runs a tool Gemini asked for; returns the JSON reply for Gemini.
static String runTool(const live::ToolCall& c) {
  JsonDocument args;
  deserializeJson(args, c.args.length() ? c.args : String("{}"));
  JsonDocument r;
  if (c.name == "set_timer") {
    long secs = (long)(numArg(args["duration_seconds"]) + 0.5);
    String label = args["label"] | "";
    if (secs <= 0) { r["error"] = "duration_seconds must be more than 0"; }
    else {
      timers::Item it;
      uint32_t id = timers::addTimer((uint32_t)secs, label);
      for (size_t i = 0; i < timers::count(); i++) if (timers::get(i).id == id) it = timers::get(i);
      r["result"] = "ok";
      r["id"] = id;
      r["ends_at"] = timers::dueClock(it);
      ui::addLine(ui::Role::System, "Timer set: " + it.label + " (" + fmtDuration(secs) + ")");
    }
  } else if (c.name == "set_reminder") {
    String hm = args["time"] | "";
    String date = args["date"] | "";
    String label = args["label"] | "";
    time_t now = time(nullptr);
    int hh = -1, mm = -1;
    int colon = hm.indexOf(':');
    if (colon > 0) {
      hh = hm.substring(0, colon).toInt();
      mm = hm.substring(colon + 1, colon + 3).toInt();
      String low = hm;
      low.toLowerCase();
      if (low.indexOf("pm") >= 0 && hh < 12) hh += 12;   // in case Gemini says "7:00 PM"
      if (low.indexOf("am") >= 0 && hh == 12) hh = 0;
    }
    if (now < 1700000000) { r["error"] = "the device clock is not set (no internet time yet)"; }
    else if (hh < 0 || hh > 23 || mm < 0 || mm > 59) { r["error"] = "time must be HH:MM, 24-hour"; }
    else {
      struct tm t;
      localtime_r(&now, &t);
      if (date.length() == 10) {
        t.tm_year = date.substring(0, 4).toInt() - 1900;
        t.tm_mon = date.substring(5, 7).toInt() - 1;
        t.tm_mday = date.substring(8, 10).toInt();
      }
      t.tm_hour = hh; t.tm_min = mm; t.tm_sec = 0; t.tm_isdst = -1;
      time_t due = mktime(&t);
      if (date.length() != 10 && due <= now) due += 24 * 3600;   // next time that clock time comes
      if (due <= now) { r["error"] = "that time has already passed"; }
      else {
        uint32_t id = timers::addReminder(due, label);
        struct tm d;
        localtime_r(&due, &d);
        char when[32];
        strftime(when, sizeof(when), "%a %d %b %H:%M", &d);
        r["result"] = "ok";
        r["id"] = id;
        r["when"] = when;
        ui::addLine(ui::Role::System, "Reminder set: " + (label.length() ? label : String("Reminder")) + " (" + when + ")");
      }
    }
  } else if (c.name == "list_timers") {
    JsonArray a = r["items"].to<JsonArray>();
    for (size_t i = 0; i < timers::count(); i++) {
      timers::Item it = timers::get(i);
      JsonObject o = a.add<JsonObject>();
      o["id"] = it.id;
      o["type"] = it.reminder ? "reminder" : "timer";
      o["label"] = it.label;
      o["rings_at"] = timers::dueClock(it);
      o["time_left"] = timers::remainingText(it);
    }
    if (timers::count() == 0) r["note"] = "nothing is set";
  } else if (c.name == "cancel_timer") {
    int n = 0;
    if (args["all"] | false) n = timers::cancelAll();
    else if (!args["id"].isNull()) n = timers::cancel((uint32_t)numArg(args["id"]));
    else if (String(args["label"] | "").length()) n = timers::cancelByLabel(args["label"] | "");
    r["cancelled"] = n;
    if (n) ui::addLine(ui::Role::System, "Cancelled " + String(n) + (n == 1 ? " timer/reminder" : " timers/reminders"));
  } else {
    r["error"] = "unknown function";
  }
  String out;
  serializeJson(r, out);
  return out;
}

// Beeps and shows the label until A (snooze 5 min) or B (stop), or 60 s pass.
static void ringAlarm(const timers::Item& it) {
  LOG("alarm: %s '%s'", it.reminder ? "reminder" : "timer", it.label.c_str());
  live::close();
  audio::captureStop();
  audio::playStop();
  ui::dim(false);
  dimmed = false;
  String title = it.reminder ? "Reminder " + timers::dueClock(it) : String("Timer done");
  ui::page(title, it.label, 0, "A: snooze 5 min   B: stop");
  uint8_t vol = cfg.volume < 170 ? 170 : cfg.volume;
  audio::setVolume(vol);
  uint32_t t0 = millis(), lastBeep = 0;
  int n = 0;
  bool snooze = false;
  while (since(t0) < 60000) {
    M5.update();
    if (since(lastBeep) > 1000) {
      lastBeep = millis();
      audio::beep(n % 2 ? 1760 : 2093, 180);
      n++;
    }
    if (M5.BtnA.wasPressed()) { snooze = true; break; }
    if (M5.BtnB.wasPressed()) break;
    delay(10);
  }
  audio::playStop();
  audio::setVolume(cfg.volume);
  if (snooze) timers::addTimer(300, it.label);
  backToChat();
  ui::addLine(ui::Role::System, String(it.reminder ? "Reminder: " : "Timer done: ") + it.label + (snooze ? " (snoozed 5 min)" : ""));
}

static void checkAlarms() {
  timers::Item it;
  while (timers::popDue(it)) ringAlarm(it);
}

// Top bar: countdown of the next timer, or the time of the next reminder.
static void updateBarCountdown() {
  if (timers::count() == 0) { ui::setClockText(""); return; }
  timers::Item it = timers::get(0);
  long left = (long)(it.due - time(nullptr));
  ui::setClockText(!it.reminder && left < 3600 ? timers::remainingText(it) : "@" + timers::dueClock(it));
}

// Timers screen (menu): A = next, hold A 2 s = cancel highlighted, B = back.
static int timerSel = 0;
static uint32_t tADown = 0;
static bool tAHeld = false;

static void drawTimers() {
  size_t n = timers::count();
  if (n == 0) {
    ui::page("Timers and reminders", "Nothing set.\n\nHold A and say, for example:\n\"Set a timer for 10 minutes\"\n\"Remind me at 7 pm to call home\"", 0, "B: back");
    return;
  }
  if (timerSel >= (int)n) timerSel = 0;
  String body;
  for (size_t i = 0; i < n && i < 5; i++) {
    timers::Item it = timers::get(i);
    String when = it.reminder ? "at " + timers::dueClock(it) : timers::remainingText(it) + " left";
    body += String((int)i == timerSel ? "> " : "   ") + it.label + "  " + when + "\n";
  }
  ui::page("Timers and reminders (" + String(n) + ")", body, 0, "A: next  hold A: cancel  B: back");
}

static void timersLoop() {
  if (M5.BtnA.wasPressed()) { touch(); tADown = millis(); tAHeld = false; }
  if (M5.BtnA.isPressed() && !tAHeld && since(tADown) >= 2000 && timers::count()) {
    tAHeld = true;
    timers::cancel(timers::get(timerSel).id);
    drawTimers();
  }
  if (M5.BtnA.wasReleased() && !tAHeld && timers::count()) {
    timerSel = (timerSel + 1) % (int)(timers::count() < 5 ? timers::count() : 5);
    drawTimers();
  }
  if (M5.BtnB.wasReleased()) { touch(); backToChat(); return; }
  if (since(lastTick) > 1000) { lastTick = millis(); drawTimers(); }
}

// ---------------------------------------------------------------- menu (double-tap B)
static const char* const MENU_ITEMS[] = {"Timers and reminders", "Phone page (settings)", "Mic test", "Back"};
static constexpr int MENU_COUNT = 4;
static int menuSel = 0;

static void showPhoneLink() {
  st = St::Info;
  if (!net::online()) { ui::message("Phone page", "Needs Wi-Fi. Any button: back", TFT_YELLOW); return; }
  ui::qr(web::address(), "Phone page\nSame Wi-Fi as\nthe stick, then scan\nor open\nstickai.local\nPIN " + cfg.webPin);
}

// Mic test: hold A to record, let go to hear it back. B: change boost. Hold B: back.
static bool micRecording = false, micBHeld = false;
static uint32_t micBDown = 0;

static void drawMicTest(const String& result) {
  String body = result.length() ? result : String("Hold A and speak normally, then let go to hear your voice played back.");
  ui::page("Mic test   boost " + String(cfg.micGain) + "/9", body, 0, "hold A: record  B: boost  hold B: back");
}

static void openMicTest() {
  st = St::MicTest;
  micRecording = micBHeld = false;
  live::close();
  drawMicTest("");
}

static void micTestLoop() {
  if (M5.BtnA.wasPressed() && !micRecording) {
    touch();
    audio::captureStart(10000);
    micRecording = true;
    lastMeter = 0;
  }
  if (micRecording) {
    if (since(lastMeter) > 200) {
      lastMeter = millis();
      drawMicTest("Recording...  mic " + String(audio::micLevelRecent()) + "%\n\nLet go of A to hear it back.");
    }
    if (M5.BtnA.wasReleased() || audio::captureFull()) {
      audio::captureStop();
      micRecording = false;
      audio::processCaptured(audio::capturedSamples());
      int peak = audio::micLevelPeak();
      String verdict = peak >= 95 ? "Too loud: it distorts. Tap B to lower the boost."
                     : peak < 4   ? "Very quiet. Speak closer, or tap B to raise the boost."
                                  : "Good level.";
      drawMicTest("Loudest: " + String(peak) + "%  (aim 10-90%)\n" + verdict + "\nPlaying it back now...");
      audio::playRecording(10000);
    }
    return;
  }
  if (M5.BtnB.wasPressed()) { touch(); micBDown = millis(); micBHeld = false; }
  if (M5.BtnB.isPressed() && !micBHeld && since(micBDown) >= 600) { micBHeld = true; backToChat(); return; }
  if (M5.BtnB.wasReleased() && !micBHeld) {
    cfg.micGain = cfg.micGain >= 9 ? 1 : cfg.micGain + 1;
    audio::setMicGain(cfg.micGain);
    settingsSave();
    drawMicTest("Boost is now " + String(cfg.micGain) + "/9. Hold A to test again.");
  }
}

static void openMenu() {
  st = St::Menu;
  menuSel = 0;
  ui::menu("Menu", MENU_ITEMS, MENU_COUNT, menuSel);
}

static void menuLoop() {
  if (M5.BtnA.wasReleased()) {
    touch();
    menuSel = (menuSel + 1) % MENU_COUNT;
    ui::menu("Menu", MENU_ITEMS, MENU_COUNT, menuSel);
  }
  if (M5.BtnB.wasReleased()) {   // act on release, so the next screen doesn't see it
    touch();
    switch (menuSel) {
      case 0: st = St::Timers; timerSel = 0; lastTick = millis(); drawTimers(); break;
      case 1: showPhoneLink(); break;
      case 2: openMicTest(); break;
      default: backToChat(); break;
    }
  }
}

// ---------------------------------------------------------------- "no reply" diagnosis
static String micText(int peak) {
  String s = "mic level " + String(peak) + "%";
  if (peak < 3) s += " (almost silent: try Menu > Mic test)";
  return s;
}

static void startProbe() {
  probeInfo = "Sent " + String(sentSamples / (float)audio::MIC_RATE, 1) + " s of audio, " +
              micText(audio::micLevelPeak()) + ". " + live::debugSummary() + ".";
  if (!live::ready()) {
    live::close();
    ui::addLine(ui::Role::System, "No reply from Gemini. " + probeInfo);
    if (userId >= 0 && turnUser.isEmpty()) ui::setText(userId, "(no answer)");
    sessionIdleSince = millis();
    setState(St::Idle);
    return;
  }
  probeGotReply = false;
  live::takeOutputText();
  live::takeTurnComplete();
  live::takeAudioStarted();
  replyId = -1;
  turnReply = "";
  live::setAccept(true);
  live::sendTextProbe("This is a connection test from the device. Reply with just: Test OK.");
  probeAt = millis();
  setState(St::Probe);
}

static void probeLoop() {
  String out = live::takeOutputText();
  if (out.length()) {
    probeGotReply = true;
    if (replyId < 0) replyId = ui::addLine(ui::Role::Assistant, "");
    turnReply += out;
    ui::setText(replyId, turnReply);
  }
  if (live::takeAudioStarted()) probeGotReply = true;
  bool done = live::takeTurnComplete();
  if (done) audio::playFinish();
  if (live::failed()) return;
  if ((done && !audio::playActive()) || since(probeAt) > PROBE_TIMEOUT_MS) {
    if (userId >= 0 && turnUser.isEmpty()) ui::setText(userId, "(no answer)");
    String verdict = probeGotReply
        ? "No reply to your voice, but a typed test worked: the microphone audio is the problem."
        : "No reply to your voice, and a typed test got no reply either: the problem is the Gemini connection.";
    ui::addLine(ui::Role::System, verdict + " " + probeInfo);
    LOG("probe: %s | %s", verdict.c_str(), probeInfo.c_str());
    live::close();
    sessionIdleSince = millis();
    setState(St::Idle);
  }
}

static void reportFailure() {
  String why = live::error();
  live::clearError();
  live::close();
  audio::captureStop();
  audio::playStop();
  ui::status("Problem", TFT_RED);
  String diag = net::diagnoseGemini();
  if (diag.length()) why = diag;
  ui::addLine(ui::Role::System, why);
  if (userId >= 0 && turnUser.isEmpty()) ui::setText(userId, "(not sent)");
  setState(St::Idle);
}

// ---------------------------------------------------------------- a turn (assistant or interpreter)
static void startTurn(live::Mode mode, bool captureRunning) {
  touch();
  if (!net::online()) {
    if (captureRunning) audio::captureStop();
    ui::addLine(ui::Role::System, "No Wi-Fi. Reconnecting...");
    net::connectWifi(6000);
    if (!net::online()) return;
    captureRunning = false;
  }
  if (!captureRunning) {
    audio::playStop();
    audio::captureStart(TALK_MAX_MS);
  }
  live::setAccept(false);  // anything still arriving belongs to the previous reply
  resetTurn();
  turnMode = mode;
  if (!live::isOpen() || live::mode() != mode || live::goingAway()) {
    live::open(mode, mode == live::Mode::Talk ? net::localTimeText() : String());
  }
  userId = ui::addLine(ui::Role::User, "...");
  lastMeter = 0;
  setState(St::Listen);
}

static void endRecording() {
  audio::captureStop();
  endPending = true;
  LOG("recording stopped: %lu ms, sent so far %u samples", (unsigned long)audio::capturedMs(), (unsigned)sentSamples);
}

// Recording starts on the button press, before the connection is up; the audio
// is sent as soon as Gemini is ready, so nothing said while connecting is lost.
static void pumpAudio() {
  if (!live::ready()) return;
  if (!actStarted) {
    if (!live::sendActivityStart()) return;
    actStarted = true;
  }
  size_t avail = audio::capturedSamples();
  audio::processCaptured(avail);
  while (sentSamples < avail) {
    size_t n = avail - sentSamples;
    if (n > SEND_BATCH) n = SEND_BATCH;
    if (!live::sendAudio(audio::captureData() + sentSamples, n)) break;
    sentSamples += n;
  }
  if (endPending && !actEnded && sentSamples >= audio::capturedSamples()) {
    live::setAccept(true);
    if (live::sendActivityEnd()) {
      actEnded = true;
      endAt = millis();
    }
  }
}

static void handleTurnEvents() {
  String in = live::takeInputText();
  if (in.length()) {
    turnUser += in;
    String t = turnUser;
    t.trim();
    if (userId >= 0) ui::setText(userId, t);
  }
  String out = live::takeOutputText();
  if (out.length()) {
    if (replyId < 0) replyId = ui::addLine(ui::Role::Assistant, "");
    turnReply += out;
    String t = turnReply;
    t.trim();
    ui::setText(replyId, t);
  }
  live::ToolCall call;
  while (live::takeToolCall(call)) live::sendToolResponse(call, runTool(call));
  if (live::takeAudioStarted()) {
    gotAudio = true;
    if (st == St::Think) setState(St::Speak);
  }
  if (live::takeInterrupted()) audio::playStop();
  if (live::takeTurnComplete() && actEnded) {
    turnDone = true;
    audio::playFinish();
  }
}

static void finishTurnIfDone() {
  if (!turnDone || audio::playActive()) return;
  LOG("turn done (%s). heard='%s' reply='%s' underruns=%u", turnMode == live::Mode::Translate ? "translate" : "talk",
      turnUser.c_str(), turnReply.c_str(), (unsigned)audio::underruns());
  if (userId >= 0 && turnUser.isEmpty()) ui::setText(userId, "(speech not transcribed)");
  if (!gotAudio && turnReply.isEmpty()) ui::addLine(ui::Role::System, "No answer. Try again.");
  if (audio::underruns() > 2) {
    ui::addLine(ui::Role::System, "The voice paused " + String(audio::underruns()) + " times waiting for data (slow internet).");
  }
  sessionIdleSince = millis();
  lastUse = millis();
  setState(St::Idle);
}

// ---------------------------------------------------------------- power
static void goToSleep() {
  live::close();
  audio::playStop();
  ui::off();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_11, 0);  // button A (active low)
  rtc_gpio_pullup_en(GPIO_NUM_11);
  rtc_gpio_pulldown_dis(GPIO_NUM_11);
  time_t next = timers::nextDue();
  if (next) {   // wake up a few seconds early to ring the next timer or reminder
    long secs = (long)(next - time(nullptr)) - 4;
    if (secs < 1) secs = 1;
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  }
  LOG("sleeping (next alarm in %ld s)", next ? (long)(next - time(nullptr)) : -1L);
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  auto mcfg = M5.config();
  M5.begin(mcfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never wait for the PC: drop log text if nobody is reading

  settingsLoad();
  ui::begin(cfg.rotation);
  bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (!timerWake) ui::splash();
  if (!audio::begin()) {
    ui::message("Error", "Not enough memory (PSRAM missing?).", TFT_RED);
    while (true) delay(1000);
  }
  audio::setVolume(cfg.volume);
  audio::setMicGain(cfg.micGain);
  audio::setMicChannel(cfg.micChan);
  live::begin();
  timers::begin();

  // Woken up to ring a timer: the clock kept running in sleep, so ring first.
  if (timerWake) {
    uint32_t t0 = millis();
    while (timers::nextDue() && timers::nextDue() > time(nullptr) && since(t0) < 8000) delay(100);
    checkAlarms();
  }

  M5.update();
  if (!cfg.configured() || M5.BtnB.isPressed()) runSetupPortal();  // never returns

  ui::splashStatus("Connecting to Wi-Fi...");
  while (!net::connectWifi(10000)) {
    String nets = "\"" + cfg.ssid1 + "\"" + (cfg.ssid2.length() ? " or \"" + cfg.ssid2 + "\"" : "");
    ui::message("No Wi-Fi", "Couldn't join " + nets + ". Retrying...\n\nHold B to open the setup page.", TFT_RED);
    uint32_t t0 = millis();
    while (since(t0) < 5000) {
      M5.update();
      if (M5.BtnB.pressedFor(1000)) runSetupPortal();
      checkAlarms();   // timers still ring without Wi-Fi
      delay(20);
    }
  }
  ui::splashStatus("Connected. Setting the clock...");
  net::syncClock();
  web::begin();
  LOG("%s | ip=%s pin=%s model=%s boost=%u timers=%u", FW_VERSION, WiFi.localIP().toString().c_str(),
      cfg.webPin.c_str(), cfg.model.c_str(), cfg.micGain, (unsigned)timers::count());

  ui::forceRender();
  ui::addLine(ui::Role::System, "Hold A: ask (timers too). Hold B: English <-> Arabic. Double-tap B: menu.");
  setState(St::Idle);
  lastUse = millis();
  updateBarCountdown();
  lockHeldButtons();
}

void loop() {
  M5.update();
  web::loop();
  live::loop();
  audio::playService();

  // timers ring from any screen, once the current recording is done
  if (st != St::Listen && timers::nextDue() && timers::nextDue() <= time(nullptr)) checkAlarms();
  if (since(lastTick) > 1000 && st != St::Timers) { lastTick = millis(); updateBarCountdown(); }

  // ---- full-screen views
  if (st == St::Info) {
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) { touch(); backToChat(); }
    delay(5);
    return;
  }
  if (st == St::Menu)    { menuLoop();    delay(5); return; }
  if (st == St::MicTest) { micTestLoop(); delay(2); return; }
  if (st == St::Timers)  { timersLoop();  delay(5); return; }

  // a button that was still held when a screen closed is ignored until released
  if (aLocked && !M5.BtnA.isPressed()) aLocked = false;
  if (bLocked && !M5.BtnB.isPressed()) { bLocked = false; bHold = true; bPreCapture = false; }  // swallow that release

  // ---- button A: ask the assistant / interrupt
  if (!aLocked && M5.BtnA.wasPressed()) startTurn(live::Mode::Talk, false);

  // ---- button B: tap = scroll, double-tap = menu, hold = interpreter
  if (!bLocked && M5.BtnB.wasPressed()) {
    bDownAt = millis();
    bHold = false;
    touch();
    if (st == St::Idle) {   // start recording at once, so the first words are not lost
      audio::captureStart(TALK_MAX_MS);
      bPreCapture = true;
    }
  }
  if (!bLocked && M5.BtnB.isPressed() && !bHold && since(bDownAt) >= HOLD_MS && (st == St::Idle || st == St::Speak)) {
    bHold = true;
    startTurn(live::Mode::Translate, bPreCapture);
    bPreCapture = false;
  }

  // ---- end of a recording (A or B let go, or 30 s reached)
  if (st == St::Listen) {
    bool released = turnMode == live::Mode::Talk ? M5.BtnA.wasReleased() : M5.BtnB.wasReleased();
    if (released || audio::captureFull()) {
      endRecording();
      if (!actStarted && audio::capturedMs() < 400) {   // a tap, not speech
        ui::setText(userId, "(too short - hold the button while you speak)");
        endPending = false;
        setState(St::Idle);
      } else {
        setState(St::Think);
      }
    }
  }
  if (M5.BtnB.wasReleased() && st != St::Listen) {
    if (!bHold) {   // a tap
      if (bPreCapture) { audio::captureStop(); bPreCapture = false; }
      if (st == St::Idle || st == St::Speak) {
        if (since(lastBTap) < 450) { lastBTap = 0; openMenu(); return; }
        lastBTap = millis();
        ui::scrollUp();
      }
    }
    bHold = false;
  }

  // ---- conversation work
  if (live::failed()) reportFailure();

  if (st == St::Listen || st == St::Think || st == St::Speak) {
    pumpAudio();
    handleTurnEvents();
    if (st == St::Listen && since(lastMeter) > 250) {   // live mic meter in the status bar
      lastMeter = millis();
      String m = String(modeWord()) + "...  mic " + String(audio::micLevelRecent()) + "%";
      ui::status(m.c_str(), TFT_GREEN);
    }
    if (st == St::Think && actEnded && !gotAudio && turnReply.isEmpty() && since(endAt) > REPLY_TIMEOUT_MS) {
      startProbe();
    } else if ((st == St::Think || st == St::Speak) && actEnded && !turnDone && !live::isOpen() && !live::failed()) {
      audio::playFinish();
      ui::addLine(ui::Role::System, "The connection to Gemini dropped. " + live::debugSummary() + ". Try again.");
      setState(St::Idle);
    }
    if (st == St::Think || st == St::Speak) finishTurnIfDone();
  } else if (st == St::Probe) {
    probeLoop();
  } else if (st == St::Idle) {
    if (live::isOpen() && (since(sessionIdleSince) > KEEP_SESSION_MS || live::goingAway())) live::close();
  }

  // ---- screen dimming and sleep
  if (st == St::Idle && !audio::playActive() && !bPreCapture) {
    uint32_t idle = since(lastUse);
    if (!dimmed && idle > DIM_AFTER_MS) { ui::dim(true); dimmed = true; }
    time_t next = timers::nextDue();
    bool alarmSoon = next && next - time(nullptr) < 60;
    if (cfg.sleepMin > 0 && idle > (uint32_t)cfg.sleepMin * 60000UL && !M5.Power.isCharging() && !alarmSoon) goToSleep();
  }

  ui::render();
  delay(1);
}
