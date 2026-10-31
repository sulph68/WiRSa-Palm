// translit.cpp - see translit.h.
#include "translit.h"
#include "translit_data.h"
#include <string.h>

namespace {

bool isVowel(char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }

bool isSmallVowelKana(uint32_t c) {
  return (c >= 0x3041 && c <= 0x3049 && (c & 1)) || (c >= 0x30A1 && c <= 0x30A9 && (c & 1));
}
bool isSmallYa(uint32_t c) { return c == 0x3083 || c == 0x3085 || c == 0x3087 || c == 0x30E3 || c == 0x30E5 || c == 0x30E7; }
bool isSokuon(uint32_t c) { return c == 0x3063 || c == 0x30C3; }
bool isKana(uint32_t c) { return (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA); }

const char *kanaRomaji(uint32_t c) {
  return kKana[c <= 0x3096 ? c - TRANSLIT_HIRA_FIRST : TRANSLIT_HIRA_COUNT + (c - TRANSLIT_KATA_FIRST)];
}

// small ya/yu/yo -> the vowel letter
char smallYaVowel(uint32_t c) {
  c &= 0xFF;   // 0x83/0x85/0x87 (hiragana) or 0xE3/0xE5/0xE7 (katakana)
  return (c == 0x83 || c == 0xE3) ? 'a' : (c == 0x85 || c == 0xE5) ? 'u' : 'o';
}

size_t kanaAppend(const std::vector<uint32_t> &c, size_t i, std::string &out) {
  uint32_t cp = c[i];
  size_t n = c.size();
  if (cp == 0x30FC) {                                   // long vowel mark: repeat the last vowel
    for (size_t k = out.size(); k > 0; k--)
      if (isVowel(out[k - 1])) { out += out[k - 1]; return 1; }
    out += '-';
    return 1;
  }
  if (!isKana(cp)) return 0;
  if (isSokuon(cp)) {                                   // small tsu: double the next consonant
    if (i + 1 < n && isKana(c[i + 1]) && !isSokuon(c[i + 1])) {
      const char *r = kanaRomaji(c[i + 1]);
      if (r[0] == 'c' && r[1] == 'h') out += 't';       // tchi
      else if (!isVowel(r[0]) && r[0] != 'n') out += r[0];
    }
    return 1;
  }
  const char *r = kanaRomaji(cp);
  size_t rl = strlen(r);
  if (i + 1 < n && isSmallYa(c[i + 1]) && rl >= 2 && r[rl - 1] == 'i') {   // kya, sha, ja
    char v = smallYaVowel(c[i + 1]);
    bool soft = (rl == 3 && r[1] == 'h') || (rl == 2 && r[0] == 'j');       // shi, chi, ji
    out.append(r, rl - 1);
    if (!soft) out += 'y';
    out += v;
    return 2;
  }
  if (i + 1 < n && isSmallVowelKana(c[i + 1]) && !isSmallVowelKana(cp)) {  // fo, wi, ti
    char v = *kanaRomaji(c[i + 1]);
    if (rl == 1 && r[0] == 'u') { out += 'w'; out += v; }
    else if (rl >= 2 && (r[rl - 1] == 'u' || r[rl - 1] == 'i')) { out.append(r, rl - 1); out += v; }
    else { out += r; out += v; }
    return 2;
  }
  out += r;
  return 1;
}

// Revised Romanization of a precomposed Hangul syllable (U+AC00..U+D7A3)
size_t hangulAppend(uint32_t cp, std::string &out) {
  static const char *const L[19] = { "g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h" };
  static const char *const V[21] = { "a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae", "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i" };
  static const char *const T[28] = { "", "k", "k", "k", "n", "n", "n", "t", "l", "k", "m", "l", "l", "l", "p", "l", "m", "p", "p", "t", "t", "ng", "t", "t", "k", "t", "p", "t" };
  uint32_t s = cp - 0xAC00;
  out += L[s / 588];
  out += V[(s % 588) / 28];
  out += T[s % 28];
  return 1;
}

}  // namespace

size_t translitAppend(const std::vector<uint32_t> &cps, size_t i, std::string &out, bool *han) {
  uint32_t cp = cps[i];
  if (cp >= TRANSLIT_HAN_FIRST && cp <= TRANSLIT_HAN_LAST) {
    uint16_t idx = kHanIdx[cp - TRANSLIT_HAN_FIRST];
    if (!idx) return 0;
    const char *s = kPinyin[idx - 1];
    if (!out.empty()) {                                  // a space after a letter or digit, not after ( or a space
      char l = out[out.size() - 1];
      if ((l >= '0' && l <= '9') || (l >= 'A' && l <= 'Z') || (l >= 'a' && l <= 'z') || (unsigned char) l >= 0x80) out += ' ';
    }
    out += (char) (s[0] - 32);
    out += s + 1;
    *han = true;
    return 1;
  }
  size_t used = 0;
  if (cp == 0x30FC || (cp >= 0x3041 && cp <= 0x30FA)) used = kanaAppend(cps, i, out);
  else if (cp >= 0xAC00 && cp <= 0xD7A3) used = hangulAppend(cp, out);
  if (used) *han = false;
  return used;
}
