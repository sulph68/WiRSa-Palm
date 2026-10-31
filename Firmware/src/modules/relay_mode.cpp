// relay_mode.cpp - see relay_mode.h.
#include "relay_mode.h"
#include "mode_exit.h"
#include "globals.h"
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "relay_proto.h"
#include "palm_ctl.h"
#include "log.h"
#include "settings.h"     // modeAfterExitSec
#include <WiFi.h>
#include "led.h"

namespace RelayMode {

bool enter(Spec &m) {
  menuMode = m.modeId;
  ledRefresh();   // an RGB LED is dark in a Mesh session
  setBinaryMode(true);
  SerialPrintLn(String("\r\nEntering ") + m.name + " Mode...");

  if (m.needsWifi) {
    if (WiFi.status() != WL_CONNECTED) {
      // waitForWiFiConnection() shows its own "Waiting for WiFi..." / "WiFi unavailable. Retry?" UI and retries for up to a
      // minute. A false return means the user backed out.
      if (!waitForWiFiConnection()) {
        SerialPrintLn(String(m.name) + ": WiFi not available - cancelled.");
        setBinaryMode(false);
        mainMenu(false);
        return false;
      }
    }
    if (m.wifiReady) *m.wifiReady = true;
  }

  if (m.onEnter) m.onEnter();
  LOGI("%s: mode entered%s", m.tag, m.needsWifi ? " - WiFi connected" : "");

  // Every session starts at the fixed idle rate, not at whatever the global setup() left the port at: HELLO negotiates up from here.
  PhysicalSerial.begin(m.idleBaud, WIRSA_SERIAL_FORMAT, RXD2, TXD2);

  if (m.drainAtEntry) {
    // The Palm app can have bytes sitting on the port before the mode starts (app already open, or a session that left a frame
    // half sent): taken as a frame header they would produce checksum noise and might never resynchronise.
    int drained = 0;
    while (PhysicalSerial.available()) { PhysicalSerial.read(); drained++; }
    if (logOn(LOG_VERBOSE) && drained > 0) logPrintf(LOG_VERBOSE, "%s: drained %d stale byte(s) from serial at mode entry\r\n", m.tag, drained);
  }

  SerialPrintLn(String(m.name) + " Active - waiting for the Palm");
  SerialPrintLn("Press +++ or BACK button to exit");
  m.drawHeader();
  m.drawStatus("Preparing...");
  RelayProto::secureBegin();                 // the serial-link key pair, once per boot (relay_proto.h)
  m.drawStatus("Waiting...");
  m.probed = false; m.probeAt = 0;
  if (m.timer) modeTimerStart(*m.timer);     // after the key: generating it is not idle time
  return true;
}

void leave(Spec &m) {
  if (m.beforeLeave) m.beforeLeave();
  setBinaryMode(false);
  if (m.onLeave) m.onLeave();
  RelayProto::secureReset();
  // Leaving while a negotiated rate is still in use (BACK or "+++" before the Palm's BYE, or before the watchdog gave it up) stops the
  // loop, so its own revert never runs: put the menu's rate back here, or the next mode that expects it (FTP Server, SASD, HotSync)
  // finds the port at whatever this one negotiated. menuBaud(): the configured baud, or 9600 after the dual-rate menu swapped.
  PhysicalSerial.begin(menuBaud(), WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  SerialPrintLn(String("\r\nExiting ") + m.name + " Mode...");
  LOGI("%s: mode exit", m.tag);
}

void leaveToMenu(Spec &m) {
  leave(m);
  ModeExit::toMenu();
}

Tick tick(Spec &m) {
  if (ModeExit::backPressed()) {
    leaveToMenu(m);
    return Tick::Left;
  }
  waitSwitches();

  // "+++" on the USB serial only: the Palm port carries the binary protocol and must not be read as commands.
  if (ModeExit::escapeTyped()) {
    leaveToMenu(m);
    return Tick::Left;
  }

  // WiRSaCtl (palm_ctl.h): PING / STATUS, and EXIT, accepted only between sessions (idle rate, within 2%: baudRate() is what the UART
  // really achieved).
  if (palmCtlModeHook(m.letter, labs((long) PhysicalSerial.baudRate() - (long) m.idleBaud) < (long) m.idleBaud / 50, m.maxPalmType)) {
    leaveToMenu(m);
    return Tick::Left;
  }

  // The Exit Timers: never used, or idle long enough after a session. A mode with a probe has its own rule (below).
  if (m.timer && !m.probeType && modeTimerExpired(*m.timer)) {
    LOGI("%s: exit timer - back to the main menu", m.tag);
    leaveToMenu(m);
    return Tick::Left;
  }

  if (m.probeType && m.sessionActive && m.lastFrameAt) {
    bool gone = false;
    if (m.busy && m.busy()) { *m.lastFrameAt = millis(); m.probed = false; }   // the Palm waits for the node, or has a dialog open: not silent
    if (m.probed && *m.lastFrameAt > m.probeAt) m.probed = false;              // something came after the probe
    if (!*m.sessionActive) {
      gone = m.timer && modeTimerExpired(*m.timer);                            // no session yet: the "unused" timer
    } else if (modeAfterExitSec) {
      unsigned long quiet = millis() - *m.lastFrameAt, limit = (unsigned long) modeAfterExitSec * 1000UL;
      if (!m.probed && quiet > limit) {
        RelayProto::writeFrame(PhysicalSerial, m.probeType, nullptr, 0);       // "are you still there?"
        LOGV("%s: silence probe sent after %lu ms\r\n", m.tag, quiet);
        m.probed = true;
        m.probeAt = millis();
      } else if (m.probed && millis() - m.probeAt > limit) {
        gone = true;                                                           // asked, and no answer in the same time
      }
    }
    if (gone) {
      LOGI("%s: exit timer - back to the main menu", m.tag);
      leaveToMenu(m);
      return Tick::Left;
    }
  }

  if (m.needsWifi && m.wifiReady && !*m.wifiReady) {
    if (WiFi.status() == WL_CONNECTED) {
      *m.wifiReady = true;
      LOGV("%s: WiFi came up", m.tag);
      SerialPrintLn(String(m.name) + ": WiFi connected - now servicing requests.");
      m.drawStatus("Waiting...");
    } else {
      return Tick::Wait;                     // nothing to service yet
    }
  }

  // A rate the Palm negotiated and then never used (no sync request): do not stay listening at a rate the next connection will not
  // expect. Checked every pass, whether or not data is waiting.
  if (m.negotiateTimeoutMs && m.revertToIdle && m.negotiatedAt && PhysicalSerial.baudRate() != m.idleBaud &&
      *m.negotiatedAt != 0 && (millis() - *m.negotiatedAt) > m.negotiateTimeoutMs) {
    LOGW("%s: negotiated baud timed out waiting for sync request, reverting to idle rate", m.tag);
    *m.negotiatedAt = 0;
    m.revertToIdle();
    m.drawStatus("Waiting...");
    if (m.timer) modeTimerSessionEnded(*m.timer);   // the Palm went quiet without a BYE
  }
  return Tick::Go;
}

bool readFrame(Spec &m, uint8_t &type, uint8_t *body, size_t cap, size_t &len, uint32_t idleMs, uint32_t byteMs) {
  if (!PhysicalSerial.available()) return false;
#if WIRSA_FEATURE_PALMCTL
  // A WiRSaCtl frame (0x16) is for palmCtlModeHook at the top of the next pass, never for this reader: taken as a header, its bytes
  // become a bogus length and the byte timeout then swallows every PING behind it (found on hardware: PINGs queued while the mode
  // generated its key at entry were never answered).
  if (PhysicalSerial.peek() == RelayProto::CTL_LEAD) return false;
#endif
  if (!RelayProto::readFrame(PhysicalSerial, type, body, cap, len, idleMs, byteMs)) return false;
  if (m.timer && !m.probeType) modeTimerSessionActive(*m.timer);
  if (m.onFrame) m.onFrame();
  // The serial-link key exchange comes first on every new connection.
  if (RelayProto::handleKex(PhysicalSerial, type, body, len)) return false;
  return true;
}

bool handleHello(Spec &m, const uint8_t *body, size_t len) {
  RelayProto::Reader r(body, len);
  uint32_t offered = r.u32();
  if (!r.ok) {
    LOGW("%s: malformed HELLO (short body, old Palm build?) - ignored, no reply sent", m.tag);
    return false;
  }
  uint32_t neg = agreedBaud(offered, m.idleBaud);
  LOGI("%s: baud negotiate - Palm offered %lu, capped to %lu (WiRSa config max)", m.tag, (unsigned long) offered, (unsigned long) neg);

  uint8_t buf[64];
  RelayProto::Writer w(buf, sizeof buf);
  if (m.helloServer) {
    w.u8(m.helloVersion);
    w.u8(0x00);                     // capability flags: none
    w.str(String(m.helloServer));
  }
  w.u32(neg);
  // writeFrame flushes before it returns: the reply is fully on the wire, at the OLD rate the Palm still listens at, before the port moves.
  RelayProto::writeFrame(PhysicalSerial, m.helloAckType, buf, w.len);
  PhysicalSerial.begin(neg, WIRSA_SERIAL_FORMAT, RXD2, TXD2);
  if (m.negotiatedAt) *m.negotiatedAt = millis();
  if (m.sessionActive) *m.sessionActive = true;
  if (m.timer) modeTimerSessionActive(*m.timer);
  m.drawHeader();                   // the live baud readout in the title bar
  return true;
}

bool dispatch(const Spec &m, const Frame *table, size_t count, uint8_t type, const uint8_t *body, size_t len) {
  for (size_t i = 0; i < count; i++) {
    if (table[i].type != type) continue;
    LOGV("%s: frame %s, %u byte(s)", m.tag, table[i].name, (unsigned) len);
    table[i].fn(body, len);
    return true;
  }
  LOGW("%s: unknown frame type 0x%02X ignored", m.tag, (unsigned) type);
  return false;
}

uint32_t agreedBaud(uint32_t offered, uint32_t idleBaud) {
  uint32_t neg = offered, maxAllowed = (uint32_t) bauds[serialSpeed];
  if (neg > maxAllowed) neg = maxAllowed;
  return neg ? neg : idleBaud;
}

}  // namespace RelayMode
