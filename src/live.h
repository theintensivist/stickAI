#pragma once
#include <Arduino.h>

// Minimal Gemini Live API client (WebSocket, push-to-talk).
//
// The stick turns off Gemini's automatic voice detection and marks each turn
// itself with activityStart / activityEnd, which matches "hold the button
// while you talk". Gemini answers with 24 kHz audio plus a text transcript
// of what it says (shown as captions) and of what you said.
namespace live {

enum class Mode : uint8_t { Talk, Translate };

// A request from Gemini to run one of the stick's tools (timers, reminders).
struct ToolCall { String id, name, args; };   // args = JSON object text

void begin();
bool open(Mode mode, const String& timeText);  // start connecting (non-blocking)
void close();
void loop();          // call every loop()

bool isOpen();        // connecting or connected
bool ready();         // setup finished, ready for audio
bool failed();        // the last session ended with an error
String error();       // short description of that error
void clearError();
void setAccept(bool on);  // accept reply audio/text (off while a new question is being recorded)
Mode mode();

bool sendActivityStart();
bool sendActivityEnd();
bool sendAudio(const int16_t* pcm, size_t samples);  // 16 kHz mono
bool sendTextProbe(const String& text);  // typed test question (no mic), for diagnosing "no reply"
String debugSummary();                   // what Gemini has sent so far, for error messages

// Events since the last call (each call clears them)
String takeInputText();   // transcript of what the user said
String takeOutputText();  // transcript of what Gemini is saying
bool takeTurnComplete();
bool takeInterrupted();
bool takeAudioStarted();  // first audio of a reply arrived
bool goingAway();         // server asked us to close soon
bool takeToolCall(ToolCall& out);                       // next pending tool request
bool sendToolResponse(const ToolCall& call, const String& responseJson);

}  // namespace live
