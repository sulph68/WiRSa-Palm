// Host tests for carddav_card.cpp (no ESP32 needed). From Firmware/:
//   g++ -std=c++17 -Wall -Wextra -Iinclude src/modules/carddav_card.cpp src/modules/palm_text.cpp src/modules/translit.cpp
//       src/modules/translit_data.cpp test_host/carddav_card_test.cpp -o /tmp/carddav_card_test   (one line)
//   /tmp/carddav_card_test                 unit checks
//   /tmp/carddav_card_test REPLY.xml       unit checks + each card of a saved
//                                          addressbook-query reply, printed
//   /tmp/carddav_card_test --stats REPLY.xml   totals only, no contents (safe
//                                          on a real address book)
#include "carddav_card.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

using namespace CardDav;

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static std::string hexify(const std::string &s) {   // non-ASCII bytes as \xNN
  std::string r; char b[8];
  for (unsigned char c : s) {
    if (c >= 0x80) { snprintf(b, sizeof b, "\\x%02X", c); r += b; } else r += (char) c;
  }
  return r;
}

static std::string xmlUnescape(std::string s) {
  struct { const char *a, *b; } m[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#13;", "\r"}, {"&amp;", "&"}};
  for (auto &e : m) { size_t p = 0; while ((p = s.find(e.a, p)) != std::string::npos) { s.replace(p, strlen(e.a), e.b); p += strlen(e.b); } }
  return s;
}

// address-data blocks of a multistatus reply
static std::vector<std::string> cardsOf(const std::string &xml) {
  std::vector<std::string> v; size_t p = 0;
  std::string s = xmlUnescape(xml);
  while ((p = s.find("BEGIN:VCARD", p)) != std::string::npos) {
    size_t e = s.find("END:VCARD", p);
    if (e == std::string::npos) break;
    v.push_back(s.substr(p, e + 9 - p)); p = e + 9;
  }
  return v;
}

static std::string readFile(const char *f) { std::ifstream in(f, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }

static void unitChecks() {
  CHECK(toPalmLatin("abc") == "abc", "ascii");
  CHECK(toPalmLatin("Jos\xC3\xA9 M\xC3\xBCller") == "Jos\xE9 M\xFCller", "latin-1 accents");
  CHECK(toPalmLatin("Zo\xC3\xAB \xC3\x85ngstr\xC3\xB6m") == "Zo\xEB \xC5ngstr\xF6m", "e diaeresis, A ring");
  CHECK(toPalmLatin("\xE2\x82\xAC") == "\x80", "euro");
  CHECK(toPalmLatin("\xE2\x80\x93\xE2\x80\x9C\xE2\x80\x9D") == "\x96\x93\x94", "en dash, curly quotes");
  CHECK(toPalmLatin("\xC5\x82\xC4\x8D") == "lc", "l stroke, c caron -> base letters");
  CHECK(toPalmLatin("\xC5\xBD\xC5\xBE") == "Zz", "Z/z caron: not in Palm Latin");
  CHECK(toPalmLatin("\xC5\xA0\xC5\x92\xC5\xB8") == "\x8A\x8C\x9F", "S caron, OE, Y diaeresis");
  CHECK(toPalmLatin("\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E") == "Wang Xiao Ming", "CJK -> pinyin");
  CHECK(toPalmLatin("\xD8\xA7\xD8\xB9") == "??", "scripts without a table (Arabic) -> ?");
  CHECK(toPalmLatin("A\xF0\x9F\x98\x80" "B") == "AB", "emoji dropped");
  CHECK(toPalmLatin("e\xCC\x81") == "e", "combining accent dropped");
  CHECK(toPalmLatin("\xE2\x9D\xA4\xEF\xB8\x8F") == "", "heart + variation selector dropped");
  CHECK(toPalmLatin("a\xFF" "b") == "a?b", "invalid byte -> ?");
  CHECK(toPalmLatin("a\xC3") == "a?", "truncated sequence -> ?");
  CHECK(toPalmLatin("\xC0\xAF") == "??", "overlong -> ?");

  Card c;
  CHECK(!trimCard("not a card", c), "rejects non-vCard");
  CHECK(!trimCard("BEGIN:VCARD\nFN:x\nEND:VCARD\n", c), "rejects card without UID");

  // folded line, \r\n, label on a line far from its item (server re-sorted)
  std::string v =
    "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:abc\r\nN:Doe;Jane;;;\r\nFN:Jane Doe\r\n"
    "item1.TEL;type=pref:+1 555\r\nNOTE:long line that is fol\r\n ded here\r\n"
    "item1.X-ABLabel:_$!<Mobile>!$_\r\nBDAY:--0704\r\nEND:VCARD\r\n";
  CHECK(trimCard(v, c), "parses simple card");
  CHECK(c.uid == "abc" && c.displayName == "Jane Doe" && c.sortName == "Doe, Jane", "names");
  CHECK(c.text.find("TEL;TYPE=\"Mobile\";type=pref:+1 555\n") != std::string::npos, "label moved into TYPE=, first");
  CHECK(c.text.find("NOTE:long line that is folded here\n") != std::string::npos, "unfolded");
  CHECK(c.text.find("BDAY:--0704\n") != std::string::npos, "no-year birthday");
  CHECK(c.text.find("FN:") == std::string::npos && c.text.find("UID:") == std::string::npos &&
        c.text.find("BEGIN:") == std::string::npos && c.text.find("VERSION") == std::string::npos, "dropped lines");
  CHECK(c.dates.size() == 1 && c.dates[0].kind == DK_BIRTHDAY && c.dates[0].year == 0 && c.dates[0].month == 7, "birthday date");

  // only FN, no N/ORG: becomes N so Address35 shows a name
  {  // romanisation of what Palm Latin can't show: Chinese -> pinyin, kana -> romaji, Hangul -> RR
    struct { const char *in, *want; } t[] = {
      {"\xE5\x8C\x97\xE4\xBA\xAC", "Bei Jing"},                                   // 北京
      {"\xE4\xBC\x9A\xE8\xAE\xAE\xE5\xAE\xA4" "A", "Hui Yi Shi A"},            // 会议室A
      {"Meeting \xE4\xBC\x9A\xE8\xAE\xAE", "Meeting Hui Yi"},
      {"\xE4\xBC\x9A\xE8\xAE\xAE(\xE5\x8C\x97)", "Hui Yi(Bei)"},
      {"\xE3\x81\x95\xE3\x81\x8F\xE3\x82\x89", "sakura"},                      // さくら
      {"\xE3\x83\xA9\xE3\x83\xBC\xE3\x83\xA1\xE3\x83\xB3", "raamen"},        // ラーメン
      {"\xE3\x81\x8D\xE3\x82\x83\xE3\x81\x8F", "kyaku"},                      // きゃく
      {"\xE3\x81\x8C\xE3\x81\xA3\xE3\x81\x93\xE3\x81\x86", "gakkou"},        // がっこう
      {"\xE3\x81\xA1\xE3\x82\x87\xE3\x81\xA3\xE3\x81\xA8", "chotto"},        // ちょっと
      {"\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xBC\xE3\x82\xAF", "fooku"},         // フォーク
      {"\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", "hangukeo"},                   // 한국어
      {"\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3\xEF\xBC\x91\xEF\xBC\x92", "ABC12"},   // fullwidth
      {"caf\xC3\xA9 \xE5\x8C\x97\xE4\xBA\xAC", "caf\xE9 Bei Jing"},
      {"\xE4\xBC\x9A\xE3\x80\x82", "Hui."},                                     // 会。
      {"\xE9\x87\x8D\xE5\xBA\x86", "Zhong Qing"},                                // known limit: first reading
    };
    for (auto &x : t) {
      std::string got = toPalmLatin(x.in);
      if (got != x.want) { printf("translit [%s]: got [%s] want [%s]\n", x.in, got.c_str(), x.want); }
      CHECK(got == x.want, "romanisation");
    }
    CHECK(toPalmLatin("\xE2\x82\xAC 5") == "\x80 5", "Palm Latin characters unchanged");
    CHECK(toPalmLatin("\xFF") == "?", "broken UTF-8 still a question mark");
  }
  {  // a big folded PHOTO is skipped without breaking the lines around it
    std::string big = "BEGIN:VCARD\r\nUID:p\r\nN:Pic;Pat;;;\r\nitem1.PHOTO;ENCODING=b;TYPE=JPEG:";
    for (int k = 0; k < 2000; k++) big += "/9j/4AAQSkZJRgABAQAAAQABAAD\r\n ";
    big += "\r\nTEL;TYPE=CELL:555-0100\r\nEND:VCARD\r\n";
    CHECK(trimCard(big, c) && c.text.find("N:Pic;Pat;;;\n") != std::string::npos &&
          c.text.find("555-0100") != std::string::npos && c.text.find("9j") == std::string::npos,
          "big folded PHOTO dropped, neighbours kept");
  }
  CHECK(trimCard("BEGIN:VCARD\nUID:x\nFN:Plumber\nEND:VCARD\n", c) && c.text == "N:Plumber;;;;\n", "FN-only -> N");
  // show as company
  CHECK(trimCard("BEGIN:VCARD\nUID:x\nN:;;;;\nORG:Acme;Sales\nX-ABShowAs:COMPANY\nEND:VCARD\n", c) &&
        c.displayName == "Acme" && c.sortName == "Acme" && c.text.find("X-ABSHOWAS") == std::string::npos, "show as company");
  // CATEGORIES with escaped comma
  CHECK(trimCard("BEGIN:VCARD\nUID:x\nN:A;B;;;\nCATEGORIES:Work,Family\\, close\nEND:VCARD\n", c) &&
        c.categories.size() == 2 && c.categories[1] == "Family, close", "categories split");
}

static void showRadicale(const char *file) {
  std::string xml = readFile(file);
  std::vector<std::string> cards = cardsOf(xml);
  printf("\n== %zu cards from %s ==\n", cards.size(), file);
  static const char *kind[] = {"Birthday", "Engagement", "Remembrance", "Wedding", "Work", "Other"};
  for (auto &v : cards) {
    Card c;
    if (!trimCard(v, c)) { printf("-- (not parsed)\n"); continue; }
    printf("-- uid=%s group=%d display=\"%s\" sort=\"%s\"\n", c.uid.c_str(), c.isGroup,
           hexify(c.displayName).c_str(), hexify(c.sortName).c_str());
    for (auto &d : c.dates) printf("   date: %04u-%02u-%02u %s%s%s\n", d.year, d.month, d.day, kind[d.kind],
                                   d.name.empty() ? "" : " ", hexify(d.name).c_str());
    for (auto &x : c.categories) printf("   category: %s\n", hexify(x).c_str());
    for (auto &m : c.members) printf("   member: %s\n", m.c_str());
    std::string t = c.text; size_t p = 0;
    while (!t.empty() && (p = t.find('\n')) != std::string::npos) { printf("   | %s\n", hexify(t.substr(0, p)).c_str()); t.erase(0, p + 1); }
  }
}

static void stats(const char *file) {
  std::vector<std::string> cards = cardsOf(readFile(file));
  size_t ok = 0, groups = 0, bad = 0, bytes = 0, maxb = 0, nonAscii = 0, dates = 0, bdays = 0, others = 0, cats = 0, rel = 0;
  std::set<std::string> uids; std::map<std::string, int> memberOf; size_t members = 0;
  std::map<int, int> kinds;
  for (auto &v : cards) {
    Card c;
    if (!trimCard(v, c)) { bad++; continue; }
    if (c.isGroup) { groups++; for (auto &m : c.members) { members++; memberOf[m]++; } continue; }
    ok++; uids.insert(c.uid);
    size_t b = c.text.size() + c.uid.size() + c.displayName.size() + c.sortName.size();
    bytes += b; if (b > maxb) maxb = b;
    for (unsigned char ch : c.text) if (ch >= 0x80) { nonAscii++; break; }
    for (auto &d : c.dates) { dates++; kinds[d.kind]++; if (d.kind == DK_BIRTHDAY) bdays++; if (d.kind == DK_OTHER) others++; }
    cats += c.categories.size();
    size_t p = 0; while ((p = c.text.find("X-ABRELATEDNAMES", p)) != std::string::npos) { rel++; p++; }
  }
  size_t matched = 0, multi = 0;
  for (auto &e : memberOf) { if (uids.count(e.first)) matched += e.second; if (e.second > 1) multi++; }
  printf("cards: %zu contacts, %zu groups, %zu not parsed\n", ok, groups, bad);
  printf("stored per contact (text + uid + names): total %zu bytes (%.0f KB), avg %zu, max %zu\n",
         bytes, bytes / 1024.0, ok ? bytes / ok : 0, maxb);
  printf("contacts with Palm Latin accented bytes: %zu\n", nonAscii);
  printf("dates: %zu (birthdays %zu; anniversaries by kind:", dates, bdays);
  for (int k = 1; k <= 5; k++) printf(" %d=%d", k, kinds[k]);
  printf(")\n");
  printf("relations kept: %zu   categories on contacts: %zu\n", rel, cats);
  printf("group members: %zu, matched to a contact UID: %zu, contacts in 2+ groups: %zu\n", members, matched, multi);
  // leaks: text that should have been dropped. Reported as pattern + the
  // property name of the line it is in (never the value), so a phone number
  // containing "1604" shows up as a harmless "1604 in TEL".
  std::map<std::string, int> leaks;
  for (auto &v : cards) { Card c; if (!trimCard(v, c) || c.isGroup) continue;
    size_t a = 0;
    while (a < c.text.size()) {
      size_t e = c.text.find('\n', a); if (e == std::string::npos) e = c.text.size();
      std::string line = c.text.substr(a, e - a), prop = line.substr(0, line.find_first_of(";:"));
      for (const char *bad2 : {"PHOTO", "X-IMAGEHASH", "X-ABLABEL", "RingTone", "VND-", "X-ADDRESSBOOKSERVER", "1604"})
        if (line.find(bad2) != std::string::npos) leaks[std::string(bad2) + " in " + prop]++;
      a = e + 1;
    } }
  printf("possible dropped-field leaks: %zu kinds\n", leaks.size());
  for (auto &e : leaks) printf("   %-40s x%d\n", e.first.c_str(), e.second);
}

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "--stats")) { stats(argv[2]); return 0; }
  unitChecks();
  printf("unit checks: %s\n", fails ? "FAILED" : "all passed");
  if (argc == 2) showRadicale(argv[1]);
  return fails ? 1 : 0;
}
