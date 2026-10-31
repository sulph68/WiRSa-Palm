// Host tests for palm_text.cpp (no ESP32 needed). From Firmware/:
//   g++ -std=c++17 -Wall -Wextra -Iinclude src/modules/palm_text.cpp src/modules/translit.cpp
//       src/modules/translit_data.cpp test_host/palm_text_test.cpp -o /tmp/palm_text_test && /tmp/palm_text_test   (one line)
#include "palm_text.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

using namespace PalmText;

static std::string name(const char *s) { Options o; o.emoji = Emoji::Marker; return fromUtf8(s, o); }

int main() {
  // fromUtf8, the default (emoji dropped)
  CHECK(fromUtf8("abc") == "abc", "ascii as it is");
  CHECK(fromUtf8("") == "", "empty");
  CHECK(fromUtf8("Jos\xC3\xA9 M\xC3\xBCller") == "Jos\xE9 M\xFCller", "latin-1 accents");
  CHECK(fromUtf8("\xE2\x82\xAC") == "\x80", "euro");
  CHECK(fromUtf8("\xC4\x80") == "A", "latin extended-A loses its accent");
  CHECK(fromUtf8("a\xF0\x9F\x94\xA5" "b") == "ab", "emoji dropped");
  CHECK(fromUtf8("Ab\xF0\x9F\x94") == "Ab?", "a sequence cut short is one '?'");
  CHECK(fromUtf8("\xE4\xB8\xAD\xE6\x96\x87") == "Zhong Wen", "ideographs romanised");
  CHECK(fromUtf8("x\xD0\x96y") == "x?y", "cyrillic is '?'");

  // fromUtf8, emoji as the marker
  CHECK(name("Dave \xF0\x9F\x94\xA5 Base") == "Dave \x10 Base", "one emoji");
  CHECK(name("\xF0\x9F\x94\xA5") == "\x10", "only an emoji");
  CHECK(name("\xF0\x9F\x94\xA5\xF0\x9F\x94\xA5") == "\x10", "a run of emoji is one mark");
  CHECK(name("X\xE2\x9D\xA4\xEF\xB8\x8F Y") == "X\x10 Y", "a variation selector belongs to its emoji");
  CHECK(name("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7") == "\x10", "a family (joiners) is one mark");
  CHECK(name("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBB") == "\x10", "a skin tone belongs to its emoji");
  CHECK(name("\xF0\x9F\x87\xB8\xF0\x9F\x87\xAC end") == "\x10 end", "a flag is one mark");
  CHECK(name("a\xF0\x9F\x94\xA5" "b\xF0\x9F\x94\xA5" "c") == "a\x10" "b\x10" "c", "two separate emoji");
  CHECK(name("Ab\xF0\x9F\x94") == "Ab?", "a cut-short sequence is '?' in this mode too");
  CHECK(name("Jos\xC3\xA9") == "Jos\xE9", "text without emoji is the same in both modes");
  CHECK(name("\xE2\x9C\x85 ok") == "\x10 ok", "a dingbat is an emoji");

  // toUtf8
  { char o[16]; size_t n = toUtf8("a\xE9\x80", 3, o, sizeof o);
    CHECK(n == 6 && !memcmp(o, "a\xC3\xA9\xE2\x82\xAC", 6), "latin to utf8");
    CHECK(toUtf8("\xE9\xE9\xE9", 3, o, 3) == 2, "stops before a character that does not fit");
    CHECK(toUtf8("\x81", 1, o, sizeof o) == 1 && o[0] == '?', "an undefined byte is '?'"); }

  // every byte makes the trip: Palm Latin -> UTF-8 -> Palm Latin (the undefined ones come back as '?')
  { static const unsigned char undef[] = {0x81, 0x8D, 0x8E, 0x8F, 0x90, 0x9D, 0x9E};
    int bad = 0;
    for (int b = 0x20; b < 256; b++) {
      char in = (char) b, u[8];
      size_t n = toUtf8(&in, 1, u, sizeof u);
      std::string back = fromUtf8(std::string(u, n));
      bool isUndef = false;
      for (unsigned char x : undef) if (x == b) isUndef = true;
      if (isUndef) { if (back != "?") bad++; }
      else if (back.size() != 1 || (unsigned char) back[0] != b) bad++;
    }
    CHECK(bad == 0, "every byte makes the round trip"); }

  if (fails) { printf("%d FAILED\n", fails); return 1; }
  printf("all passed\n");
  return 0;
}
