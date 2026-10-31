// carddav_card.cpp - see carddav_card.h. Plain C++, no Arduino calls (host
// tests build it as is).

#include "carddav_card.h"
#include "palm_text.h"

#include <cstdlib>
#include <map>
#include <set>

namespace CardDav {

namespace {

// ---- small string helpers ---------------------------------------------------

std::string upper(const std::string &s) {
  std::string r = s;
  for (char &c : r) if (c >= 'a' && c <= 'z') c = (char) (c - 32);
  return r;
}

std::string lower(const std::string &s) {
  std::string r = s;
  for (char &c : r) if (c >= 'A' && c <= 'Z') c = (char) (c + 32);
  return r;
}

bool startsWithNoCase(const std::string &s, const char *prefix) {
  size_t i = 0;
  for (; prefix[i]; i++) {
    if (i >= s.size()) return false;
    char a = s[i], b = prefix[i];
    if (a >= 'a' && a <= 'z') a = (char) (a - 32);
    if (b >= 'a' && b <= 'z') b = (char) (b - 32);
    if (a != b) return false;
  }
  return true;
}

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

// vCard TEXT unescape (\n \, \; \\)
std::string unescape(const std::string &s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char e = s[++i];
      r += (e == 'n' || e == 'N') ? '\n' : e;
    } else {
      r += s[i];
    }
  }
  return r;
}

// split at unescaped `sep`, each piece still escaped
std::vector<std::string> splitEscaped(const std::string &s, char sep) {
  std::vector<std::string> v;
  std::string cur;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) { cur += s[i]; cur += s[++i]; continue; }
    if (s[i] == sep) { v.push_back(cur); cur.clear(); continue; }
    cur += s[i];
  }
  v.push_back(cur);
  return v;
}

std::string stripUrnUuid(const std::string &s) {
  std::string t = trim(s);
  return startsWithNoCase(t, "urn:uuid:") ? t.substr(9) : t;
}

// Apple wraps its standard labels as "_$!<Mother>!$_" (4 + 4 wrapper chars)
std::string unwrapLabel(const std::string &s) {
  std::string t = unescape(s);
  if (t.size() >= 8 && t.compare(0, 4, "_$!<") == 0 && t.compare(t.size() - 4, 4, ">!$_") == 0)
    return t.substr(4, t.size() - 8);
  return t;
}

// a label made safe for a quoted TYPE= value: no quotes, ';', ',', ':'
std::string typeValue(const std::string &label) {
  std::string r;
  for (char c : label) r += (c == '"' || c == ';' || c == ',' || c == ':') ? ' ' : c;
  return trim(r);
}

// ---- one content line ---------------------------------------------------------

struct Prop {
  std::string group;    // "item1" or ""
  std::string name;     // upper case
  std::string params;   // raw, without the leading ';' ("" if none)
  std::string value;    // raw (still escaped)
};

bool parseLine(const std::string &line, Prop &p) {
  // the first ':' outside a quoted parameter value ends the name part
  bool q = false;
  size_t colon = std::string::npos;
  for (size_t i = 0; i < line.size(); i++) {
    if (line[i] == '"') q = !q;
    else if (line[i] == ':' && !q) { colon = i; break; }
  }
  if (colon == std::string::npos) return false;
  std::string np = line.substr(0, colon);
  p.value = line.substr(colon + 1);
  size_t semi = np.find(';');
  std::string name = semi == std::string::npos ? np : np.substr(0, semi);
  p.params = semi == std::string::npos ? "" : np.substr(semi + 1);
  size_t dot = name.find('.');
  p.group = dot == std::string::npos ? "" : name.substr(0, dot);
  p.name = upper(dot == std::string::npos ? name : name.substr(dot + 1));
  return !p.name.empty();
}

// value of parameter `key` (case-insensitive), "" if absent
std::string paramValue(const std::string &params, const char *key) {
  for (const std::string &part : splitEscaped(params, ';')) {
    size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    if (upper(trim(part.substr(0, eq))) == upper(key)) {
      std::string v = trim(part.substr(eq + 1));
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      return v;
    }
  }
  return "";
}

// "YYYY-MM-DD", "YYYYMMDD", "--MM-DD", "--MMDD" (+ optional time part)
bool parseDate(const std::string &s, uint16_t &y, uint8_t &m, uint8_t &d) {
  std::string t = trim(s);
  y = 0; m = 0; d = 0;
  size_t i = 0;
  if (t.compare(0, 2, "--") == 0) {
    i = 2;
  } else {
    int dig = 0;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9' && dig < 4) { y = (uint16_t) (y * 10 + (t[i] - '0')); i++; dig++; }
    if (dig != 4) return false;
  }
  if (i < t.size() && t[i] == '-') i++;
  if (i + 1 >= t.size() || t[i] < '0' || t[i] > '9' || t[i + 1] < '0' || t[i + 1] > '9') return false;
  m = (uint8_t) ((t[i] - '0') * 10 + (t[i + 1] - '0')); i += 2;
  if (i < t.size() && t[i] == '-') i++;
  if (i + 1 >= t.size() || t[i] < '0' || t[i] > '9' || t[i + 1] < '0' || t[i + 1] > '9') return false;
  d = (uint8_t) ((t[i] - '0') * 10 + (t[i + 1] - '0'));
  return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

std::string dateText(uint16_t y, uint8_t m, uint8_t d) {
  char b[12];
  if (y) {
    b[0] = (char) ('0' + (y / 1000) % 10); b[1] = (char) ('0' + (y / 100) % 10);
    b[2] = (char) ('0' + (y / 10) % 10);   b[3] = (char) ('0' + y % 10);
  } else {
    b[0] = '-'; b[1] = '-'; b[2] = 0; b[3] = 0;
  }
  std::string r(b, y ? 4 : 2);
  r += (char) ('0' + m / 10); r += (char) ('0' + m % 10);
  r += (char) ('0' + d / 10); r += (char) ('0' + d % 10);
  return r;
}

// Address35's anniversary rule (vcard.c): engage/rememb/wedding|marriage/
// work, any other label = Other + that label, none = Wedding.
uint8_t anniversaryKind(const std::string &label) {
  std::string l = lower(label);
  if (l.find("engage") != std::string::npos) return DK_ENGAGEMENT;
  if (l.find("rememb") != std::string::npos) return DK_REMEMBRANCE;
  if (l.find("wedding") != std::string::npos || l.find("marriage") != std::string::npos ||
      l == "anniversary") return DK_WEDDING;
  if (l.find("work") != std::string::npos) return DK_WORK;
  return l.empty() ? DK_WEDDING : DK_OTHER;
}

// Apple relation label -> the token Address35's vcRelFromToken() knows
const char *relationToken(const std::string &label) {
  std::string l = lower(label);
  if (l == "spouse" || l == "husband" || l == "wife") return "spouse";
  if (l == "partner" || l == "boyfriend" || l == "girlfriend") return "partner";
  if (l == "child" || l == "son" || l == "daughter") return "child";
  if (l == "parent" || l == "mother" || l == "father") return "parent";
  if (l == "sibling" || l == "sister" || l == "brother") return "sibling";
  if (l == "assistant") return "assistant";
  if (l == "manager") return "manager";
  if (l == "friend") return "friend";
  return "other";
}

// properties dropped outright (upper case, after the group prefix)
bool isDropped(const std::string &name) {
  static const char *const kDrop[] = {
    "BEGIN", "END", "VERSION", "PRODID", "REV", "UID", "KIND", "MEMBER",
    "PHOTO", "LOGO", "SOUND", "KEY",
    "X-ABLABEL", "X-ABSHOWAS", "X-ABUID", "X-ABADR",
    "X-IMAGEHASH", "X-IMAGETYPE", "X-SHARED-PHOTO-DISPLAY-PREF",
    "X-ADDRESSING-GRAMMAR",
  };
  for (const char *d : kDrop) if (name == d) return true;
  if (startsWithNoCase(name, "X-ADDRESSBOOKSERVER-")) return true;   // groups handled separately
  if (startsWithNoCase(name, "VND-")) return true;                   // e.g. VND-63-SENSITIVE-CONTENT-CONFIG
  return false;
}

// old instant-messaging properties -> the IMPP;TYPE=<name> Address35 parses
const char *imName(const std::string &name) {
  if (name == "X-AIM") return "AIM";
  if (name == "X-JABBER") return "Jabber";
  if (name == "X-MSN") return "MSN";
  if (name == "X-ICQ") return "ICQ";
  if (name == "X-YAHOO") return "Yahoo";
  if (name == "X-SKYPE" || name == "X-SKYPE-USERNAME") return "Skype";
  if (name == "X-GOOGLE-TALK" || name == "X-GTALK") return "Google Talk";
  if (name == "X-QQ") return "QQ";
  if (name == "X-GADUGADU") return "Gadu-Gadu";
  return nullptr;
}

}  // namespace

std::string toPalmLatin(const std::string &utf8) { return PalmText::fromUtf8(utf8); }

bool trimCard(const std::string &vcard, Card &out) {
  out = Card();

  // 1. lines: drop CRs, unfold continuation lines
  std::vector<std::string> lines;
  {
    std::string cur;
    bool have = false, skipping = false;   // skipping: inside a folded PHOTO-type value
    size_t i = 0, n = vcard.size();
    while (i < n) {
      size_t e = vcard.find('\n', i);
      if (e == std::string::npos) e = n;
      size_t len = e - i;
      if (len && vcard[e - 1] == '\r') len--;
      bool cont = len && (vcard[i] == ' ' || vcard[i] == '\t') && (have || skipping);
      if (cont) {
        if (!skipping) cur.append(vcard, i + 1, len - 1);
      } else {
        // a PHOTO/LOGO/SOUND/KEY value is dropped later anyway and can be hundreds of KB of
        // base64: never copy it (three copies of it exhausted the heap)
        size_t nameEnd = i;
        while (nameEnd < i + len && vcard[nameEnd] != ':' && vcard[nameEnd] != ';') nameEnd++;
        size_t dot = vcard.rfind('.', nameEnd);
        size_t ns = (dot != std::string::npos && dot >= i && dot < nameEnd) ? dot + 1 : i;
        std::string nm = upper(vcard.substr(ns, nameEnd - ns));
        skipping = (nm == "PHOTO" || nm == "LOGO" || nm == "SOUND" || nm == "KEY");
        if (have) lines.push_back(cur);
        if (skipping) { cur.clear(); have = false; }
        else { cur.assign(vcard, i, len); have = true; }
      }
      i = e + 1;
    }
    if (have) lines.push_back(cur);
  }

  std::vector<Prop> props;
  bool begun = false;
  for (const std::string &l : lines) {
    Prop p;
    if (!parseLine(l, p)) continue;
    if (p.name == "BEGIN" && upper(trim(p.value)) == "VCARD") begun = true;
    props.push_back(p);
  }
  if (!begun) return false;

  // 2. labels by group (servers may reorder lines, so collect them first)
  std::map<std::string, std::string> label;   // group -> label text
  std::set<std::string> ringTone;              // groups to drop entirely
  for (const Prop &p : props) {
    if (p.group.empty() || p.name != "X-ABLABEL") continue;
    std::string lb = unwrapLabel(p.value);
    if (lower(lb) == "ringtone") ringTone.insert(p.group);
    else label[p.group] = lb;
  }

  // 3. identity, names, group card
  std::string fn, nLast, nFirst, org;
  bool showCompany = false, haveN = false;
  for (const Prop &p : props) {
    if (p.name == "UID") out.uid = stripUrnUuid(p.value);
    else if (p.name == "FN") fn = unescape(p.value);
    else if (p.name == "N") {
      std::vector<std::string> c = splitEscaped(p.value, ';');
      nLast = c.size() > 0 ? trim(unescape(c[0])) : "";
      nFirst = c.size() > 1 ? trim(unescape(c[1])) : "";
      for (const std::string &x : c) if (!trim(unescape(x)).empty()) haveN = true;
    }
    else if (p.name == "ORG") org = trim(unescape(splitEscaped(p.value, ';')[0]));
    else if (p.name == "X-ABSHOWAS" && upper(trim(p.value)) == "COMPANY") showCompany = true;
    else if ((p.name == "X-ADDRESSBOOKSERVER-KIND" || p.name == "KIND") &&
             lower(trim(p.value)) == "group") out.isGroup = true;
  }
  if (out.uid.empty()) return false;

  if (out.isGroup) {
    out.displayName = toPalmLatin(trim(!fn.empty() ? fn : (!org.empty() ? org : nLast)));
    out.sortName = out.displayName;
    for (const Prop &p : props)
      if (p.name == "X-ADDRESSBOOKSERVER-MEMBER" || p.name == "MEMBER")
        out.members.push_back(stripUrnUuid(p.value));
    return true;
  }

  if (showCompany && !org.empty()) {
    out.displayName = out.sortName = org;
  } else if (!nLast.empty() || !nFirst.empty()) {
    out.displayName = !fn.empty() ? fn : trim(nFirst + " " + nLast);
    out.sortName = nLast.empty() ? nFirst : (nFirst.empty() ? nLast : nLast + ", " + nFirst);
  } else if (!fn.empty()) {
    out.displayName = out.sortName = fn;
  } else {
    out.displayName = out.sortName = org;
  }
  out.displayName = toPalmLatin(trim(out.displayName));
  out.sortName = toPalmLatin(trim(out.sortName));

  // 4. the trimmed text
  std::string text;
  auto emit = [&](const std::string &line) { text += toPalmLatin(line); text += '\n'; };
  CardDate bday{0, 0, 0, DK_BIRTHDAY, ""};
  bool haveBday = false;

  for (const Prop &p : props) {
    if (!p.group.empty() && ringTone.count(p.group)) continue;      // RingTone items
    if (isDropped(p.name)) continue;
    if (p.name == "FN") {
      // Address35 builds the name from N; keep FN only as N when nothing else names the card
      if (!haveN && org.empty() && !fn.empty()) emit("N:" + p.value + ";;;;");
      continue;
    }
    std::string enc = upper(paramValue(p.params, "ENCODING"));
    if (enc == "B" || enc == "BASE64" || startsWithNoCase(trim(p.value), "data:")) continue;

    std::string lb;
    if (!p.group.empty()) {
      auto it = label.find(p.group);
      if (it != label.end()) lb = it->second;
    }

    if (p.name == "BDAY") {
      uint16_t y; uint8_t m, d;
      if (!parseDate(p.value, y, m, d)) continue;
      std::string omit = paramValue(p.params, "X-APPLE-OMIT-YEAR");
      if ((!omit.empty() && y == (uint16_t) atoi(omit.c_str())) || y == 1604) y = 0;   // Apple's placeholder year (1604 even without the parameter)
      if (!haveBday) { bday = CardDate{y, m, d, DK_BIRTHDAY, ""}; haveBday = true; }
      emit("BDAY:" + dateText(y, m, d));
      continue;
    }
    if (p.name == "X-ABDATE" || p.name == "ANNIVERSARY" || p.name == "X-ANNIVERSARY") {
      uint16_t y; uint8_t m, d;
      if (!parseDate(p.value, y, m, d)) continue;
      std::string omit = paramValue(p.params, "X-APPLE-OMIT-YEAR");
      if ((!omit.empty() && y == (uint16_t) atoi(omit.c_str())) || y == 1604) y = 0;
      std::string kindLabel = p.name == "X-ABDATE" ? lb : paramValue(p.params, "TYPE");
      uint8_t kind = anniversaryKind(kindLabel);
      CardDate cd{y, m, d, kind, kind == DK_OTHER ? toPalmLatin(typeValue(kindLabel)) : std::string()};
      out.dates.push_back(cd);
      static const char *const kTok[] = {"", "engagement", "remembrance", "wedding", "work"};
      std::string type = kind == DK_OTHER ? "\"" + typeValue(kindLabel) + "\"" : kTok[kind];
      emit("ANNIVERSARY;TYPE=" + type + ":" + dateText(y, m, d));
      continue;
    }
    if (p.name == "X-ABRELATEDNAMES") {
      std::string tok = lb.empty() ? lower(paramValue(p.params, "TYPE")) : relationToken(lb);
      if (tok.empty()) tok = "other";
      emit("X-ABRELATEDNAMES;TYPE=" + tok + ":" + p.value);
      continue;
    }
    if (const char *im = imName(p.name)) {
      emit(std::string("IMPP;TYPE=\"") + im + "\":" + p.value);
      continue;
    }
    if (p.name == "CATEGORIES") {
      for (const std::string &c : splitEscaped(p.value, ',')) {
        std::string v = toPalmLatin(trim(unescape(c)));
        if (!v.empty()) out.categories.push_back(v);
      }
      emit("CATEGORIES:" + p.value);
      continue;
    }

    // everything else: group prefix off, Apple label (if any) into TYPE=
    // (the label goes first: Address35 keeps the FIRST TYPE= as a custom label)
    std::string line = p.name;
    if (!lb.empty() && (p.name == "TEL" || p.name == "EMAIL" || p.name == "ADR" ||
                        p.name == "URL" || p.name == "IMPP" || p.name == "X-SOCIALPROFILE"))
      line += ";TYPE=\"" + typeValue(lb) + "\"";
    if (!p.params.empty()) line += ";" + p.params;
    emit(line + ":" + p.value);
  }

  if (haveBday) out.dates.insert(out.dates.begin(), bday);
  out.text = text;
  return true;
}

}  // namespace CardDav
