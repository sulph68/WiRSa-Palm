// palm_text.h - text for the Palm: UTF-8 <-> Palm Latin. One place for every relay that sends names, notes
// or messages to a Palm (CardDAV, CalDAV, BitWarden, Mesh). The core is plain C++ (host-tested:
// test_host/palm_text_test.cpp); the Arduino String adapter at the end is only built on the board.
//
// Palm Latin is Windows-1252 except the bytes Palm OS leaves undefined (0x81, 0x8D, 0x8E, 0x8F, 0x90, 0x9D, 0x9E).
// Never use it on secrets (passwords, keys, URIs, TOTP seeds): those must reach the Palm byte for byte.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace PalmText {

const uint8_t EMOJI_MARK = 0x10;     // what Marker mode writes for an emoji: character 0x10 of the Palm's Symbol font

enum class Emoji : uint8_t {
  Drop,     // emoji and pictographs vanish (contacts, calendar, vault items)
  Marker,   // each emoji, with its joiners, selectors and skin tones, becomes one EMOJI_MARK (names in lists)
};

struct Options {
  Emoji emoji = Emoji::Drop;
};

// UTF-8 -> Palm Latin. Latin-1 and the 1252 extras map 1:1; other accented Latin letters lose the accent; Chinese
// ideographs, Japanese kana and Korean Hangul are romanised (translit.h: "Bei Jing", "sakura", "hangukeo"); combining
// accents and zero-width characters are dropped; anything else (Arabic, Cyrillic, ...) becomes '?'. Pure ASCII is
// returned as it is. Invalid UTF-8 gives '?' for each broken sequence.
std::string fromUtf8(const std::string &utf8, const Options &opt = Options());

// Palm Latin -> UTF-8, for text the Palm sends. Returns the bytes written (no NUL counted); stops before a character
// that would not fit in cap. Bytes Palm OS leaves undefined become '?'.
size_t toUtf8(const char *in, size_t n, char *out, size_t cap);

}  // namespace PalmText

#ifdef ARDUINO
#include <WString.h>
namespace PalmText {
// The same for Arduino Strings (embedded NULs are not kept: names and texts have none).
inline String fromUtf8(const String &s, const Options &opt = Options()) {
  std::string r = fromUtf8(std::string(s.c_str(), s.length()), opt);
  String out;
  if (r.size()) out.concat((const uint8_t *) r.data(), (unsigned int) r.size());
  return out;
}
inline String fromUtf8(const char *s, const Options &opt = Options()) { return fromUtf8(String(s ? s : ""), opt); }
}  // namespace PalmText
#endif
