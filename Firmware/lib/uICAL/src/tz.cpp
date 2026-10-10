/*############################################################################
# Copyright (c) 2020 Source Simian  :  https://github.com/sourcesimian/uICAL #
############################################################################*/
#include "uICAL/cppstl.h"
#include "uICAL/types.h"
#include "uICAL/error.h"
#include "uICAL/tz.h"
#include "uICAL/datestamp.h"
#include "uICAL/epochtime.h"
#include "uICAL/tzmap.h"

namespace uICAL {

    TZ_ptr TZ::undef() {
        static TZ_ptr UNDEF = [](){
            TZ_ptr tz = new_ptr<TZ>();
            return tz;
        }();
        return UNDEF;
    }

    TZ_ptr TZ::unaware() {
        static TZ_ptr UNAWARE = [](){
            TZ_ptr tz = new_ptr<TZ>();
            tz->offsetMins = 0;
            tz->aware = false;
            return tz;
        }();
        return UNAWARE;
    }

    TZ::TZ() {
        this->offsetMins = -1;
        this->aware = false;
    }

    TZ::TZ(int offsetMins) {
        this->offsetMins = offsetMins;
        this->aware = true;
    }

    // Fallback IANA-name -> current standard-time UTC offset table (user
    // suggestion, 2026-09-17): a real report showed a VTIMEZONE registering
    // successfully (fixed the same day - see parseOffset's own 7-char
    // branch) yet the VEVENT referencing that same TZID still failed to
    // resolve it. Root cause is more fundamental than a parse bug - this
    // whole parser reads components strictly in document order with no
    // lookahead (VObjectStream::loadObject just calls stm.next()
    // repeatedly), so a VEVENT physically appearing before its own
    // VTIMEZONE in the raw ICS text (as at least one of a real CalDAV
    // server's per-event response blocks apparently does) genuinely has
    // nothing registered in the tzmap yet at the moment it's constructed.
    // Not a full tzdata replacement (no DST rules, no historical accuracy)
    // - deliberately a small, bounded set of commonly-used zones' CURRENT
    // standard-time offset, used only as a last resort when the real
    // VTIMEZONE data isn't available yet (or a server never embeds one at
    // all, relying on the client's own system tzdata - also common).
    // Extend this list as new "Bad timezone: <name>" reports come in for
    // zones not covered here.
    struct NamedZoneOffset { const char *name; int offsetMins; };
    static const NamedZoneOffset kNamedZoneOffsets[] = {
        {"UTC", 0}, {"GMT", 0}, {"Etc/UTC", 0}, {"Etc/GMT", 0},
        {"Asia/Singapore", 480}, {"Asia/Kuala_Lumpur", 480}, {"Asia/Hong_Kong", 480},
        {"Asia/Shanghai", 480}, {"Asia/Taipei", 480}, {"Asia/Manila", 480},
        {"Asia/Tokyo", 540}, {"Asia/Seoul", 540},
        {"Asia/Bangkok", 420}, {"Asia/Jakarta", 420}, {"Asia/Ho_Chi_Minh", 420},
        {"Asia/Kolkata", 330}, {"Asia/Calcutta", 330},
        {"Asia/Dubai", 240}, {"Asia/Karachi", 300},
        {"Australia/Sydney", 600}, {"Australia/Melbourne", 600}, {"Australia/Brisbane", 600},
        {"Australia/Perth", 480},
        {"Pacific/Auckland", 720},
        {"Europe/London", 0}, {"Europe/Dublin", 0},
        {"Europe/Paris", 60}, {"Europe/Berlin", 60}, {"Europe/Madrid", 60},
        {"Europe/Rome", 60}, {"Europe/Amsterdam", 60}, {"Europe/Zurich", 60},
        {"Europe/Athens", 120}, {"Europe/Helsinki", 120}, {"Europe/Moscow", 180},
        {"America/New_York", -300}, {"America/Chicago", -360},
        {"America/Denver", -420}, {"America/Los_Angeles", -480},
        {"America/Anchorage", -540}, {"America/Sao_Paulo", -180},
        {"America/Toronto", -300}, {"America/Vancouver", -480},
    };

    static bool namedZoneOffsetMinutes(const string& tz, int& outMinutes) {
        for (const auto& z : kNamedZoneOffsets) {
            if (tz == z.name) {
                outMinutes = z.offsetMins;
                return true;
            }
        }
        return false;
    }

    TZ::TZ(const string& tz) {
        this->offsetMins = TZ::parseOffset(tz);
        this->aware = true;
    }

    TZ::TZ(const string& tz, const TZMap_ptr& tzmap)
    : idmap(tzmap)
    {
        string id = this->idmap->findId(tz);
        if (!id.empty()) {
            this->id = id;
            this->aware = true;
        }
        else {
            int fallbackMins;
            if (namedZoneOffsetMinutes(tz, fallbackMins)) {
                this->offsetMins = fallbackMins;
            }
            else {
                this->offsetMins = TZ::parseOffset(tz);
            }
            this->aware = true;
        }
    }

    bool TZ::is_aware() const {
        return this->aware;
    }

    int TZ::parseOffset(const string& tz) {
        if (tz == "Z") {
            return 0;
        }
        try {
            if (tz.length() == 5) {
                char sign;
                unsigned tzH, tzM;

                // e.g.: +0200
                sign = tz.at(0);
                tzH = tz.substr(1, 2).as_int();
                tzM = tz.substr(3, 2).as_int();

                int offset = (tzH * 60) + tzM;
                if (sign == '-') {
                    offset *= -1;
                }
                return offset;
            }
            if (tz.length() == 7) {
                // e.g.: +065525 (RFC 5545 permits an optional seconds
                // component on TZOFFSETFROM/TZOFFSETTO, and real calendars
                // use it - real report, 2026-09-17: Apple's own VTIMEZONE
                // for Asia/Singapore includes its full pre-1933 historical
                // transition table, whose earliest entry is the actual,
                // documented historical Singapore Standard Time offset of
                // UTC+06:55:25 - not malformed data, just older than this
                // parser accounted for. Seconds are truncated, not rounded
                // - offsetMins only ever stores whole minutes (see the
                // header), so sub-minute precision can't be represented
                // regardless; every other modern client handling this same
                // real-world VTIMEZONE has the identical limitation.
                char sign;
                unsigned tzH, tzM;

                sign = tz.at(0);
                tzH = tz.substr(1, 2).as_int();
                tzM = tz.substr(3, 2).as_int();

                int offset = (tzH * 60) + tzM;
                if (sign == '-') {
                    offset *= -1;
                }
                return offset;
            }
        }
        catch (std::invalid_argument const &e)
        {}
        catch (std::out_of_range const &e)
        {}
        throw ValueError("Bad timezone: \"" + tz + "\"");
    }

    void TZ::offsetAsString(ostream& out, int offsetMins) {
        if (offsetMins != -1) {
            if (offsetMins == 0) {
                out << "Z";
            }
            else
            {
                if (offsetMins < 0) {
                    out << "-";
                    offsetMins *= -1;
                }
                else
                {
                    out << "+";
                }
                out << string::fmt(fmt_02d, offsetMins / 60);
                out << string::fmt(fmt_02d, offsetMins % 60);
            }
        }
    }

    int TZ::offset() const {
        if (!this->id.empty()) {
            return this->idmap->getOffset(this->id);
        }
        if (this->offsetMins == -1)
            throw ImplementationError("Timezone not defined");
        return this->offsetMins;
    }

    seconds_t TZ::toUTC(seconds_t timestamp) const {
        return timestamp - (this->offset() * 60);
    }

    seconds_t TZ::fromUTC(seconds_t timestamp) const {
        return timestamp + (this->offset() * 60);
    }

    void TZ::str(ostream& out) const {
        if (!this->id.empty()) {
            out << this->idmap->getName(this->id);
            return;
        }

        if (this->offsetMins == -1)
            throw ImplementationError("Timezone not defined");
        if (!this->aware)
            return;
        TZ::offsetAsString(out, this->offsetMins);
    }
}
