#!/usr/bin/env python3
# Checks the frame tables of docs/protocol/Protocol-wire.md (sections 9 to 12: Mesh, CalDAV, BitWarden, WiRSaCtl) against the code: the type numbers and names in the
# firmware headers, and in the Palm sources if their folder is given or found; and that each mode's Palm-sent bound (the junk filter) is the highest type the
# Palm sends (6.2 rule 4). Exit status 1 if anything differs.
#
#   check_wire.py [PALM_APPS_DIR]
#
# PALM_APPS_DIR is the palmos_sdk apps folder (hackmaster_y2035/apps); default ~/palmos_sdk/hackmaster_y2035/apps, skipped if it is not there.
# Gaps both sides agree on would be listed in ALLOWED below, with the reason.
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, ".."))
DOC = os.path.normpath(os.path.join(FW, "..", "docs", "protocol", "Protocol-wire.md"))
PALM = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/palmos_sdk/hackmaster_y2035/apps")

# (side, protocol, name) -> reason it may be missing on that side. None at the moment.
ALLOWED = {}
# names the firmware spells differently from the document (the document uses the Palm's longer names)
CTL_ALIAS = {"SECRETS": "SECRETS_SET", "ENTER": "ENTER_MODE", "EXIT": "EXIT_MODE", "FACTORY": "FACTORY_RESET"}

doc = open(DOC).read()
problems = 0


def section(num):
    m = re.search(r"^## %d\. .*$" % num, doc, re.M)
    n = re.search(r"^## \d+\. ", doc[m.end():], re.M)
    return doc[m.start(): m.end() + (n.start() if n else len(doc))]


def doc_table(num):
    rows = {}
    for m in re.finditer(r"^\| `0x([0-9A-F]{2})` \| ([A-Z0-9_]+) \|", section(num), re.M):
        rows.setdefault(m.group(2), set()).add(int(m.group(1), 16))
    return rows


def enum_vals(path, prefix):
    t = open(path).read()
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(r"\b(" + prefix + r"[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)", t)}


def defines(path, rx):
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(rx, open(path, errors="ignore").read())}


def snake(s):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", s).upper()


def compare(side, proto, docrows, code):
    global problems
    bad = []
    for n, vs in sorted(docrows.items()):
        if n not in code:
            if (side, proto, n) not in ALLOWED:
                bad.append("the document has %s, the code does not" % n)
        elif code[n] not in vs:
            bad.append("%s: document %s, code 0x%02X" % (n, sorted("0x%02X" % v for v in vs), code[n]))
    for n in sorted(code):
        if n not in docrows:
            bad.append("the code has %s (0x%02X), the document does not" % (n, code[n]))
    if bad:
        problems += len(bad)
        print("FAIL %-6s %-9s" % (side, proto))
        for b in bad:
            print("       " + b)
    else:
        print("ok   %-6s %-9s %d frames" % (side, proto, len(docrows)))


common = {k: v for k, v in doc_table(9).items() if k in ("HELLO", "HELLO_ACK", "BYE")}

mesh = {k[2:]: v for k, v in enum_vals(os.path.join(FW, "include/mesh_internal.h"), "M_").items()
        if k != "M_MAX_PALM_TYPE" and not k.startswith("M_ERR_")}
cddv = {k[5:]: v for k, v in enum_vals(os.path.join(FW, "include/caldav_internal.h"), "CDDV_").items()
        if not k.startswith(("CDDV_TYPE_", "CDDV_DIAGNOSTIC"))}
bw = {k[3:]: v for k, v in enum_vals(os.path.join(FW, "include/bitwarden_internal.h"), "BW_").items()
      if not k.startswith("BW_ERR_") and k != "BW_PROTOCOL_VERSION"}
ctl = {CTL_ALIAS.get(k[4:], k[4:]): v for k, v in enum_vals(os.path.join(FW, "src/modules/palm_ctl.cpp"), "CTL_").items() if k != "CTL_LEAD"}
compare("firmware", "Mesh", doc_table(9), mesh)
compare("firmware", "CalDAV", doc_table(10), cddv)
compare("firmware", "BitWarden", doc_table(11), bw)
compare("firmware", "control", doc_table(12), ctl)

def palm_sent_max(num):
    """The highest type the Palm sends in a protocol's table: the rows whose direction is P -> W or either."""
    best = 0
    for m in re.finditer(r"^\| `0x([0-9A-F]{2})` \| [A-Z0-9_]+ \| ([^|]*) \|", section(num), re.M):
        if "P -> W" in m.group(2) or "either" in m.group(2):
            best = max(best, int(m.group(1), 16))
    return best


def check_bound(proto, num, relay_cpp, header):
    """The mode's Palm-sent bound (RelayMode::Spec, the junk filter) must equal the highest type the Palm sends (Protocol-wire.md 6.2 rule 4)."""
    global problems
    spec = open(os.path.join(FW, "src/modules", relay_cpp)).read()
    m = re.search(r"BAUD_IDLE,\s*(\w+),\s*/\*needsWifi", spec)
    sym = m.group(1)
    h = open(os.path.join(FW, "include", header)).read()
    vals = {k: int(v, 16) for k, v in re.findall(r"\b([A-Z][A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)", h)}
    alias = re.search(r"\b" + sym + r"\s*=\s*([A-Z][A-Z0-9_]+);", h)
    bound = vals[sym] if sym in vals else vals[alias.group(1)]
    want = palm_sent_max(num)
    if bound != want:
        problems += 1
        print("FAIL bound  %-9s the junk-filter bound %s is 0x%02X, the highest type the Palm sends is 0x%02X" % (proto, sym, bound, want))
    else:
        print("ok   bound  %-9s %s = 0x%02X" % (proto, sym, bound))


check_bound("Mesh", 9, "mesh_relay.cpp", "mesh_internal.h")
check_bound("CalDAV", 10, "caldav_relay.cpp", "caldav_internal.h")
check_bound("BitWarden", 11, "bitwarden_relay.cpp", "bitwarden_internal.h")


if os.path.isdir(PALM):
    pm = {snake(k): v for k, v in defines(os.path.join(PALM, "meshtastic_client/Link.c"), r"#define\s+mw(\w+)\s+(0x[0-9A-Fa-f]+)").items()}
    pc = {k[5:]: v for k, v in enum_vals(os.path.join(PALM, "caldav_sync/Serial.h"), "CDDV_").items()}
    pb = {}
    for f in ("bitwarden/Sync.c", "bitwarden/BitWarden.h", "bitwarden/Wire.h"):
        for k, v in defines(os.path.join(PALM, f), r"#define\s+(BW_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)").items():
            pb[k[3:]] = v
    pb = {k: v for k, v in pb.items() if not k.startswith("ERR_")}
    wc = {snake(k[len("wctlType"):]): v for k, v in defines(os.path.join(PALM, "lib/wirsactl.h"), r"#define\s+(wctlType\w+)\s+(0x[0-9A-Fa-f]+)").items()}
    wc = {{"WIFI_SCAN": "WIFI_SCAN", "SECRETS": "SECRETS_SET", "ENTER": "ENTER_MODE", "EXIT": "EXIT_MODE", "FACTORY": "FACTORY_RESET",
           "SYS": "SYSINFO"}.get(k, k): v for k, v in wc.items()}
    compare("palm", "Mesh", doc_table(9), pm)
    compare("palm", "CalDAV", doc_table(10), pc)
    compare("palm", "BitWarden", doc_table(11), pb)
    compare("palm", "control", doc_table(12), wc)
else:
    print("skipped the Palm sources (%s not found)" % PALM)

print("\n%d difference(s)" % problems if problems else "\nall tables match the code")
sys.exit(1 if problems else 0)
