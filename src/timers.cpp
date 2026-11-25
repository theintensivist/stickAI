#include "timers.h"
#include <LittleFS.h>
#include <vector>
#include <algorithm>

namespace timers {

static std::vector<Item> items;   // kept sorted by due time
static uint32_t nextId = 1;
static bool fsOk = false;
static const char* PATH = "/alarms.txt";
static const char* TMP = "/alarms.tmp";

static void sortItems() {
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.due < b.due; });
}

// One per line: id \t due \t reminder(0/1) \t label
static void save() {
  if (!fsOk) return;
  File f = LittleFS.open(TMP, "w");
  if (!f) return;
  for (auto& it : items) {
    String label = it.label;
    label.replace('\t', ' ');
    label.replace('\n', ' ');
    f.printf("%lu\t%ld\t%d\t%s\n", (unsigned long)it.id, (long)it.due, it.reminder ? 1 : 0, label.c_str());
  }
  f.close();
  LittleFS.remove(PATH);
  LittleFS.rename(TMP, PATH);
}

void begin() {
  fsOk = LittleFS.begin(true);
  items.clear();
  if (!fsOk) return;
  File f = LittleFS.open(PATH, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    int a = line.indexOf('\t'), b = line.indexOf('\t', a + 1), c = line.indexOf('\t', b + 1);
    if (a < 0 || b < 0 || c < 0) continue;
    Item it;
    it.id = line.substring(0, a).toInt();
    it.due = (time_t)line.substring(a + 1, b).toInt();
    it.reminder = line.substring(b + 1, c).toInt() != 0;
    it.label = line.substring(c + 1);
    items.push_back(it);
    if (it.id >= nextId) nextId = it.id + 1;
  }
  f.close();
  sortItems();
}

static uint32_t add(time_t due, bool reminder, const String& label) {
  Item it;
  it.id = nextId++;
  it.due = due;
  it.reminder = reminder;
  it.label = label.length() ? label : String(reminder ? "Reminder" : "Timer");
  items.push_back(it);
  sortItems();
  save();
  return it.id;
}

uint32_t addTimer(uint32_t seconds, const String& label) {
  if (seconds == 0) seconds = 1;
  return add(time(nullptr) + seconds, false, label);
}

uint32_t addReminder(time_t due, const String& label) { return add(due, true, label); }

size_t count() { return items.size(); }
Item get(size_t i) { return i < items.size() ? items[i] : Item(); }

int cancel(uint32_t id) {
  size_t before = items.size();
  items.erase(std::remove_if(items.begin(), items.end(), [id](const Item& it) { return it.id == id; }), items.end());
  int n = before - items.size();
  if (n) save();
  return n;
}

int cancelByLabel(const String& label) {
  String want = label;
  want.toLowerCase();
  size_t before = items.size();
  items.erase(std::remove_if(items.begin(), items.end(), [&want](const Item& it) {
                String l = it.label;
                l.toLowerCase();
                return l.indexOf(want) >= 0;
              }),
              items.end());
  int n = before - items.size();
  if (n) save();
  return n;
}

int cancelAll() {
  int n = items.size();
  items.clear();
  if (n) save();
  return n;
}

bool popDue(Item& out) {
  if (items.empty() || items.front().due > time(nullptr)) return false;
  out = items.front();
  items.erase(items.begin());
  save();
  return true;
}

time_t nextDue() { return items.empty() ? 0 : items.front().due; }

String remainingText(const Item& it) {
  long s = (long)(it.due - time(nullptr));
  if (s < 0) s = 0;
  char buf[24];
  if (s < 3600) snprintf(buf, sizeof(buf), "%ld:%02ld", s / 60, s % 60);
  else snprintf(buf, sizeof(buf), "%ld h %02ld min", s / 3600, (s % 3600) / 60);
  return String(buf);
}

String dueClock(const Item& it) {
  struct tm t;
  localtime_r(&it.due, &t);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

}  // namespace timers
