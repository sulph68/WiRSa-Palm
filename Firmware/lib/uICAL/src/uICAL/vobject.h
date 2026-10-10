/*############################################################################
# Copyright (c) 2020 Source Simian  :  https://github.com/sourcesimian/uICAL #
############################################################################*/
#ifndef uical_vobject_h
#define uical_vobject_h

#include "uICAL/base.h"

namespace uICAL {
    class VLine;
    class VObjectStream;

    class VObject : public Base {
        public:
            VObject();

            virtual const string& getName() const;

            VLine_ptr getPropertyByName(const string& name) const;
            using vector = std::vector<VObject_ptr>;
            vector listObjects(const string& name) const;

            // WiRSa CalDAV relay: getPropertyByName() only ever returns the
            // FIRST matching line - fine for singular properties (SUMMARY,
            // DTSTART, ...) but EXDATE legitimately repeats (RFC 5545: one
            // line per excluded occurrence, or a comma-separated list within
            // a line, or both). Same shape as the existing listObjects(),
            // just over `lines` instead of `children`.
            using line_vector = std::vector<VLine_ptr>;
            line_vector listProperties(const string& name) const;

            void str(ostream& out) const;

        protected:

            string name;
            std::vector<VLine_ptr> lines;
            std::vector<VObject_ptr> children;

            friend class VObjectStream;
    };
}
#endif
