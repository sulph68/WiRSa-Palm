// mesh_state.cpp - Mesh Relay mode: the state its files share (declared in mesh_internal.h), the settings kept in NVS, the OLED.
#include "mesh_internal.h"

#if WIRSA_FEATURE_MESH

#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include "palm_ctl.h"
#include "log.h"
#include "settings.h"
#include "relay_timing.h"
#include "palm_text.h"
#include <time.h>

namespace MeshRelay {

// ---- shared state: what each is for is in mesh_internal.h ----
uint8_t gBody[BODY_MAX];
uint8_t gOut[BODY_MAX];
uint8_t gAckBody[BODY_MAX];
uint8_t gRadio[520];
Preferences gPrefs;

Paired gPaired;
MeshSlots::Slot gSlots[MeshSlots::MAX];
uint8_t gActive = 0, gBgSlot = MeshSlots::NONE;
uint8_t gRelayMode = 0, gRelayEvery = 1, gSdFlush = 4;
bool gExitAfterBye = false;
ModeExitTimer gTimer;
unsigned long gNextFetch = 0, gFetchStart = 0, gLastFrameAt = 0;
bool gConfigOnly = false;
uint8_t gLink = L_NONE;
char gLinkText[40] = "";
bool gPalmSession = false;
uint32_t gSeenSeq = 0, gSeenVer = 0;
uint8_t gSeenLink = 0;
int gSeenBatt = -2;
unsigned long gLastNews = 0;
unsigned long gLastAttempt = 0, gLastPalmFrame = 0, gLastPoll = 0, gNegotiatedAt = 0, gHoldUntil = 0;
uint32_t gLastPollUtc = 0;
bool gStarted = false;
unsigned long gRestartUntil = 0;
volatile uint8_t gCheckState = 0;
bool gHeadless = false, gSpiUp = false;
volatile bool gStandalone = false;

bool gHistKeep = false, gRestored = false, gSdHas = false, gSpoolSaid = false;
uint32_t gFlushedSeq = 0, gConfirmedSeq = 0;
unsigned long gPendingSince = 0, gSpoolRetryAt = 0;

RTC_FAST_ATTR uint8_t gRingMem[RING_BYTES];
RTC_FAST_ATTR uint32_t gBgSleepMagic = 0;
RTC_FAST_ATTR uint8_t gRingChecks = 0;
MeshSpool::Ring gRing(gRingMem, RING_BYTES);

QueueHandle_t gCmdQ = nullptr;
SemaphoreHandle_t gCoreMx = nullptr;
volatile bool gWorkerOn = false, gShutdownDone = false;
volatile uint8_t gBusyOp = 0;

bool gLinkPending = false; uint8_t gLinkNew = 0; char gLinkNewText[40] = "";
ScanResult gScan;
OpResult gPairRes, gConnRes, gCfgReadRes, gCfgWriteRes;
uint8_t gKey[16]; size_t gKeyLen = 0;
volatile bool gPinNeed = false, gPinGiven = false; volatile uint32_t gPinValue = 0; bool gPinSent = false;
SendRes gSendRes[6]; volatile uint8_t gSendHead = 0, gSendTail = 0;
bool gPairing = false;
bool gWasUp = false;

// ---- helpers ----

uint32_t utcNow() { time_t t = time(nullptr); return t > 1700000000 ? (uint32_t) t : 0; }

// Names for the Palm: each emoji becomes the single byte 0x10 (PalmText::Emoji::Marker), which the Palm draws as character 0x10 of its Symbol font.
String latinName(const char *s) {
  PalmText::Options o; o.emoji = PalmText::Emoji::Marker;
  return PalmText::fromUtf8(String(s ? s : ""), o);
}

void loadPrefs() {
  gPrefs.begin("wmesh", true);
  gRelayMode = gPrefs.getUChar("mode", 0);
  gRelayEvery = gPrefs.getUChar("every", 1);
  gSdFlush = gPrefs.getUChar("flush", 4);
  if (gSdFlush > 4) gSdFlush = 4;
  gHistKeep = gPrefs.getUChar("hist", 0) != 0;
  memset(gSlots, 0, sizeof gSlots);
  if (gPrefs.getBytesLength("slots") == sizeof gSlots) gPrefs.getBytes("slots", gSlots, sizeof gSlots);
  if (!MeshSlots::valid(gSlots)) memset(gSlots, 0, sizeof gSlots);
  gActive = gPrefs.getUChar("active", 0);
  gBgSlot = gPrefs.getUChar("bgslot", MeshSlots::NONE);
  gPrefs.end();
  if (gBgSlot >= MeshSlots::MAX || gSlots[gBgSlot].state != MeshSlots::PAIRED) gBgSlot = MeshSlots::NONE;
  if (gBgSlot != MeshSlots::NONE) { gRelayEvery = (uint8_t) (gSlots[gBgSlot].every - 1); gSdFlush = (uint8_t) (gSlots[gBgSlot].flush - 1); }
  if (gRelayMode == 1 && gBgSlot == MeshSlots::NONE) gRelayMode = 0;
  if (gRelayMode == 1) gActive = gBgSlot;                       // background checks are the node's home: a restart or a new session starts there
  else if (gActive >= MeshSlots::MAX) gActive = 0;
  syncPaired();
}

// The active slot, the background slot and the background setting.
void saveSlots() {
  gPrefs.begin("wmesh", false);
  gPrefs.putBytes("slots", gSlots, sizeof gSlots);
  gPrefs.putUChar("active", gActive);
  gPrefs.putUChar("bgslot", gBgSlot);
  gPrefs.end();
}

void saveRelay() {
  gPrefs.begin("wmesh", false);
  gPrefs.putUChar("mode", gRelayMode);
  gPrefs.putUChar("every", gRelayEvery);
  gPrefs.putUChar("flush", gSdFlush);
  gPrefs.putUChar("bgslot", gBgSlot);
  gPrefs.end();
}

uint8_t linkIdle() { return gPaired.have ? L_DOWN : gSlots[gActive].state == MeshSlots::ARCHIVED ? L_ARCHIVED : L_NONE; }
bool bgReady() { return gRelayMode == 1 && gBgSlot == gActive && gPaired.have; }
uint8_t activeEvery() { uint8_t e = gSlots[gActive < MeshSlots::MAX ? gActive : 0].every; return e ? (uint8_t) (e - 1) : 1; }
uint8_t activeFlush() { uint8_t f = gSlots[gActive < MeshSlots::MAX ? gActive : 0].flush; return f ? (uint8_t) (f - 1) : 4; }
uint8_t modeOfActive() { return gRelayMode == 1 && gBgSlot == gActive ? 1 : 0; }

void drawHeader() {
  display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
  display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
  display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
  display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
  display.drawLine(126, 1, 126, 14, SSD1306_BLACK);
  display.setCursor(4, 4);
  display.setTextColor(SSD1306_BLACK);
  String line = "Meshtastic";
  display.print(line);
  display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 17);
  display.display();
  drawWifiStatusIcon();
}

void drawStatus() {
  display.fillRect(0, 17, 128, 47, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 19);
  display.print(gPaired.name[0] ? gPaired.name : "No node paired");
  display.setCursor(0, 31);
  display.print(gLink == L_READY ? "Linked" : gLink == L_FAILED ? "Link failed" : gLink == L_ARCHIVED ? "Archived" : gPaired.have ? "Not linked" : "Pair from the Palm");
  display.setCursor(0, 43);
  display.print(MeshCore::msgCount());
  display.print(" messages");
  display.display();
}

void setLink(uint8_t l, const char *text) {
  bool changed = l != gLink;
  gLink = l;
  snprintf(gLinkText, sizeof gLinkText, "%s", text);
  if (changed) drawStatus();
}

unsigned long everyMs() { return (unsigned long) EVERY_MIN[gRelayEvery < 5 ? gRelayEvery : 1] * 60000UL; }

}  // namespace MeshRelay

#endif  // WIRSA_FEATURE_MESH
