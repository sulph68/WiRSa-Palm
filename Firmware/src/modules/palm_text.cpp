// palm_text.cpp - see palm_text.h. Plain C++, no Arduino calls (host tests build it as is).

#include "palm_text.h"
#include "translit.h"

namespace PalmText {

namespace {

// Windows-1252 0x80..0x9F as Unicode; 0 = Palm OS leaves the byte undefined. Both directions use this one table.
const uint16_t kCp1252[32] = {
  0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0,      0,
  0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0,      0x0178,
};

// U+0100..U+017F (Latin Extended-A) without the accent. The few that Palm
// Latin has as their own character (OE/oe, S/s caron, Y diaeresis) are
// handled before this table.
const char kLatinExtA[] =
  "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Ii" "Jj" "Kkk"
  "LlLlLlLlLl" "NnNnNnnNn" "OoOoOo" "Oo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu" "Ww"
  "YyY" "ZzZzZz" "s";
static_assert(sizeof(kLatinExtA) == 128 + 1, "one letter per code point U+0100..U+017F");

// Code point -> Palm Latin byte(s). Empty = drop.
void appendPalm(std::string &out, uint32_t cp) {
  if (cp < 0x80) { out += (char) cp; return; }
  if (cp >= 0xA0 && cp <= 0xFF) { out += (char) cp; return; }   // Latin-1: same byte
  for (int i = 0; i < 32; i++)             // the Windows-1252 0x80-0x9F extras Palm Latin has (the same table serves toUtf8)
    if (kCp1252[i] == cp) { out += (char) (0x80 + i); return; }
  switch (cp) {
    // no Palm Latin equivalent: plain stand-ins
    case 0x2010: case 0x2011: case 0x2012: case 0x2212: out += '-'; return;
    case 0x2032: out += '\''; return;
    case 0x2033: out += '"'; return;
    case 0x2028: case 0x2029: out += '\n'; return;
    case 0x0218: out += 'S'; return;   // S/s/T/t with comma below (Romanian)
    case 0x0219: out += 's'; return;
    case 0x021A: out += 'T'; return;
    case 0x021B: out += 't'; return;
    default: break;
  }
  if (cp >= 0xFF01 && cp <= 0xFF5E) { out += (char) (cp - 0xFEE0); return; }   // fullwidth ASCII
  switch (cp) {   // CJK punctuation
    case 0x3001: case 0xFF64: out += ','; return;
    case 0x3002: case 0xFF61: out += '.'; return;
    case 0x300C: case 0x300D: case 0x300E: case 0x300F: case 0x300A: case 0x300B: out += '"'; return;
    case 0x3010: case 0x3014: out += '['; return;
    case 0x3011: case 0x3015: out += ']'; return;
    case 0x30FB: out += '.'; return;
    default: break;
  }
  if (cp >= 0x0100 && cp <= 0x017F) { out += kLatinExtA[cp - 0x0100]; return; }
  if (cp >= 0x2000 && cp <= 0x200A) { out += ' '; return; }      // typographic spaces
  if (cp == 0x202F || cp == 0x205F || cp == 0x3000) { out += ' '; return; }
  // dropped: combining accents, zero-width/joiners/marks, variation
  // selectors, BOM, emoji and pictographs, regional indicators, tags
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
      (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
      (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF ||
      (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) ||
      (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0xE0000 && cp <= 0xE007F) ||
      (cp >= 0xE0100 && cp <= 0xE01EF))
    return;
  out += '?';
}

// What Marker mode takes for an emoji: the pictograph blocks, plus the arrows, technical symbols and "!!" / "!?" that phones draw as pictures.
bool isPictograph(uint32_t cp) {
  return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) ||
         (cp >= 0x2300 && cp <= 0x23FF) || cp == 0x203C || cp == 0x2049 || (cp >= 0x2190 && cp <= 0x21FF);
}

// Zero-width joiner, variation selectors and tag characters: they belong to the emoji before them.
bool isEmojiGlue(uint32_t cp) {
  return cp == 0x200D || cp == 0xFE0F || cp == 0xFE0E || (cp >= 0xE0020 && cp <= 0xE007F);
}

}  // namespace

std::string fromUtf8(const std::string &utf8, const Options &opt) {
  bool ascii = true;
  for (unsigned char ch : utf8) if (ch >= 0x80) { ascii = false; break; }
  if (ascii) return utf8;                                   // the usual case: nothing to convert
  const uint32_t BAD = 0xFFFFFFFFu;
  std::vector<uint32_t> cps;
  cps.reserve(utf8.size());
  const unsigned char *p = (const unsigned char *) utf8.data();
  size_t n = utf8.size(), i = 0;
  while (i < n) {
    unsigned char c = p[i];
    uint32_t cp;
    int len;
    if (c < 0x80)                { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { cps.push_back(BAD); i++; continue; }  // stray continuation / bad lead byte
    if (i + len > n) { cps.push_back(BAD); break; }
    bool ok = true;
    for (int k = 1; k < len; k++) {
      if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (p[i + k] & 0x3F);
    }
    if (!ok || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) {
      cps.push_back(BAD); i++; continue;         // broken or overlong sequence
    }
    cps.push_back(cp);
    i += len;
  }
  std::string out;
  out.reserve(utf8.size());
  bool han = false;       // the last thing written was a pinyin syllable
  bool lastEmoji = false; // the last thing was an emoji (Marker mode)
  for (size_t k = 0; k < cps.size();) {
    if (cps[k] == BAD) { out += '?'; han = false; lastEmoji = false; k++; continue; }
    if (opt.emoji == Emoji::Marker) {
      if (isPictograph(cps[k])) { if (!lastEmoji) out += (char) EMOJI_MARK; lastEmoji = true; han = false; k++; continue; }
      if (lastEmoji && isEmojiGlue(cps[k])) { k++; continue; }      // joiners, variation selectors, tags: part of the same emoji
      lastEmoji = false;
    }
    size_t used = translitAppend(cps, k, out, &han);
    if (used) { k += used; continue; }
    size_t before = out.size();
    appendPalm(out, cps[k]);
    if (han && out.size() > before) {
      char f = out[before];
      if ((f >= '0' && f <= '9') || (f >= 'A' && f <= 'Z') || (f >= 'a' && f <= 'z'))
        out.insert(before, " ");                 // "Hui Yi Room", not "Hui YiRoom"
      han = false;
    }
    k++;
  }
  return out;
}

size_t toUtf8(const char *in, size_t n, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char b = (unsigned char) in[i];
    uint32_t cp = b;
    if (b >= 0x80 && b < 0xA0) { cp = kCp1252[b - 0x80]; if (!cp) cp = '?'; }
    size_t need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;
    if (o + need > cap) break;
    if (need == 1) out[o++] = (char) cp;
    else if (need == 2) { out[o++] = (char) (0xC0 | (cp >> 6)); out[o++] = (char) (0x80 | (cp & 0x3F)); }
    else { out[o++] = (char) (0xE0 | (cp >> 12)); out[o++] = (char) (0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char) (0x80 | (cp & 0x3F)); }
  }
  return o;
}

}  // namespace PalmText
