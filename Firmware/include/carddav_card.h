#ifndef CARDDAV_CARD_H
#define CARDDAV_CARD_H

// carddav_card - turn one vCard from a CardDAV server (iCloud, Radicale,
// Nextcloud...) into what the Palm stores: a trimmed vCard in Palm Latin,
// plus the values CalDAV Sync needs without parsing it (UID, display and
// sort name, birthday/anniversaries, categories, group members).
// Design: palmos_sdk apps/carddav_sync.md §2.3, §2.3.1, §2.4.
//
// Plain C++ (std::string), no Arduino calls, so it is tested on the host
// too. Not called by any mode yet.
//
// The trimmed text is meant for Address35's own vCard parser (vcard.c):
// - binary values (PHOTO, LOGO, SOUND, KEY, base64, data: URIs) and server
//   noise are dropped - Address35 would keep unknown properties as custom
//   fields;
// - Apple's itemN.X-ABLabel labels are moved into a TYPE= parameter, since
//   servers reorder lines and Address35 only pairs a label with the line
//   right before it;
// - iCloud's RingTone pseudo-relations are dropped;
// - X-ABDATE becomes ANNIVERSARY, X-AIM/X-JABBER/... become IMPP, dates are
//   written YYYYMMDD or --MMDD (Apple's X-APPLE-OMIT-YEAR = no year);
// - BEGIN/END/UID/FN are left out (UID travels separately; Address35 adds
//   BEGIN/END back and builds the name from N);
// - every value is converted from UTF-8 to Palm Latin (toPalmLatin).

#include <stdint.h>
#include <string>
#include <vector>

namespace CardDav {

// Birthday / anniversary kinds, as sent to the Palm (Address35's anniversary
// labels are kind - 1).
enum DateKind : uint8_t {
  DK_BIRTHDAY = 0, DK_ENGAGEMENT = 1, DK_REMEMBRANCE = 2, DK_WEDDING = 3,
  DK_WORK = 4, DK_OTHER = 5
};

struct CardDate {
  uint16_t year;        // 0 = unknown
  uint8_t month, day;
  uint8_t kind;         // DateKind
  std::string name;     // DK_OTHER: the anniversary's own name (Palm Latin)
};

struct Card {
  bool isGroup = false;              // a group card: name + members only
  std::string uid;                   // without "urn:uuid:"
  std::string displayName;           // Palm Latin
  std::string sortName;              // Palm Latin
  std::vector<CardDate> dates;       // birthday first, then anniversaries
  std::vector<std::string> categories;  // CATEGORIES values (Palm Latin)
  std::vector<std::string> members;  // group cards: member UIDs
  std::string text;                  // trimmed vCard lines, '\n'-separated, Palm Latin
};

// UTF-8 -> Palm Latin: PalmText::fromUtf8 (palm_text.h), kept under this name for the CardDAV code.
std::string toPalmLatin(const std::string &utf8);

// Parse and trim one vCard (BEGIN:VCARD...END:VCARD, any line endings,
// folded or not, XML already unescaped). False if it isn't a vCard or has
// no UID.
bool trimCard(const std::string &vcard, Card &out);

}  // namespace CardDav

#endif  // CARDDAV_CARD_H
