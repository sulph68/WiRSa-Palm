// modes.cpp - see modes.h. The table below is the only place with a `#if WIRSA_FEATURE_xxx` for a mode.
#include "modes.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include "ftp_server.h"
#include "slip_mode.h"
#include "sasd.h"
#include "hotsync.h"
#include "wifi_setup.h"
#include "diagnostics.h"
#include "bitwarden_relay.h"
#include "caldav_relay.h"
#include "mesh_relay.h"

namespace Modes {

// A mode's functions and flag exist only when its feature is built: IF_xxx(thing) is thing, or nothing (nullptr / 0) without the feature.
#if WIRSA_FEATURE_FTPSERVER
#define IF_FTP(x) x
#else
#define IF_FTP(x) nullptr
#endif
#if WIRSA_FEATURE_HOTSYNC
#define IF_HOTSYNC(x) x
#else
#define IF_HOTSYNC(x) nullptr
#endif
#if WIRSA_FEATURE_SASD
#define IF_SASD(x) x
#else
#define IF_SASD(x) nullptr
#endif
#if WIRSA_FEATURE_SLIP
#define IF_SLIP(x) x
#else
#define IF_SLIP(x) nullptr
#endif
#if WIRSA_FEATURE_BITWARDEN
#define IF_BITWARDEN(x) x
#else
#define IF_BITWARDEN(x) nullptr
#endif
#if WIRSA_FEATURE_CALDAV
#define IF_CALDAV(x) x
#else
#define IF_CALDAV(x) nullptr
#endif
#if WIRSA_FEATURE_MESH
#define IF_MESH(x) x
#else
#define IF_MESH(x) nullptr
#endif
#if WIRSA_FEATURE_UTILITIES
#define IF_UTILITIES(x) x
#else
#define IF_UTILITIES(x) nullptr
#endif

namespace {

// Starting a mode from the main menu, for the ones that are more than one call.
void startFtp() { ftpServerMenu(false); }
#if WIRSA_FEATURE_HOTSYNC
void startHotsync() { startHotsyncFromMenu(); }
#endif
void startUtilities() { diagnosticsInit(); diagnosticsMenu(false); }
void startConfig() { settingsMenu(false); }

// The order of the rows is the order of the main menu. A row without a label is a screen the menus move between, not a mode on the menu.
//                id               key  label                flag                       built                    ctlEnter needsWifi needsCard transfer loop                      enter
const Info kModes[] = {
  { MODE_MAIN,        0,   nullptr,             0,                         true,                    false, false, false, false, mainLoop,                nullptr },
  { MODE_FTPSERVER,   'F', "FTP Server",        FLAG_FTP,      WIRSA_FEATURE_FTPSERVER, false, false, false, false, IF_FTP(ftpServerLoop),   IF_FTP(startFtp) },
  { MODE_FILE,        'H', "HotSync",           FLAG_HOTSYNC, WIRSA_FEATURE_HOTSYNC, true,  false, true,  true,  IF_HOTSYNC(hotsyncLoop), IF_HOTSYNC(startHotsync) },
  { MODE_PLAYBACK,    'D', "SAS[D] SDCard",     FLAG_SASD,    WIRSA_FEATURE_SASD,      true,  false, true,  true,  IF_SASD(sasdLoop),      IF_SASD(startSasdFromMenu) },
  { MODE_SLIP,        'S', "SLIP Gateway",      FLAG_SLIP,    WIRSA_FEATURE_SLIP,      true,  true,  false, false, IF_SLIP(slipLoop),      IF_SLIP(enterSlipMode) },
  { MODE_BITWARDEN,   'B', "BitWarden Relay",   FLAG_BITWARDEN, WIRSA_FEATURE_BITWARDEN, true, true, false, true, IF_BITWARDEN(bitwardenRelayLoop), IF_BITWARDEN(enterBitwardenRelayMode) },
  // 'A' not 'C'/'D': both are taken (Config, SASD); the label's "[A]" shows it, as "SAS[D] SDCard" does for its letter.
  { MODE_CALDAV_RELAY, 'A', "C[A]lDAV Relay",   FLAG_CALDAV, WIRSA_FEATURE_CALDAV,   true,  true,  false, true,  IF_CALDAV(caldavRelayLoop), IF_CALDAV(enterCalDavRelayMode) },
  { MODE_MESH,        'T', "Mesh[T]astic",      FLAG_MESH,    WIRSA_FEATURE_MESH,      true,  false, false, true,  IF_MESH(meshRelayLoop), IF_MESH(enterMeshRelayMode) },
  { MODE_DIAGNOSTICS, 'U', "Utilities",         0,                         WIRSA_FEATURE_UTILITIES, false, false, false, false, IF_UTILITIES(diagnosticsLoop), IF_UTILITIES(startUtilities) },
  { MODE_SETTINGS,    'C', "Config",            0,                         true,                    false, false, false, false, settingsLoop,            startConfig },   // always built, never gated
  { MODE_SETBAUD,     0,   nullptr,             0,                         true,                    false, false, false, false, baudLoop,                nullptr },
  { MODE_ORIENTATION, 0,   nullptr,             0,                         true,                    false, false, false, false, orientationLoop,         nullptr },
  { MODE_WIFI_SETUP,  0,   nullptr,             0,                         true,                    false, false, false, false, wifiSetupLoop,           nullptr },
  { MODE_WIFI_PASSWORD, 0, nullptr,             0,                         true,                    false, false, false, false, wifiPasswordLoop,        nullptr },
};

char upper(char c) { return (c >= 'a' && c <= 'z') ? (char) (c - 32) : c; }

}  // namespace

const Info *all(size_t &count) {
  count = sizeof kModes / sizeof kModes[0];
  return kModes;
}

const Info *byId(int id) {
  for (const Info &m : kModes) if (m.id == id && m.built) return &m;
  return nullptr;
}

const Info *byKey(char key) {
  if (!key) return nullptr;
  for (const Info &m : kModes) if (m.key == upper(key) && m.built) return &m;
  return nullptr;
}

bool knownCtlLetter(char key) {
  for (const Info &m : kModes) if (m.key == key && m.ctlEnter) return true;
  return false;
}

int menuEntries(const char **labels, int cap) {
  int n = 0;
  for (const Info &m : kModes)
    if (m.built && m.label && n < cap) labels[n++] = m.label;
  return n;
}

bool startFromMenu(char key, int menuSel) {
  int idx = 0;
  for (const Info &m : kModes) {
    if (!m.built || !m.label) continue;
    // Both the letter and the position start it, and a mode that is not built is gone from both: its letter typed over the USB console
    // does nothing either (it has no row to match).
    if (m.enter && ((key && upper(key) == m.key) || menuSel == idx)) {
      SerialPrint(key);                 // echo it back if it was a valid entry
      m.enter();
      return true;
    }
    idx++;
  }
  return false;
}

bool runLoop(int menuModeId) {
  for (const Info &m : kModes) {
    if (m.id != menuModeId || !m.built || !m.loop) continue;
    m.loop();
    return true;
  }
  return false;
}

bool isTransfer(int menuModeId) {
  for (const Info &m : kModes) if (m.id == menuModeId && m.built) return m.transfer;
  return false;
}

char ctlLetter(int menuModeId) {
  for (const Info &m : kModes)
    if (m.id == menuModeId && m.built && m.ctlEnter) return m.key;
  return 'M';   // the main menu or any menu screen
}

uint16_t builtFlags() {
  uint16_t f = 0;
  for (const Info &m : kModes) if (m.built) f |= m.flag;
  return f;
}

}  // namespace Modes
