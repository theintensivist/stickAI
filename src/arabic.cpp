#include "arabic.h"
#include <vector>
#include <cstdint>

namespace arabic {

// ---- UTF-8 helpers -------------------------------------------------------
static std::vector<uint32_t> decode(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { i++; continue; }
    if (i + n > s.size()) break;
    for (int k = 1; k < n; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

static std::string encode(const std::vector<uint32_t>& v) {
  std::string s;
  for (uint32_t cp : v) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    } else {
      s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
      s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
    }
  }
  return s;
}

// ---- letter table ----------------------------------------------------------
// For U+0621..U+064A: first presentation form (isolated) and how many forms
// it has: 1 = never joins, 2 = joins on its right only, 4 = joins both sides.
struct Forms { uint16_t iso; uint8_t n; };
static const Forms TABLE[] = {
  {0xFE80, 1},  // 0621 hamza
  {0xFE81, 2},  // 0622 alef madda
  {0xFE83, 2},  // 0623 alef hamza above
  {0xFE85, 2},  // 0624 waw hamza
  {0xFE87, 2},  // 0625 alef hamza below
  {0xFE89, 4},  // 0626 yeh hamza
  {0xFE8D, 2},  // 0627 alef
  {0xFE8F, 4},  // 0628 beh
  {0xFE93, 2},  // 0629 teh marbuta
  {0xFE95, 4},  // 062A teh
  {0xFE99, 4},  // 062B theh
  {0xFE9D, 4},  // 062C jeem
  {0xFEA1, 4},  // 062D hah
  {0xFEA5, 4},  // 062E khah
  {0xFEA9, 2},  // 062F dal
  {0xFEAB, 2},  // 0630 thal
  {0xFEAD, 2},  // 0631 reh
  {0xFEAF, 2},  // 0632 zain
  {0xFEB1, 4},  // 0633 seen
  {0xFEB5, 4},  // 0634 sheen
  {0xFEB9, 4},  // 0635 sad
  {0xFEBD, 4},  // 0636 dad
  {0xFEC1, 4},  // 0637 tah
  {0xFEC5, 4},  // 0638 zah
  {0xFEC9, 4},  // 0639 ain
  {0xFECD, 4},  // 063A ghain
  {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},  // 063B..063F (not used)
  {0, 0},       // 0640 tatweel (handled separately)
  {0xFED1, 4},  // 0641 feh
  {0xFED5, 4},  // 0642 qaf
  {0xFED9, 4},  // 0643 kaf
  {0xFEDD, 4},  // 0644 lam
  {0xFEE1, 4},  // 0645 meem
  {0xFEE5, 4},  // 0646 noon
  {0xFEE9, 4},  // 0647 heh
  {0xFEED, 2},  // 0648 waw
  {0xFEEF, 2},  // 0649 alef maksura
  {0xFEF1, 4},  // 064A yeh
};

static bool isMark(uint32_t c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670; }

static uint32_t normalise(uint32_t c) {
  if (c == 0x06CC) return 0x064A;  // Farsi yeh -> yeh
  if (c == 0x06A9) return 0x0643;  // keheh -> kaf
  return c;
}

static const Forms* formsOf(uint32_t c) {
  if (c < 0x0621 || c > 0x064A) return nullptr;
  const Forms* f = &TABLE[c - 0x0621];
  return f->n ? f : nullptr;
}

static bool joinsNext(uint32_t c) {  // can connect to the following letter
  if (c == 0x0640) return true;
  const Forms* f = formsOf(c);
  return f && f->n == 4;
}
static bool joinsPrev(uint32_t c) {  // can connect to the previous letter
  if (c == 0x0640) return true;
  const Forms* f = formsOf(c);
  return f && f->n >= 2;
}
static bool isAlef(uint32_t c) { return c == 0x0622 || c == 0x0623 || c == 0x0625 || c == 0x0627; }

bool hasArabic(const std::string& utf8) {
  for (uint32_t c : decode(utf8)) if ((c >= 0x0600 && c <= 0x06FF) || (c >= 0xFE70 && c <= 0xFEFF)) return true;
  return false;
}

std::string shape(const std::string& utf8) {
  std::vector<uint32_t> in;
  for (uint32_t c : decode(utf8)) if (!isMark(c)) in.push_back(normalise(c));
  std::vector<uint32_t> out;
  for (size_t i = 0; i < in.size(); i++) {
    uint32_t c = in[i];
    const Forms* f = formsOf(c);
    if (!f) { out.push_back(c); continue; }
    bool prev = i > 0 && joinsNext(in[i - 1]) && joinsPrev(c);
    // lam + alef ligature
    if (c == 0x0644 && i + 1 < in.size() && isAlef(in[i + 1])) {
      uint32_t base;
      switch (in[i + 1]) {
        case 0x0622: base = 0xFEF5; break;
        case 0x0623: base = 0xFEF7; break;
        case 0x0625: base = 0xFEF9; break;
        default:     base = 0xFEFB; break;
      }
      out.push_back(prev ? base + 1 : base);
      i++;
      continue;
    }
    bool next = i + 1 < in.size() && joinsNext(c) && joinsPrev(in[i + 1]);
    uint32_t g;
    if (f->n == 1) g = f->iso;
    else if (f->n == 2) g = prev ? f->iso + 1 : f->iso;
    else if (prev && next) g = f->iso + 3;
    else if (next) g = f->iso + 2;
    else if (prev) g = f->iso + 1;
    else g = f->iso;
    out.push_back(g);
  }
  return encode(out);
}

static bool isLtr(uint32_t c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= 0x0660 && c <= 0x0669);  // Arabic-Indic digits also read left to right
}

std::string toVisual(const std::string& shapedLine) {
  std::vector<uint32_t> s = decode(shapedLine);
  // split into runs: LTR runs (Latin/digits, plus inner . , : % between them) and RTL runs
  std::vector<std::vector<uint32_t>> runs;
  std::vector<bool> ltr;
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t c = s[i];
    bool l = isLtr(c);
    if (!l && (c == '.' || c == ',' || c == ':' || c == '%' || c == '/') && i > 0 && i + 1 < s.size() &&
        isLtr(s[i - 1]) && isLtr(s[i + 1])) l = true;  // 3.5, 19:00, 50%
    if (runs.empty() || ltr.back() != l) { runs.push_back({}); ltr.push_back(l); }
    runs.back().push_back(c);
  }
  std::vector<uint32_t> out;
  for (int r = (int)runs.size() - 1; r >= 0; r--) {
    if (ltr[r]) out.insert(out.end(), runs[r].begin(), runs[r].end());
    else for (int k = (int)runs[r].size() - 1; k >= 0; k--) {
      uint32_t c = runs[r][k];
      if (c == '(') c = ')'; else if (c == ')') c = '(';   // mirror brackets
      out.push_back(c);
    }
  }
  return encode(out);
}

}  // namespace arabic
