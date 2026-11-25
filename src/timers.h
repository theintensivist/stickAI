#pragma once
#include <Arduino.h>
#include <time.h>

// Timers ("in 10 minutes") and reminders ("at 19:00"), kept on the stick so
// they ring even without internet, and saved so they survive sleep or restart.
namespace timers {

struct Item {
  uint32_t id = 0;
  time_t due = 0;        // when it rings (unix time)
  bool reminder = false; // true = clock-time reminder, false = countdown timer
  String label;
};

void begin();
uint32_t addTimer(uint32_t seconds, const String& label);
uint32_t addReminder(time_t due, const String& label);
size_t count();
Item get(size_t i);                  // sorted: soonest first
int cancel(uint32_t id);             // returns how many were cancelled
int cancelByLabel(const String& label);
int cancelAll();
bool popDue(Item& out);              // takes out one item that is due now
time_t nextDue();                    // 0 if none

String remainingText(const Item& it);  // "4:32", "1 h 05 min"
String dueClock(const Item& it);       // "19:00"

}  // namespace timers
