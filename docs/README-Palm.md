# WiRSa — Palm OS Modes (SASD SDCard, HotSync, FTP Server, BitWarden & CalDAV Relay)

This fork of [WiRSa](https://github.com/nullvalue0/WiRSa) adds several modes purpose-built for Palm OS
devices: **SASD SDCard mode** lets a Palm mount WiRSa's SD card as a native, always-available VFS
volume, and **HotSync mode** lets a Palm HotSync directly against WiRSa — no computer, no Palm
Desktop, no cable to a PC at all. **BitWarden Relay** and **CalDAV Relay** let dedicated Palm apps sync a
Bitwarden/Vaultwarden vault and CalDAV/iCloud/Google/ICS calendars respectively, with WiRSa doing the
network side over WiFi so the Palm never needs its own TCP/TLS stack. It also adds a general-purpose **FTP
Server mode** — not Palm-specific, but useful alongside the others for uploading files onto the SD card
before a HotSync/SASD run. This document covers all of those, plus the SLIP Gateway. For the hardware
list see the main [README.md](../README.md). The original project's own documentation is kept as
[README-Original.md](README-Original.md) — note that the MODEM mode, AT commands and built-in web
config server it describes are **not present in this firmware** (see "What changed" below).

> **Branch note.** This describes the `improved` branch, which differs from `main` in these areas:
> - The SLIP Gateway has no menu of its own, its addresses are set under Config → SLIP Setup, and
>   port forwarding and the serial statistics dump are removed. See "SLIP Gateway" below.
> - WiRSa can be controlled from the Palm with the **WiRSaCtl** app, and can be built **without a
>   screen or buttons**. See "Controlling WiRSa from the Palm" below.
> - The **BOOT button** starts HotSync, modes go back to the main menu by themselves (**Exit
>   Timers**), and SLIP Gateway stops after a quiet spell (**SLIP timeout**).
> - The main menu starts at **9600** after every boot, and each Palm's WiRSaCtl moves it to that
>   Palm's own speed. The configured baud rate is the maximum for syncs. See "Baud rate: one WiRSa,
>   Palms of different speeds" below.

## What changed from the original WiRSa

- **File Transfer mode is gone**, replaced by **HotSync**.
- **Text Playback mode is gone**, replaced by **SASD SDCard**.
- **PPP Gateway mode has been removed entirely** to make room.
- **MODEM mode is gone**, replaced by **FTP Server mode** (see below).
- **Three relay modes added**: **BitWarden Relay**, **CalDAV Relay** and **Mesh Relay (Meshtastic)** — each pairs with its own
  dedicated Palm app (BitWarden, CalDAV Sync) and does the WiFi/TLS/HTTP work on WiRSa's side so the
  Palm app itself only ever speaks a simple framed protocol over serial. See below.
- **The built-in web config server is gone.** Configuration is via the OLED+button menu or typed
  serial only.
- **Serial Config (data bits/parity/stop bits) is no longer a menu setting** — locked to 8-N-1. Baud
  Rate is still user-selectable as before.
- **No modem control lines are used.** RTS/CTS hardware flow control and DTR were removed (not wired
  on this hardware), and with MODEM mode gone nothing drives DCD, DSR or RI either. Only the serial
  data lines are used.
- **WiRSa now manages WiFi in the background** — see "Background WiFi & the status icon" below.
- **Utilities has been trimmed down**: Auto-Detect Baud, Hex Dump Mode, and Loopback Test are no
  longer menu items (and with MODEM mode gone there are no AT commands for them either), and Signal
  Monitor is gone entirely. A new **Retry WiFi** item was added, and **Information** was redesigned to
  scroll instead of a small popup that used to cut text off. Utilities is now just those two items.
- **SLIP Gateway is simpler and more reliable**: it starts straight from the main menu and Back
  stops it; there is no SLIP submenu, port forwarding or statistics dump. Its addresses are set in
  the new **Config → SLIP Setup** item (see below).
- **Palm control link** (`improved`): the WiRSaCtl Palm app shows WiRSa's status and sets it up over
  the same serial line, and CalDAV Sync and BitWarden can switch WiRSa into their mode themselves.
  Single menu keys from the Palm line now count only when they arrive on their own (see below).
- **Factory Reset always asks first**: ENTER/BACK on the buttons, or `Y` when typed.
- **Virtual card** (`improved`): 12 MB of the ESP32's flash is a card that is always there, named
  **ESP32**. SASD, HotSync and the FTP server can use it (see "Virtual card" below).
- **Logging** (`improved`): leveled log with a session log the Palm can read (see "Logs and the System
  page" below).
- **Exit Timers** and **SLIP timeout** (`improved`): HotSync, CalDAV Relay, BitWarden Relay and SLIP
  Gateway return to the main menu by themselves. Set in **Config → Exit Timers**.
- **Menu at 9600, Palm-chosen speed** (`improved`): the main menu starts at 9600 after every boot;
  **Config → Baud Rate (Max)** is now the fastest rate syncs may use. The rate in the menu's
  top-right corner is the rate the menu is using right now.

---

## Background WiFi & the status icon

WiRSa now keeps your configured WiFi network connected automatically, in the background, all the
time — no mode has to bring WiFi up itself anymore before it can be used.

- Every screen's title bar now shows a small 3-bar signal-strength icon (top-right, next to the baud
  rate): solid bars while connected, animated/climbing bars while trying to connect, and a static
  X once it gives up.
- WiRSa retries connecting for about a minute and a half. If it still can't connect after that, it
  stops trying and shows the X — this matches what real hardware actually does (WiFi tends to stop responding to
  repeated connection attempts after about that long anyway), rather than spinning forever with no
  visible outcome.
- To try again, go to **Utilities → Retry WiFi**. **Utilities → Information** shows the network name,
  signal strength and WiRSa's IP address once connected.
- Any mode that needs WiFi (FTP Server, SLIP Gateway, BitWarden Relay, CalDAV Relay) will
  automatically show **"Waiting for
  WiFi..."** and try connecting itself if it isn't already connected when you enter it. If that also
  times out, you'll see **"WiFi unavailable. Retry?"** — press the center/select button to try again,
  or Back to cancel and return to the menu you came from.

---

## SASD SDCard mode

Mounts WiRSa's SD card as a real, native storage card on the Palm — the same way a physical SD card
slot would, if your Palm had one. Wire-compatible with [dmitry.gr's SASD
hardware](https://dmitry.gr/?r=05.Projects&proj=32.%20SASD) and its Palm-side driver, so this works with
the **unmodified `SASD.prc`** slot driver — no special app on the Palm side, and every app that already
uses the standard VFS APIs (file browsing in Address35, card-file features in CryptoNote, beam/export
to card, etc.) just works, with zero changes needed on either side.

### Requirements

- A microSD card inserted in WiRSa, formatted FAT16 or FAT32. On the `improved` branch the card can
  be the internal **virtual card** instead (see "Virtual card"): with Disk set to Auto, WiRSa serves it when no
  SD card answers.
- The Palm's `SASD.prc` slot driver installed (from the original SASD project — this is the Palm-side
  half; WiRSa is standing in for the SASD *hardware*). Over a wireless serial link use the improved driver (see "Over a wireless serial link" below).

### Using it

1. From WiRSa's main menu, select **`SAS[D] SDCard`**.
2. WiRSa initializes the SD card and enters SASD mode — the screen shows the card's used/total space
   and a live transfer speed indicator.
3. On the Palm, the card just appears wherever VFS-aware apps look for storage. Browse, copy, beam to
   card, split large files across the card — all standard VFS behavior.
4. Press the **Back** button (or send `+++` over the USB serial console) to exit SASD mode and return
   to the main menu.

### What it shows

- **Title bar**: `SASDCard` with the current live baud rate and WiFi status in the top-right corner.
- **`SD:`**: used/total card space (human-readable, auto-scaling B/KB/MB/GB). It reads **`VD:`** when
  the virtual card is being served.
- **`Speed:`**: live transfer speed in KB/s while a file operation is in progress.

### Which disk (`improved`)

SASD serves one disk. **WiRSaCtl → Setup → Disk** picks it: **Auto** (the SD card if one answers, otherwise
the virtual card), **SD card** (SASD does not start without one: "PLEASE INSERT SD CARD"), or **Virtual**
(the SD card is not touched). Mgr1 and other VFS apps then show that one card: the virtual card is
named `ESP32`; an SD card with no volume label shows as "Card 0". Every write is on flash before it is
acknowledged, so ending SASD with a reset does not lose files. The Palm cannot see the SD card and the
virtual card at the same time: one SASD slot, one serial line.

### Over a wireless serial link (NOWttl)

SASD is the least forgiving of the Palm modes on a link that adds delay and noise: the Palm driver gives an ack 210 ms, reads one byte per call, and stops the whole device on a failed read.
Over NOWttl (two ESP32-C3 units copying the TTL waveform by radio) that failed at once, and the stock driver cannot get a backup through. What WiRSa does for it (all invisible on a direct cable):

- **A packet that stops half way no longer holds the receiver.** The reference reader keeps waiting for the rest of a packet whose length it has read; when the link lost bytes, the Palm's resends were
  swallowed as that packet's missing bytes (a 46 s stall was measured). Now a packet with no byte for 100 ms is dropped, and the bytes of any dropped packet (CRC error or stall) except the first are looked at
  again, because a real packet can sit inside them. The code is `include/sasd_rx.h`, tested on the host by `test_host/sasd_rx_test.cpp`.
- **No return to 9600 inside SASD.** After `CMD_SET_BAUD` the port stays at the negotiated rate until the mode ends. An earlier version went back to 9600 when bytes arrived for a while without forming a
  packet (to catch a Palm that had restarted). A damaged data frame on a slow link met the same test, the port dropped to 9600 in the middle of a backup, the Palm unmounted the card, and it could not mount it again
  while the serial port stayed connected - so the rule is gone. The SASD timeout (Config > Exit Timers: no byte from the Palm for that long) is what hands the port back at 9600.
- **`SASD_FLAG_PIPE` (0x2000) in the INFO reply**: "a request and the data that goes with it may follow each other without waiting for the first ack". WiRSa handles packets in order, so it copes. The improved
  Palm driver (`hackmaster_y2035/apps/SASD`, see `SASD_driver.md` there) uses this only on a slow link; the stock driver ignores the bit.

The Palm side needs the improved driver for the rest (a longer ack wait that follows the link, reading in bulk, fewer round trips, errors instead of a fatal alert).

### Known limitation

Occasionally, a small amount of leftover serial traffic can bleed into the very next menu screen right
after exiting SASD mode (WiRSa waits briefly to let any in-flight traffic settle before switching back
to the normal menu baud, but this isn't a complete guarantee). If you see odd characters in the next
menu after backing out of SASD mode, it should clear up on its own with the next button press or
keystroke.

---

## HotSync mode

Lets a Palm perform a real HotSync directly against WiRSa — press the HotSync button on your Palm (or
tap the HotSync application) with WiRSa connected over serial, and WiRSa acts as the desktop cradle:
it negotiates the connection, then installs every `.PRC`/`.PDB` file it finds in two SD-card folders
onto the device, then ends the sync cleanly. Built from the same SASD hardware project's own reference
HotSync implementation, extended to support both `.PRC` **and** `.PDB` files (the original reference
only handled `.PRC`).

### Requirements

- A microSD card inserted in WiRSa, or, on `improved`, the virtual card (**Disk** in WiRSaCtl Setup: SD
  card, Virtual, or Auto = the SD card if one answers, else the virtual card). HotSync installs from
  whichever disk is selected.
- WiRSa's serial port physically connected to the Palm (cradle/cable).

### Folder layout (on the selected disk)

On the `improved` branch HotSync creates `/PALM`, `/PALM/PROGRAMS`, `INSTALL`, `SASD` and `INSTALLED` itself
on the disk it mounts, when they are missing, so there is somewhere to put files. Put apps there from
the Palm (Mgr1 over SASD), over FTP, or from a PC.

```
/PALM/PROGRAMS/SASD/       - "driver" files: installed once, left alone after that
/PALM/PROGRAMS/INSTALL/    - general app installs: installed, then moved out of the way
/PALM/PROGRAMS/INSTALLED/  - where INSTALL files land after a successful install (auto-created)
```

**`PALM/PROGRAMS/SASD/`** is for things like the `SASD.prc` driver itself, or anything else that only
ever needs to be installed once and then just work. `.PRC` files here are installed only if not
already present — if the same file is already on the Palm, HotSync skips it (logged, not an error).
`.PDB` files in this folder are special: they're **only installed if a `.PRC` in the same folder was
actually installed this run** — so a routine HotSync where nothing in this folder changed leaves any
SASD-folder `.PDB` files completely untouched, and when it does trigger, those `.PDB`s are installed
with a **full overwrite** (any existing copy is deleted and replaced), since they're meant to travel
together with a freshly-installed driver.

**`PALM/PROGRAMS/INSTALL/`** is for ordinary app installs. Files here are deleted-then-reinstalled
fresh on the Palm every time they're found — but once a file installs successfully, it's **moved to a
sibling `PALM/PROGRAMS/INSTALLED/` folder** (created automatically if it doesn't exist yet), so it
won't be found in `INSTALL` — and won't reinstall — on the next sync. To install an updated version of
something later, just drop the new file back into `INSTALL`.

Within either folder, `.PRC` files install before `.PDB` files. Hidden files and macOS resource-fork
sidecar files (anything starting with a `.`, like `._SomeApp.prc`) are automatically skipped — they
won't get installed by mistake.

### Using it

1. From WiRSa's main menu, select **`HotSync`** (or type `H` on the serial console).
2. The screen shows `State: Waiting` — WiRSa is now listening for a HotSync connection.
3. On the Palm, start a HotSync (press the HotSync button, or launch the HotSync app and tap the
   HotSync icon).
4. The screen progresses through `Connected` → `Syncing` (with each installed file's name scrolling
   through the detail log) → `Completed` (or `Init Complete`, see below).
5. If any `.PRC` in the `SASD` folder was installed this run, the Palm will automatically reset itself
   once the sync finishes, to load the newly-installed driver — this is normal, standard HotSync
   behavior for driver-type installs, not a malfunction.
6. Press the **Back** button any time to return to the main menu. It also returns by itself 15
   seconds after reaching `Completed`/`Init Complete`, or 45 seconds after entering if no HotSync
   starts (both can be changed in **Config → Exit Timers**).

On the `improved` branch, a short press of the **BOOT** button in any menu screen also starts
HotSync, and a second press while it is still `Waiting` leaves it. This is meant for a screenless
WiRSa and a Palm that has lost its apps: press BOOT, then HotSync to put them back. See "Controlling
WiRSa from the Palm" below.

### What the screen shows

- **Title bar**: `HotSync` with the current live baud rate and WiFi status in the top-right corner.
- **`State:`**: `Waiting` / `Connected` / `Syncing` / `Completed` / `Init Complete`.
  - `Init Complete` specifically means the `SASD` folder's own initial-driver-install path triggered
    this run (see the folder layout section above) — otherwise it just says `Completed`.
- **`Details:`**: a small 2-line scrolling log of what's currently happening — which folder is being
  checked, which file is being installed, and the final completion message.

### Notes

- The connection starts at a fixed 9600 baud (this is required by the HotSync protocol itself, not
  configurable) and negotiates up to whatever WiRSa's own configured baud rate allows once connected.
- No `.PRC`/`.PDB` files found in a folder is completely normal and not an error — HotSync will simply
  move on to the next folder.

---

## BitWarden Relay mode

Lets the dedicated **BitWarden** Palm app sync a Bitwarden or self-hosted Vaultwarden vault. WiRSa does
the WiFi/TLS/HTTP work and the PBKDF2 key derivation; the Palm only ever speaks a simple framed
protocol over serial and never opens a network connection itself.

### Requirements

- WiFi configured and reachable.
- A Bitwarden/Vaultwarden server reachable from WiRSa's network.
- The BitWarden Palm app installed, with server URL and email set in its own Preferences.
- The account's key derivation set to **PBKDF2** (Bitwarden's default). Accounts switched to
  **Argon2id are not supported yet** — the sync is refused with a message saying so.

### Using it

1. From WiRSa's main menu, select **`BitWarden Relay`**. The screen shows `Preparing...` briefly the
   first time after a boot, then `Waiting...` — WiRSa is now listening for the Palm app.
2. On the Palm, open BitWarden and tap **Sync Now**. The Palm prompts for the master password if none
   is cached.
3. The **Palm** shows the progress: `Connecting...` → `Negotiating rate...` → `Negotiated NNNNN baud`
   → `Syncing...` with a running item count. **WiRSa** shows `Syncing...` while it is working (this
   step can take a while — key derivation alone can run 1-3+ minutes depending on the vault's
   configured iteration count), then returns to `Waiting...`.
4. Press **Back** (or send `+++` over the USB serial console) to exit and return to the main menu.
   On `improved` it also returns by itself (Exit Timers, defaults: 15 seconds after a sync, or 45
   seconds after entering if no sync starts).

### What it shows

- **Title bar**: `Bitwarden` with the current live baud rate and WiFi status in the top-right corner.
- **Status line**: `Preparing...`, `Waiting...` or `Syncing...`. Progress detail and any error
  message appear on the Palm, not here.

### Notes

- **Baud rate is negotiated automatically**, not manually matched: every session starts at a fixed 9600
  baud, the Palm proposes its own configured target rate, WiRSa caps it to its own configured maximum,
  and both switch to whichever is lower. The title bar's baud readout always reflects the live rate.
- The negotiated rate reverts back to 9600 once the sync session ends (or after about 15 seconds of no
  further activity, if the Palm goes silent mid-negotiation) — so the next sync always starts from a
  known-good rate on both ends, with nothing to configure by hand.
- **The serial link is encrypted.** Every frame between the Palm and WiRSa is encrypted (a key
  exchange at the start of each session, then AES), so the master password and vault items never
  cross the cradle cable in the clear. There is nothing to set up. The first time a relay mode is
  entered after WiRSa boots, the status briefly shows `Preparing...` while WiRSa makes its key.
  There is no unencrypted fallback: if the Palm app shows **"Serial link cannot be secured!"**, the
  WiRSa firmware and the Palm app are from different versions — update both.

---

## CalDAV Relay mode

Lets the dedicated **CalDAV Sync** Palm app sync CalDAV, iCloud, Google Calendar, or plain ICS-feed
calendars into Datebook35. Up to 8 independently configured calendars ("slots"); WiRSa does the
WiFi/TLS/HTTP/iCalendar parsing work, the Palm only ever speaks a simple framed protocol over serial.

### Requirements

- WiFi configured and reachable.
- The calendar server(s) reachable from WiRSa's network.
- The CalDAV Sync Palm app installed, with at least one slot configured (server details + credentials).
- **For Google Calendar slots only**: a Google Cloud OAuth 2.0 Client ID **and Client Secret** (type
  "Desktop app") entered on the Palm itself, per slot — see "Google Calendar sign-in" below. **WiRSa
  never holds either of these, or anything else Google-account-identifying, resident** — both are sent
  fresh from the Palm with every sync, same as a CalDAV/iCloud password.

### Which calendar type should I use?

For **Google Calendar specifically**, use **ICS Feed** unless you have a real reason not to
(see below) — it's a single URL, no Google Cloud project, no OAuth sign-in dance, no Client
ID/Secret to keep in sync between two places. The tradeoff: Google refreshes that feed on its
own schedule (commonly every few hours), not instantly, and it's a read-only, one-way sync
either way (this app doesn't write events back for any calendar type). If you need closer-to-
live sync, use the Google OAuth flow instead — see "Google Calendar sign-in" below.

For a self-hosted or standards-compliant CalDAV server (Nextcloud, Radicale, Fastmail, etc.) or
iCloud, use the **CalDAV** or **iCloud** slot type respectively — neither of those offers a
plain ICS-feed shortcut the way Google does.

### Field reference — where each value comes from

Every field below is entered directly on the Palm, in CalDAV Sync's slot editor (tap a field to
edit it inline, or tap **Edit** on a long field — Feed URL, Client ID, Client Secret, Calendar
ID — to open the full-screen text editor). Nothing here is WiRSa-specific configuration.

**ICS Feed** (recommended for Google Calendar — see above)

| Field | What it is | Where to find it |
|---|---|---|
| URL | A private, unauthenticated `.ics` link for the calendar | **Google Calendar**: on the web, click the gear icon → *Settings* → pick the calendar under *Settings for my calendars* in the left column → scroll to *Integrate calendar* → copy **Secret address in iCal format**. **Other providers**: look for "iCal", "ICS", or "Subscribe" under that calendar's sharing/export settings — the link typically ends in `.ics`. |

Treat this URL as a bearer secret — anyone who has it can read the calendar without signing in
anywhere. Don't post it publicly or paste it somewhere it could leak.

**CalDAV** (generic — any standards-compliant CalDAV server that isn't Google or iCloud)

| Field | What it is | Where to find it |
|---|---|---|
| Server | The CalDAV base URL for your account, including scheme (`https://...`) | Given by your provider's own CalDAV documentation/settings page — often ends in something like `/dav/` or `/remote.php/dav/`. |
| Path | The specific calendar's path on that server | Provider-specific — some show it directly in that calendar's own settings/properties; others require browsing the CalDAV collection from the Server URL above. |
| User | Your username on that server | Your account login for that service. |
| Passwd | Your password | If the server has 2FA enabled, generate an **app-specific password** for it instead — a normal password won't work with 2FA on. |

**iCloud**

| Field | What it is | Where to find it |
|---|---|---|
| AppleID | Your Apple ID email address | — |
| Passwd | An **app-specific password** | Apple ID account page → *Sign-In and Security* → *App-Specific Passwords*. iCloud's CalDAV endpoint always requires this — your normal Apple ID password never works here, 2FA or not. |
| Host | Your iCloud CalDAV server ("shard") | Discovered via a one-time lookup against Apple's CalDAV service (see below) — it's account-specific, not a fixed hostname. |
| DSID | Your account's numeric CalDAV principal ID | Same lookup as Host. |
| Cal UUID | The specific calendar's own identifier under that principal | Same lookup as Host, one step further. |

Apple doesn't expose Host/DSID/Cal UUID anywhere in iCloud.com's own UI — they're only visible
via CalDAV's own discovery protocol. A one-time `curl` session finds all three (run these from
any computer, filling in your own Apple ID and the app-specific password from above):

```
curl -v -u "you@example.com:app-specific-password" https://caldav.icloud.com/
```

The response redirects to your real shard host (e.g. `pNN-caldav.icloud.com`) — that's **Host**.
Then, against that shard host:

```
curl -u "you@example.com:app-specific-password" -X PROPFIND https://pNN-caldav.icloud.com/ \
  -H "Depth: 0" -H "Content-Type: application/xml" \
  --data '<propfind xmlns="DAV:"><prop><current-user-principal/></prop></propfind>'
```

The returned `<href>` looks like `/123456789/principal/` — the number is your **DSID**. Finally,
a PROPFIND for `calendar-home-set` against that principal URL, then one more against the
returned calendar-home collection, lists your actual calendars — each one's own path segment
(a UUID-looking string) under `.../calendars/` is the **Cal UUID** for that specific calendar.

### Google Calendar sign-in (advanced/optional — see "Which calendar type should I use?" above)

Google Calendar authenticates differently from the other three types — a one-time OAuth sign-in instead
of a stored username/password. Every credential involved lives on the Palm (Sec35-encrypted where it's a
real secret) or in your own `google_auth.py` copy — nothing is ever stored on WiRSa itself:

1. **One-time, per Google Cloud project**: create a Google Cloud project, enable the Calendar API, and
   create an OAuth 2.0 Client ID of type **Desktop app**. Google's own docs describe the client secret as
   optional for this client type, but its token endpoint rejects requests without one in practice
   (`client_secret is missing`) — so note both the **Client ID** and **Client Secret** it gives you. Set
   the OAuth consent screen to **"In production"** (not "Testing") — otherwise Google silently expires
   the sign-in after 7 days. If you see an "unverified app"/"has not completed the Google verification
   process" screen during sign-in, add your own Google account under **Test users** on the consent
   screen, then click through **Advanced → Go to [app] (unsafe)** when it appears — normal for a personal,
   unreviewed app, not a real blocker.
2. **Per calendar slot, on the Palm**: in CalDAV Sync, set a slot's type to Google, enter the **Calendar
   ID** (`primary` for your main calendar, or a specific calendar's ID from Google Calendar's own
   settings), the **Client ID**, and the **Client Secret** from step 1 (tap any of the three fields to
   open the full-screen text editor — all can be long strings; the Client Secret field is labelled
   **Secret**). Also paste that same Client ID and Client Secret into the `GOOGLE_CLIENT_ID`/
   `GOOGLE_CLIENT_SECRET` constants of `google_auth.py`, once, on your computer. That script ships
   with the CalDAV Sync Palm app's sources (`apps/caldav_sync/tools/` in the Palm apps repository),
   not with this firmware.
3. Tap **Sign in with Google**. The Palm shows WiRSa's IP address and a 6-digit PIN, and the exact
   command to run. On a computer with Python 3 and a browser, run
   `python3 google_auth.py --ip <ip> --pin <pin>` from that `tools/` folder — it opens your browser
   for Google sign-in, then sends the resulting refresh token to WiRSa, which relays it straight to the
   Palm over serial and discards its own copy immediately. The PIN and listener are single-use and expire
   after 5 minutes.
4. Once signed in, that slot syncs like any other — **Sync Now**/**Sync All** work identically across all
   four calendar types. The Calendar ID, Client ID, and Client Secret are all sent fresh from the Palm
   with every `SYNC_REQUEST`, so WiRSa only ever sees them in passing, during an active sync.

### Using it

1. From WiRSa's main menu, select **`C[A]lDAV Relay`** (the brackets mark its serial shortcut key).
   The screen shows `Preparing...` briefly the first time after a boot, then `Waiting...`.
2. On the Palm, open CalDAV Sync and tap **Sync** (one calendar) or use **Record → Sync All** (every
   configured, non-skipped slot in one run).
3. The **Palm** shows the progress the same way as BitWarden: `Connecting...` → `Negotiating rate...`
   → `Negotiated NNNNN baud` → syncing. **WiRSa** shows `Connecting...` → `Syncing...` →
   `Completed!` for each calendar, then returns to `Waiting...`.
4. Press **Back** (or `+++`) to exit and return to the main menu. On `improved` it also returns by
   itself, as BitWarden Relay does (Exit Timers).

### What it shows

- **Title bar**: `CalDAV` with the current live baud rate and WiFi status in the top-right corner.
- **Status line**: `Preparing...`, `Waiting...`, `Connecting...`, `Syncing...`, `Completed!`, or
  `Google sign-in...` while a Google sign-in is waiting for the computer. Error messages appear on
  the Palm, not here.

### Notes

- **Baud negotiation** works exactly as described for BitWarden Relay above — automatic, starts at
  9600, caps to WiRSa's own configured maximum.
- **The serial link is encrypted** the same way as BitWarden Relay's (see its Notes above), including
  the `Preparing...` status and the "Serial link cannot be secured!" version-mismatch message.
- **Sync All keeps one negotiated connection open across every calendar in the run** instead of
  renegotiating per calendar — the rate is only released (and WiRSa reverts to 9600) after the actual
  last calendar in the batch finishes, or after about 15 seconds of inactivity if the Palm goes silent.

---

### Non-Latin text on the Palm (`improved` only)

Calendars, contacts and BitWarden hand WiRSa UTF-8; the Palm only shows Palm Latin (Windows-1252). WiRSa converts
the **display text** on the way to the Palm - CalDAV event titles, locations and notes, CardDAV contacts, and
BitWarden item names, notes, folder and organisation names - and never touches usernames, passwords, URIs, TOTP seeds
or card and identity fields:

- accented Latin letters that Palm Latin has stay as they are, the others lose the accent (ł -> l);
- **Chinese** becomes pinyin without tones, one capitalised syllable each: 会议室 -> `Hui Yi Shi`;
- **Japanese kana** becomes romaji (さくら -> `sakura`, ラーメン -> `raamen`), kanji get their Chinese reading;
- **Korean Hangul** becomes Revised Romanization, syllable by syllable (한국어 -> `hangukeo`);
- full-width letters and digits become ASCII, CJK punctuation becomes `. , " [ ]`;
- emoji and zero-width characters are dropped; anything else (Arabic, Cyrillic, ...) shows as `?`.

Limits: a character with several readings gets its most common one (重庆 comes out `Zhong Qing`, not
`Chong Qing`); the text is longer than the original (about five letters per Chinese character), so the Palm's
field lengths cut a long title sooner. The tables (about 60 KB of flash) are generated from the Unicode Unihan
database by `Firmware/tools/gen_translit.py` and committed; the code is `translit.cpp`, tested by
`test_host/carddav_card_test.cpp`.

## Mesh Relay mode (Meshtastic) (`improved`)

Links the Palm app **Meshtastic** (`meshtastic.prc`) to a Meshtastic node. The Palm cannot do Bluetooth LE or
Meshtastic's protocol, so WiRSa holds the Bluetooth connection to the paired node, keeps the node list, channels and
messages, and talks to the Palm over the serial line (framed and encrypted like the BitWarden and CalDAV relays).

### Requirements

- A Meshtastic node in range with Bluetooth on (random PIN, fixed PIN or no PIN all work).
- The Palm app; WiRSa built with `WIRSA_FEATURE_MESH` (default 1; it needs the NimBLE-Arduino library).

### Using it

1. From WiRSa's main menu select **`Mesh[T]astic`**, or let the Palm app start it (Preferences > Screenless Mode, or
   WiRSaCtl).
2. In the Palm app use **Settings > Device...**: Scan, pick the node, Pair (a PIN keypad opens if the node asks). WiRSa remembers the node and reconnects by itself.
   Up to four nodes can be paired (Pair adds one; Unpair keeps it as an archive with its texts, or deletes it with its SD folder); the **Paired** popup picks which one the Palm shows and connects to.
3. Chat from the Palm. Mode *Session* ends when the app closes; mode *Background* (for one node only, marked `[B]`) makes WiRSa check that node by itself every
   1, 5, 15, 30 or 60 minutes so messages wait for the Palm; **SD Flush** chooses when a sleeping WiRSa writes its RTC ring to the card (every check, 3, 5 or 10 checks, when full). Interval and flush are kept per node, and a new pair starts as Session. In both modes the app closing takes WiRSa back to the main
   menu; with *Background* the checks go on from there, also while the board sleeps (below).

### What it shows

`Mesh Relay Active - waiting for the Palm` and the node's name and link state on the screen; the USB log
(Log level Info or Verbose) shows `Mesh BLE: ready`, the speed agreed with the Palm and `Mesh: text from <node> on channel <n>`.

### Notes

- The mode goes back to the main menu by itself by the Exit Timers (a quiet Palm is probed first).
- **Background checks and sleep:** with *Background* on and the **Sleep** setting (WiRSaControl Setup: Off, 1, 3, 5, 15
  minutes) not Off, WiRSa sleeps when the main menu has had no Palm traffic for that long and wakes by a timer for each check
  (about 10 s awake, Bluetooth only, 80 MHz, WiFi off; only the blue LED shows the Bluetooth link) and by the Palm line (a normal start at full
  speed, awake for the whole Sleep time). About 28 mA asleep at 5 V against 178 mA awake. During the ~10 s of a
  check the board does not listen to the Palm: WiRSaCtl's Find and Refresh just need another try.
- **Where texts wait:** in WiRSa's memory (200), in a 4 KB ring in RTC memory while asleep (lost on a reset), and on the SD
  card in `/Meshtastic/<node number>/` (`mesh.dat`, `mesh.old`, plain text, 1 to 2 MB, one folder per node). The Palm's **SD History** preference decides whether
  the card is emptied once the Palm has everything (the default) or keeps a history. Without a card the oldest ring entries go
  when it is full. Details and measurements: `docs/Mesh-Palm.md`.
- **Keep-alive:** the Palm app sends a small PING every 5 s while connected; WiRSa leaves the mode only when it has heard nothing for the *After session* time and a probe then also goes unanswered. WiRSa sends BYE to a connected Palm when the mode ends.
- **After a boot** the first background check waits 1 minute.
- **Delivery marks and errors:** the Palm marks each sent message (sending, sent, relayed, delivered, exchanging keys, failed); a direct message that fails for a missing public key is retried once after a node info exchange. The error list is in `docs/Mesh-Palm.md`.
- **Nodes:** WiRSa holds up to 500 (PSRAM) and sends them sorted in batches; the Palm asks for a node's full details when its card opens.
- Speed: 57600 or 19200 is the safe choice; some Palms' 38400 serial timing is irregular.
- Frames and the wire: `docs/protocol/Protocol-wire.md`; behaviour and details: `docs/Mesh-Palm.md`; the Palm app: `apps/meshtastic_client.md` in the palmos_sdk repository.

## FTP Server mode

Runs a background FTP server that serves WiRSa's SD card (and, on `improved`, the virtual card) — upload files to it from a desktop (or from
the Palm itself over SLIP, via an FTP client app like lazyFTP), then HotSync or SASD them onto the
device without a cable swap. Replaces MODEM mode's old menu slot.

### Requirements

- A microSD card inserted in WiRSa, or the virtual card (`improved`; the FTP server does not need the
  SD card).
- WiFi configured (WiFi Setup) for normal (STA) mode, or an AP SSID/password configured (AP Setup) to
  run the server as its own standalone WiFi network.

### Using it

1. Optionally, set up FTP credentials first: **Config → FTP Setup** (username/password — leave both
   blank to allow anonymous login) and, if you plan to use AP mode, **Config → AP Setup**
   (SSID/password). Both can be entered via the OLED+buttons or typed over the serial console.
2. From WiRSa's main menu, select **`FTP Server`**, then **Start** (joins your existing WiFi network —
   if it isn't already connected, you'll briefly see "Waiting for WiFi...", see "Background WiFi & the
   status icon" above) or **Start (AP Mode)** (WiRSa broadcasts its own WiFi network — useful with no
   existing WiFi available, e.g. in the field). **Status** shows whether the server is currently
   running.
3. Connect with any FTP client to WiRSa's IP address — browse, upload, download, delete, same as any
   FTP server. On your own WiFi network the address is listed under **Utilities → Information**; in AP
   mode it is the access point's own address (192.168.4.1 unless your ESP32 core differs). An AP
   password shorter than 8 characters starts an **open** network, with a warning on screen. It keeps running in the background regardless of
   which other menu you're in.
4. **Stop** from the same menu when done.

### What the client sees (`improved`)

The root `/` lists two folders: **`SDCard`** (only when a card has mounted) and **`ESP32`** (the virtual
card, set up like in any other mode: formatted on first use). Each shows that disk's own files. Without
an SD card only `ESP32` is listed. Nothing can be created, renamed or deleted at the root, and a file
cannot be renamed from one disk to the other (copy it). The Disk setting does not apply: both are
always shown. The server starts as long as one of them is available. A file put into
`/ESP32/PALM/PROGRAMS/INSTALL` is installed by HotSync when Disk is Virtual.

### Interaction with other modes

- **Cannot run at the same time as SASD SDCard mode** (the two touch the SD card in incompatible ways)
  — whichever is active blocks the other from starting, with a clear on-screen message. Stop one to
  start the other.
- **Can stay running during HotSync mode** — nothing blocks the two from being active together, so
  you can upload a file via FTP and then HotSync it onto the Palm without stopping the FTP server
  first. HotSync does write to the disk (it moves each installed file from `INSTALL` to `INSTALLED`),
  so don't upload into `INSTALL` while a sync is actually in progress. The virtual card is shared
  between them: it is released when the last of the two stops.

---

## SLIP Gateway

Selecting **`SLIP Gateway`** from the main menu starts the gateway immediately and shows its status
screen (gateway, client and WiFi addresses, connection and packet counts). Press **Back** to stop it
and return to the main menu; the other buttons just repaint the status screen. If WiFi isn't already
connected, you'll see "Waiting for WiFi..." — see "Background WiFi & the status icon" above.

The gateway has no menu of its own. Its addresses are set under **Config → SLIP Setup**: Gateway IP,
Client IP and DNS Server, entered with the buttons (Up/Down cycle through digits and the dot, Enter
adds, Back deletes, hold Enter to accept, hold Back to cancel) or typed over serial. An address that
isn't valid, or a client address equal to the gateway's, is refused; cancelling any of the three
prompts saves nothing. The defaults are 192.168.7.1 / 192.168.7.2 / 8.8.8.8.

Port forwarding and the serial "View Stats" dump have been removed, along with the serial-only
address wizard that Config → SLIP Setup replaces.

**Speed** (`improved`): SLIP Gateway runs at whatever rate the main menu is using when it starts:
9600 after a boot, or the speed a Palm's WiRSaCtl moved it to (see "Baud rate" below). Set the
Palm's network connection to that same speed. To start SLIP at a faster rate, open WiRSaCtl first,
or start SLIP from WiRSaCtl's Modes screen.

**SASD timeout** (`improved`): SASD stops and WiRSa returns to the main menu after 3, 5, 10 or 15 minutes (default 3) with
nothing heard from the Palm. The Palm's SASD driver keeps the serial port while it works, so WiRSaCtl cannot end SASD; this timer (or Back /
`+++` / a reset) does. After it, the Palm's next card access gives its "no card" alert and SASD is started again. Set in **Config → Exit Timers**
(fourth prompt) or WiRSaCtl (Setup → Exit Timers...).

**SLIP timeout** (`improved`): the gateway stops and WiRSa returns to the main menu after 1, 2, 3 or
5 minutes (default 5) with no traffic either way. Traffic WiRSa sends to the Palm counts, so a Palm
waiting on a slow server is not cut off. The time counts from when the gateway starts, so a Palm
that never connects also times out. Set it in **Config → Exit Timers** (third prompt) or in WiRSaCtl.

### Reliability improvements

The SLIP↔WiFi gateway has been substantially hardened since the original WiRSa release:

- **Checksum validation** on all incoming IP/TCP/UDP traffic from the serial side — corrupted packets
  are now detected and dropped instead of being silently forwarded and potentially corrupting a
  transfer.
- **More robust TCP handling** for out-of-order/duplicate acknowledgments and sequence numbers, which
  used to be able to corrupt an in-progress transfer under some real-world network conditions.
- **Diagnostic logging** available via the USB debug console for troubleshooting connection issues.
- **A Palm connecting through the gateway to a service running on WiRSa itself now works** (e.g. an FTP
  client on the Palm talking to WiRSa's own FTP Server mode above) — this used to fail silently because
  the connection had to leave over the WiFi radio and loop back, which most WiFi networks don't support
  for a device talking to itself; it's now detected and routed internally instead.

If you were seeing occasional file corruption during large transfers (e.g. FTP) over a SLIP-connected
Palm, these fixes directly address that.

**Fixed (2026-10-26): a server that speaks first.** A server that sent data the moment the connection opened (an FTP
server's `220` greeting, for example) had its first byte dropped by the gateway, so the Palm saw `20 ...` and gave up. The
gateway counted the opening handshake as a byte of data in flight. Servers that wait for the client were not affected.

### Speed: network HotSync at 57600 and 115200 (`improved`)

The gateway runs at the speed the Palm's login script sets with `SETBAUD` / `BR`. A network HotSync
used to work only up to 38400 although the serial port itself is fine at 115200. The Palm's network
software has no flow control from WiRSa, and at the higher rates it drops bytes when frames arrive back to
back, so segments were lost and the sync timed out. The gateway now:

- **Paces what it sends to the Palm, above 57600 only.** (At 57600 and below the Palm keeps up, and a timeout there is usually a damaged
  acknowledgement on the Palm's own line, which slowing down does not cure: paced, 57600 took 60 s or more against 40 s unpaced.) Frames go out in 64-byte pieces spaced to a target rate that starts at the
  line rate, slows by a quarter each time the Palm fails to acknowledge a segment, and speeds up again after a clean
  run (floor: a fifth of the line rate). It finds the fastest rate this Palm can take at the chosen speed.
- **Never waits for the server while the Palm is sending.** What the Palm sends is queued in PSRAM (32 KB per
  connection), acknowledged at once, and handed to the server without blocking, with a window that follows the
  free space (up to 8 KB). A write that only partly went is acknowledged only as far as it went.
- Resends sooner (a timeout that follows the line speed, and fast retransmit after three duplicate ACKs), turns off
  delayed sending on the server sockets, redraws the OLED only when the line is quiet, and at 115200 wakes the UART
  reader earlier.

Seen on hardware (Palm III / V): HotSync completes at 57600 and at 115200. At 115200 the pace settles near 25 to 50 kbit/s
(about 3 to 5 KB/s toward the Palm), so the sync is limited by what the Palm can receive, not by the line.

**Which speed to use for a network HotSync.** Timed on the same Palm over the same job (the data part of the log, to the
nearest 10 s): **57600: 40 s** (about 90 KB toward the Palm, 7 resends). **115200 with pacing: 50 s** (94 KB, 8 resends); a second
build with smaller segments and a remembered pace was slower, 81 s (16 resends), and was not kept. 115200 without pacing did not finish.
**19200: 70 s**, no resends. **9600: fails** - about one Palm frame in five arrives damaged (the same with the firmware from before
these changes), so use 19200 or higher. In short, **57600 is the fastest and the one to use**: the Palm's network stack can take only
about 3 to 5 KB/s however fast the line is, so 115200 gains nothing.

With the log level at Info the gateway writes a **`SLIP stats`** pair of lines every 10 seconds while traffic flows and
when it stops: line speed and the current pace (with how many times it slowed down), frames and bytes each way,
checksum and sequence drops, resends and duplicate ACKs, the size of the queue to the server, UART overflow counters,
the largest serial backlog and the longest pause of the main loop. A few damaged frames from the Palm at 115200 are
normal; the Palm sends them again.

---

## Virtual card (`improved` only)

12 MB of the ESP32's 16 MB flash is set aside as a card that is always there. The Palm sees it
through SASD as a normal VFS card named **ESP32**, HotSync can install from it, and the FTP server
shows it next to the SD card. The SD card is not needed for any of that.

### Requirements

- An ESP32-S3 with 16 MB of flash (the LilyGO T7-S3) and the `Firmware/partitions_vdisk.csv` partition
  table: `min_spiffs.csv`'s two 1.9 MB app slots plus a 12 MB `vdisk` partition at 0x400000. Both environments of
  `Firmware/platformio.ini` set `board_build.partitions = partitions_vdisk.csv` (and `board_upload.flash_size =
  16MB`). An ordinary upload rewrites the partition table; settings and both app slots keep their place.
  Without it the build still works and the System page says "Virtual card: none".

### Using it

1. **WiRSaCtl → Setup → Disk**: Auto, SD card or Virtual (see SASD and HotSync above).
2. The first time it is used (SASD, HotSync or FTP) a blank partition is formatted: FAT16, one
   partition, about 12 MB, volume name `ESP32`. A card that is already formatted is only given the name.
3. Typical use: Disk = Virtual, start SASD, and with Mgr1 copy apps into `/PALM/PROGRAMS/INSTALL` (the
   folders are created by HotSync when you start it once, or make them in Mgr1); then start HotSync and
   the Palm installs them. The `SASD` folder holds the driver, so a Palm that lost its apps can get
   the driver back from the virtual card with HotSync alone.
4. The System page (Help → System) shows the virtual card's size and whether it is formatted.

### Limits and care

- One disk at a time over SASD: the Palm's SASD driver registers a single slot and keeps the one serial
  port open, so the SD card and the virtual card cannot both appear. Two partitions on one slot do not
  mount as two volumes, and two copies of the driver cannot share the line. Switch the Disk setting (and
  start SASD again) to change disks.
- 12 MB, with 2 KB clusters. The FAT16 root directory holds only about 127 long file names, so keep
  files in folders (HotSync's folders are).
- It is flash: writes are collected in memory and written to flash when each file or transfer is
  closed, so end a session normally. A reset in the middle of a transfer can leave a partial file, as
  with a real card. Nothing is stored in WiRSa's flash apart from what you put on the card.
- To wipe it, erase the partition from a PC (`esptool.py erase_region 0x400000 0xC00000`); it is
  formatted again the next time it is used.

---

## Logs and the System page (`improved` only)

WiRSa keeps a leveled log. The level is set in **WiRSaCtl → Setup → Log level** (**Off, Warn, Info,
Verbose**; default Info). A line goes to the USB serial console if its level is at or below the setting.
Warnings and errors are also kept in a small **session log** even with USB output off. The session log
lives in RAM that survives a software reset, a crash or a watchdog reset, but not loss of power; nothing
is written to flash or the SD card. Read it on the Palm with **Help → Show Logs** (newest at the
bottom; Refresh and Clear).

Lines worth knowing:

- **SD card:** `SD: card mounted`, `SD: card mounted only at a reduced clock (4 MHz) - check the wiring`
  (the card failed at 20 MHz and WiRSa retried slower), `SD: no card (or it did not answer at 20 MHz or
  4 MHz)`. They are logged when the state changes, not on every try.
- **Disks:** `SASD: serving the virtual card (12288 KB) - no SD card` or `- chosen in Setup`,
  `SASD: not started - no SD card`, `HotSync: installing from the virtual card (ESP32)` or `the SD
  card`, `FTP: serving SDCard ESP32`, `VDISK: FatFs mount failed (...)`.
- **BitWarden:** `BW: synced N item(s), N folder(s), N org(s) to the Palm` appears only after the Palm
  has received everything. If it did not, `BW: sync incomplete - X of N item(s) reached the Palm`. An
  item whose notes are over 8192 encrypted bytes (about 6 KB of text) or malformed is sent with empty
  notes and logged: the Palm keeps only 512 bytes of notes anyway.
- **CalDAV:** `CalDAV: synced N event(s) to the Palm` is logged only once the Palm has taken them,
  otherwise `CalDAV: sync incomplete - X of N event(s) reached the Palm`. The earlier "fetched" line is
  Verbose.

**Help → System** shows the chip and revision, CPU speed and core temperature, flash size, mode and
speed, firmware size, internal RAM and PSRAM (used, free, lowest free, largest block), uptime, the last
reset reason, the ESP-IDF version, the virtual card, and the partition table.

---

## Controlling WiRSa from the Palm (`improved` only)

### WiRSaCtl Palm app

The **WiRSaCtl** app (`wirsacontrol.prc`, "WiRSa-Palm Control", a separate Palm app) talks to
WiRSa over the normal serial line. WiRSa
must be at its main menu (or in HotSync, CalDAV Relay or BitWarden Relay, which answer it too). The
app finds WiRSa's baud rate by itself, and moves WiRSa's menu to this Palm's speed (see "Baud rate"
below).

- **Main screen:** current mode, firmware, WiFi network and signal, IP address, the disk and FTP
  state. It refreshes every 15 seconds. The disk row follows the Disk setting: **`SD:`** with the card's
  used/total space (or "Not present"); **`V. SD:`** with the virtual card's used/total (or "not
  formatted", or "Not present" if the build has no virtual card); with Auto, **`SD:`** and either
  "Card 1.2GB/14.8GB", "Virt 0.000MB/12.0MB", "Not found" or "Unknown" (not probed yet).
- **Modes:** start HotSync, SASD, SLIP Gateway, CalDAV Relay, BitWarden Relay or Mesh Relay, or go back to the
  main menu; start or stop the FTP server. SASD and SLIP can't be left from the Palm: reset WiRSa
  to get back.
- **WiFi:** scan, pick a network, enter the password, connect.
- **Setup:** maximum baud rate ("Max baud"), Log level (Off / Warn / Info / Verbose), screen
  orientation (hidden on a screenless WiRSa), **Disk** (Auto / SD card / Virtual; used by SASD and
  HotSync), and four buttons: **FTP Setup...** (user, password), **AP Setup...** (access-point name,
  password), **SLIP Address...** and **Exit Timers...**. A blank password field means "keep the
  current one".
- **Options menu:** Find WiRSa, Reboot WiRSa, Factory Reset (asks first), Preferences (this Palm's
  "Pref Baud").
- **Help menu:** **System** (chip, memory, flash and partitions), **Show Logs** (WiRSa's session log)
  and About. See "Logs and the System page".
- SASD and HotSync are not started from the Palm when the last status showed no disk for the selected
  Disk setting ("The last status showed no disk").

Passwords (WiFi, FTP, access point) are sent encrypted, the same way the relay modes encrypt their
traffic.

**CalDAV Sync and BitWarden** have a **Screenless Mode** preference. When it is on, the app puts
WiRSa into its relay mode before a sync and sends it back to the main menu afterwards, so nothing
needs to be pressed on WiRSa.

How it works on the wire: each command is the byte 0x16 followed by a small checked frame. Menu
letters typed on the Palm line still work, but a letter only counts if nothing else arrives within
100 ms before and after it (a CR/LF after it is fine). This stops stray bytes, such as an SASD wake
ping, from starting a mode. Text entry (WiFi passwords, addresses) is not affected, so pasting
works.

### Baud rate: one WiRSa, Palms of different speeds

Palms differ in how fast their serial port can go (one might manage 115200, an older one only
57600), but WiRSa has one Palm line. So:

- **After every boot the main menu runs at 9600**, which every Palm can use. The rate in the
  menu's top-right corner is always the rate the menu is using right now.
- **Each Palm sets its own speed in WiRSaCtl: Options → Preferences → "Pref Baud"** (default 9600 =
  stay at 9600). It is stored on the Palm. When WiRSaCtl finds WiRSa's menu at 9600, it asks WiRSa
  to switch the menu to that speed, and WiRSa does, at any standard rate the Palm asks for. If the
  Palm can't actually talk at that speed, WiRSaCtl drops back to 9600 and won't ask for it again
  until you change Pref Baud or use Find WiRSa.
- **The switch lasts until WiRSa restarts.** Nothing is saved on WiRSa.
- **Swapping Palms works without a restart.** While the menu runs faster than 9600, WiRSa also
  listens at 9600 on the same line. When the next Palm talks at 9600, the menu drops back to 9600,
  and that Palm's WiRSaCtl then moves it to its own speed.
- **Config → Baud Rate (Max)** (WiRSaCtl: Setup → "Max baud") is the fastest rate HotSync, SASD,
  CalDAV Relay and BitWarden Relay may agree on. Each sync still starts at 9600 and settles on the
  lower of the Palm app's own rate and this maximum. Changing it doesn't move the menu.
- **Utilities → Information** shows "Baud: 115200 now 57600" when the menu runs at a different
  rate from the maximum.
- **What only works after WiRSaCtl has run:** anything that talks to the menu at a fixed rate
  without asking, such as a network login script's `S` or a terminal app. Right after a boot only
  9600 works for them; once WiRSaCtl has moved the menu, the Palm's own speed does.

How it works: a second, receive-only serial receiver in the ESP32 (UART1) reads the same Palm
receive pin as the main one, at 9600, while the menu runs faster. WiRSaCtl's request is a new
control command (SPEED). Turn the whole feature off with `WIRSA_FEATURE_DUAL_BAUD 0` in
`globals.h`: the menu then runs at the configured rate, as before.

### Setting the speed from a login script (SETBAUD / BR)

A Palm network login script runs at the speed set for its connection, which can't be changed from
the script. Right after a boot the menu is at 9600, so a script at 115200 has been talking to
nobody. In the menu screens WiRSa now answers a two-line handshake:

    Send:        SETBAUD
    Wait For:    OK
    Send:        BR115200
    Wait For:    OK
    Send:        S

(adjust to the script editor's own wording; `S` is just the usual next step, the SLIP key.)

1. **`SETBAUD`** - if the menu already hears the Palm (9600, or the menu's current speed) the
   line is read as text. Otherwise WiRSa measures the speed: the first character `S` makes eight
   edges on the line at fixed bit times (0, 1, 3, 5, 6, 7, 8, 9), so a least-squares fit through them
   gives the bit time, and the nearest of 9600, 14400, 19200, 28800, 38400, 57600 and 115200 (within
   8%) is the speed. A Palm's serial port can put one of the eight edges up to a bit off, always the
   same one at a given speed (the sixth at 38400, the fourth at 28800), so the fit is also tried
   without each inner edge in turn. When the phrase is over the menu moves there. WiRSa answers `OK` at that speed.
2. **`BR<rate>`** - the UART now reads the Palm at the measured speed, so getting this line at all
   is the proof, and its number is the cross-check. Moved by the measurement: the number must equal
   the measured rate, then `OK`; otherwise `ERROR` and the menu goes back to 9600. Read as text at
   a speed the menu already hears: any of the rates above is accepted, answered `OK`, and the
   menu moves to it (for a terminal or Palm app that then changes its own speed).
   No `BR` line within 5 seconds of a measured speed change: back to 9600.

Why two lines: the phrase gives the measurement more than a single `AT` would (the first
character carries it; the rest only keeps the line busy), but the reading that matters is done by
the UART on the second line, at the new speed.

Things to know:
- **No CR needed.** Each line also ends when the Palm pauses (about 200 ms), so the script need not
  send one; a CR or LF still ends the line at once.
- **Everything is logged on the USB console** (whether or not USB debug is on), with the line WiRSa
  received: `SetBaud: measured 115200 baud ...`, `... BR line "BR115200" matches ... speed confirmed`,
  or why it failed - `wrong baud: the script says BR38400 but the line measured 115200 baud`, not a
  number, unsupported rate, or no BR line within 5 s. The Palm gets `ERROR` for the failures.
- It listens in the menu screens at any menu speed, so a script can ask for a speed other than the one
  WiRSaCtl moved the menu to (a Palm preferring 57600 whose script says `BR38400`). A different Palm
  still gets in at 9600 as before.
- **The script's speed is held, then given back.** After a confirmed `BR` the menu stays at the script's
  speed for its next step (the `S` key, a mode). When a mode ends and the menu returns, or the line has
  been quiet for 10 seconds, the menu goes back to the speed it had before the script
  (`SetBaud: script over - menu back to 57600 baud` on the USB console). Setting a speed from the
  Palm (WiRSaCtl SPEED, the Baud menu) cancels the pending return.
- The speeds a script may ask for are the Palm's: 9600, 14400, 19200, 28800, 38400, 57600, 115200.
  Max baud (Config → Baud Rate, WiRSaCtl Setup) offers the same seven.
- A lone `S` (the SLIP key) still works: it only counts as the phrase when an `E` follows within 30 ms.
- The speed measurement uses a pin interrupt on the Palm RX pin (GPIO 16) and the CPU cycle counter;
  on a quiet line it costs nothing. It was checked in a host test (every rate, +-2 us of interrupt
  jitter) but **not yet on the hardware**: turn it off with `WIRSA_FEATURE_AUTOBAUD 0` in
  `globals.h` if the menu ever misbehaves. Run with USB debug to see the
  `PalmCtl: autobaud burst -> <rate>` and `speed ... confirmed by BR line` lines.

### Exit Timers

HotSync, CalDAV Relay and BitWarden Relay go back to the main menu by themselves, however they were
started:

- **Unused mode exit**: no session starts within this many seconds of entering (default 45).
- **After session exit**: this many seconds after a session ended (default 15). A session ends at
  the Palm app's goodbye, when a relay hears nothing for 15 seconds, or when a HotSync finishes. A new session stops
  the countdown.

Both are in seconds, 0-255; 0 means never. Set them in **Config → Exit Timers** (key `E`), which
also asks for the SLIP and SASD timeouts, or in WiRSaCtl (Setup → Exit Timers...).

### BOOT button = HotSync

A short press of the ESP32's **BOOT** button (GPIO0) in any menu screen starts HotSync, just like
the `H` key; a second press while HotSync is still waiting leaves it. The red LED shows the
result: 3 slow blinks = HotSync started (or left), one long blink = not started (no disk: no SD card, or the Disk setting is SD card and none answers). The HotSync traffic then flickers green (serial) and red (SD card). Don't hold BOOT while WiRSa resets or powers up: that puts
the ESP32 into its firmware-download mode. Turn it off with `WIRSA_FEATURE_BOOT_HOTSYNC 0` in
`globals.h`.

### Status LED

The colour LED shows what the radios and the ports are doing, and nothing else: no mode has a state of its own. HotSync, SASD, SLIP, CalDAV, BitWarden and Mesh show only as the traffic they cause. Three kinds of LED are built; the pin and the levels belong to the board, in `globals.h`.

- **Single LED.** `LED_PIN` in `globals.h` (GPIO2 by default, active-low, full brightness). On the LilyGO T7-S3 build (`-DWIRSA_BOARD_T7S3`) it is the on-board LED on GPIO17, active-high, dimmed to `WIRSA_LED_BRIGHTNESS` 5 %, both set in the T7-S3 block of `globals.h`. It shows the WiFi state and flashes inverted for 30 ms on SD traffic, as it always did, beside the three LEDs below.
- **Three LEDs (T7-S3).** The `t7s3` environment adds `-DWIRSA_LED_DISCRETE`: D1 red on GPIO4, D2 green on GPIO7, D3 blue on GPIO8, each through a resistor to ground (active-high, `WIRSA_LED_RGB_ON`), at `WIRSA_LED_NEO_PCT` 5 %. In the deep sleep the three pins are driven low and held.
- **RGB LED (WS2812).** Add `-DWIRSA_LED_NEOPIXEL=<gpio>` to the build flags (GPIO 48 on the ESP32-S3 board with the OLED). It replaces the single LED. Level: `WIRSA_LED_NEO_PCT` (default 1 %, a WS2812 at full power is very bright). The T7-S3 build refuses the flag (`#error`): it has the three LEDs above.

| Colour | Steady | Flickers on |
|---|---|---|
| Blue | the WiFi is up (connected, or the access point) or the BLE link is up: steady with **Wifi Pulse** "Always On", otherwise see below | WiFi traffic of ours (unicast IP, not ARP, broadcast, multicast or mDNS) or BLE traffic (a short dark dip) |
| Green | off | Palm serial port traffic, in either direction |
| Red | off | SD card traffic |

Every flicker is 30 ms, followed by at least 30 ms without one, so heavy traffic shows as a steady fast flicker. The BOOT button (HotSync) blinks red, 1 s on and 1 s off, three times (once for no SD card). The deep sleep is all dark.

**Wifi Pulse** (WiRSaControl, Setup, below Sleep; `wifiPulse` in `settings.h`, default 30s) sets how blue shows that the WiFi or BLE is up. *Always On*: steady. *15s*, *30s*, *1min*: dark, with a 100 ms blip at that interval. *Never*: dark, no blip. With any setting but Always On, the traffic flicker is a blue flash on the dark LED. Whatever the setting, the changes are shown: while the WiFi connects, blue blinks slowly (1 s period, 30 s at most; if nothing is up by then: the drop blinks); when it comes up (or the BLE link), blue is steady for 30 s; when it is lost, blue blinks three times quickly.

How the traffic is seen: SD accesses call `sdActivity()`; the Palm UART calls are wrapped at link time (`-Wl,--wrap=uart_write_bytes` and `uart_read_bytes` in `platformio.ini`), so no call site is involved; the WiFi station and access point interfaces get a thin wrapper around their lwIP receive and send functions when they come up; BLE traffic is the frames to and from the Mesh node (`mesh_ble.cpp`). A SLIP session is serial traffic and, because each frame also crosses the WiFi, WiFi traffic: it flickers green and blue.
| Before deep sleep | dark | off |

The LED follows the WiFi events (connect, disconnect), not the display, so it works on a screenless build. When two pulses overlap the stronger one wins: mode started, then text, then frame, then SD. After leaving a Mesh session the LED returns to the WiFi colour.

### Screenless build

Set `WIRSA_SCREENLESS 1` in `globals.h` to build for hardware with no OLED and no buttons. The
display calls do nothing, the buttons always read as not pressed, the splash screen is skipped, and
prompts that would wait for a button give up instead. Everything is then done from the Palm:
WiRSaCtl, typed menu keys, or the BOOT button for HotSync. The control link itself can be turned
off with `WIRSA_FEATURE_PALMCTL 0`; the normal screen build has it on alongside the OLED and
buttons.

---

## Credits & references

- **This project** is a fork of [nullvalue0/WiRSa](https://github.com/nullvalue0/WiRSa) — see the main
  [README.md](../README.md) for hardware details, and [README-Original.md](README-Original.md) for the
  original upstream feature set (note: this firmware no longer includes the MODEM mode, AT commands
  or web config server described there — see "What changed" above).
- **FTP Server mode** is built on the vendored
  [MultiFTPServer](https://github.com/renzo-mischianti/MultiFTPServer) library.
- **CalDAV Relay mode** uses the vendored [uICAL](https://github.com/sourcesimian/uICAL) library for
  iCalendar/RRULE parsing and expansion.
- **SASD protocol and HotSync install client** are wire-compatible reimplementations based on
  [dmitry.gr's SASD project](https://dmitry.gr/?r=05.Projects&proj=32.%20SASD) — an excellent
  real-hardware reference for both the SD-card-over-serial protocol and an embedded HotSync host
  implementation.
- **HotSync protocol validation** cross-checked against [pilot-link](https://github.com/jichu4n/pilot-link),
  the long-standing open-source implementation of the real desktop-side HotSync stack.
