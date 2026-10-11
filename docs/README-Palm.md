# WiRSa — Palm OS Modes (SASD SDCard, HotSync, FTP Server, BitWarden & CalDAV Relay)

This fork of [WiRSa](https://github.com/nullvalue0/WiRSa) adds several modes purpose-built for Palm OS
devices: **SASD SDCard mode** lets a Palm mount WiRSa's SD card as a native, always-available VFS
volume, and **HotSync mode** lets a Palm HotSync directly against WiRSa — no computer, no Palm
Desktop, no cable to a PC at all. **BitWarden Relay** and **CalDAV Relay** let dedicated Palm apps sync a
Bitwarden/Vaultwarden vault and CalDAV/iCloud/Google/ICS calendars respectively, with WiRSa doing the
network side over WiFi so the Palm never needs its own TCP/TLS stack. It also adds a general-purpose **FTP
Server mode** — not Palm-specific, but useful alongside the others for uploading files onto the SD card
before a HotSync/SASD run. This document covers all of those, plus a note on SLIP Gateway reliability
improvements. For the hardware list see the main [README.md](../README.md). The original project's own
documentation is kept as [README-Original.md](README-Original.md) — note that the MODEM mode, AT
commands and built-in web config server it describes are **not present in this firmware** (see "What
changed" below).

## What changed from the original WiRSa

- **File Transfer mode is gone**, replaced by **HotSync**.
- **Text Playback mode is gone**, replaced by **SASD SDCard**.
- **PPP Gateway mode has been removed entirely** to make room.
- **MODEM mode is gone**, replaced by **FTP Server mode** (see below).
- **Two new relay modes added**: **BitWarden Relay** and **CalDAV Relay** — each pairs with its own
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
- Config is otherwise unchanged; SLIP Gateway has been made substantially more reliable and now
  starts immediately instead of opening a submenu first (see below).

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
  or Back to cancel and return to the previous menu.

---

## SASD SDCard mode

Mounts WiRSa's SD card as a real, native storage card on the Palm — the same way a physical SD card
slot would, if your Palm had one. Wire-compatible with [dmitry.gr's SASD
hardware](https://dmitry.gr/?r=05.Projects&proj=32.%20SASD) and its Palm-side driver, so this works with
the **unmodified `SASD.prc`** slot driver — no special app on the Palm side, and every app that already
uses the standard VFS APIs (file browsing in Address35, card-file features in CryptoNote, beam/export
to card, etc.) just works, with zero changes needed on either side.

### Requirements

- A microSD card inserted in WiRSa, formatted FAT16 or FAT32.
- The Palm's `SASD.prc` slot driver installed (from the original SASD project — this is the Palm-side
  half; WiRSa is standing in for the SASD *hardware*).

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
- **`SD:`**: used/total card space (human-readable, auto-scaling B/KB/MB/GB).
- **`Speed:`**: live transfer speed in KB/s while a file operation is in progress.

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

- A microSD card inserted in WiRSa, with a `PALM` folder at the root (standard Palm Desktop layout).
- WiRSa's serial port physically connected to the Palm (cradle/cable).

### SD card folder layout

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
6. Press the **Back** button any time to return to the main menu, or it will return automatically
   15 seconds after reaching `Completed`/`Init Complete`.

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
4. Press **Back** (or `+++`) to exit and return to the main menu.

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

## FTP Server mode

Runs a background FTP server that serves WiRSa's SD card — upload files to it from a desktop (or from
the Palm itself over SLIP, via an FTP client app like lazyFTP), then HotSync or SASD them onto the
device without a cable swap. Replaces MODEM mode's old menu slot.

### Requirements

- A microSD card inserted in WiRSa.
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

### Interaction with other modes

- **Cannot run at the same time as SASD SDCard mode** (the two touch the SD card in incompatible ways)
  — whichever is active blocks the other from starting, with a clear on-screen message. Stop one to
  start the other.
- **Can stay running during HotSync mode** — nothing blocks the two from being active together, so
  you can upload a file via FTP and then HotSync it onto the Palm without stopping the FTP server
  first. HotSync does write to the card (it moves each installed file from `INSTALL` to `INSTALLED`),
  so don't upload into `INSTALL` while a sync is actually in progress.

---

## SLIP Gateway — reliability improvements

Selecting **`SLIP Gateway`** from the main menu now starts the gateway immediately (no submenu first);
press **Back** from the active gateway screen to return to the SLIP submenu, same as before. If WiFi
isn't already connected, you'll see "Waiting for WiFi..." — see "Background WiFi & the status icon"
above.

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
