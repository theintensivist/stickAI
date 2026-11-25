#pragma once
#include <string>

// Minimal Arabic text support for the small screen: joins letters into their
// connected shapes (initial / medial / final / isolated, plus lam-alef) and
// reorders a line for right-to-left display. Plain C++ so it can be tested on a PC.
namespace arabic {

bool hasArabic(const std::string& utf8);

// Logical order in, logical order out, with letters replaced by their joined
// presentation forms (U+FE70..U+FEFC). Vowel marks are dropped.
std::string shape(const std::string& utf8);

// Reorders one already-shaped line into visual order (left to right on screen):
// Arabic runs are reversed, Latin letters and digits keep their own order.
std::string toVisual(const std::string& shapedLine);

}  // namespace arabic
