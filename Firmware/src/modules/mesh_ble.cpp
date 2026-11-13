// mesh_ble.cpp - see mesh_ble.h. Meshtastic's BLE service: ToRadio (write), FromRadio (read until empty),
// FromNum (notify when FromRadio has something). Bonding is on: the node asks for a PIN the first time.
#include "mesh_ble.h"
#include "relay_timing.h"

#if WIRSA_FEATURE_MESH

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <nvs.h>
#include "log.h"
#include "globals.h"
#include "led.h"

// Overrides the core's weak btInUse() (esp32-hal-misc.c), which is asked right after NVS starts and, when it
// says no, releases all Bluetooth controller memory for good. See mesh_ble.h.
extern "C" bool btInUse() {
  nvs_handle_t h;
  uint8_t v = 0;
  if (nvs_open("wmesh", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, "bt", &v);
    nvs_close(h);
  }
  return v != 0;
}

namespace MeshBle {
namespace {

const char *SVC_UUID = "6ba1b218-15a8-461f-9fa8-5dcae273eafd";
const char *TORADIO_UUID = "f75c76d2-129e-4dad-a1dd-7866124401e7";
const char *FROMRADIO_UUID = "2c55e69e-4993-11ed-b878-0242ac120002";
const char *FROMNUM_UUID = "ed9da18c-a800-4f66-a670-aa7547e34453";

bool gInit = false;
NimBLEClient *gClient = nullptr;
NimBLERemoteCharacteristic *gTo = nullptr, *gFrom = nullptr, *gNum = nullptr;
volatile bool gNotify = false;
volatile uint32_t gPin = 0;
PinProvider gPinProvider = nullptr;
volatile bool gPinAsked = false;
volatile bool gAuthDone = false, gAuthOk = false;
char gErr[40] = "";

class ClientCb : public NimBLEClientCallbacks {
  uint32_t onPassKeyRequest() override {
    gPinAsked = true;
    if (gPinProvider) return gPinProvider();
    return gPin;
  }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(ble_gap_conn_desc *d) override {
    gAuthOk = d && d->sec_state.encrypted;
    gAuthDone = true;
  }
} gCb;

// secureConnection() waits for the pairing with no time limit; if the node never finishes it, this timer
// drops the link, which wakes the wait.
TimerHandle_t gWatchdog = nullptr;
void watchdogCb(TimerHandle_t) {
  if (gClient && gClient->isConnected()) {
    LOGW("Mesh BLE: pairing took too long (45 s) - dropping the link");
    gClient->disconnect();
  }
}

// The scan hands out the address in the stack's own byte order (low byte first). NimBLEAddress(uint8_t[6], type)
// expects display order and reverses it, which pointed the connection at the wrong device; the ble_addr_t
// constructor takes the native order as is.
NimBLEAddress nativeAddress(const uint8_t addr[6], uint8_t type) {
  ble_addr_t a;
  memcpy(a.val, addr, 6);
  a.type = type;
  return NimBLEAddress(a);
}

void notifyCb(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool) { gNotify = true; }

void fail(const char *why) { snprintf(gErr, sizeof gErr, "%s", why); LOGW("Mesh BLE: %s", why); }

}  // namespace

void setPinProvider(PinProvider p) { gPinProvider = p; }

bool ready() { return btInUse(); }
bool active() { return gInit; }

void enableAtNextBoot() {
  Preferences p;
  p.begin("wmesh", false);
  p.putUChar("bt", 1);
  p.end();
}

bool begin() {
  if (gInit) return true;
  if (!ready()) return false;
  // The radio coexistence code aborts the BLE start if WiFi is up without modem sleep ("Should enable WiFi
  // modem sleep when both WiFi and Bluetooth are enabled"); WiRSa turns it off for its network modes.
  WiFi.setSleep(true);
  NimBLEDevice::init("WiRSa");
  NimBLEDevice::setSecurityAuth(true, true, true);          // bond, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);  // the PIN comes from us
  NimBLEDevice::setMTU(247);
  gInit = true;
  ledBle(true);
  return true;
}

void shutdown() {
  if (!gInit) return;
  disconnect();
  NimBLEDevice::deinit(true);
  gInit = false;
  ledBle(false);
  if (WiFi.status() == WL_CONNECTED) WiFi.setSleep(false);    // back to the network modes' setting (network.cpp)
  LOGI("Mesh BLE: stopped");
}

int scan(Hit *hits, int max, uint8_t seconds) {
  if (!begin()) return 0;
  NimBLEScan *s = NimBLEDevice::getScan();
  s->setActiveScan(true);
  s->setInterval(100);
  s->setWindow(99);
  NimBLEScanResults r = s->start(seconds, false);
  int n = 0;
  for (int i = 0; i < r.getCount() && n < max; i++) {
    NimBLEAdvertisedDevice d = r.getDevice(i);               // by value in this library version
    if (!d.isAdvertisingService(NimBLEUUID(SVC_UUID))) continue;
    Hit &h = hits[n++];
    memcpy(h.addr, d.getAddress().getNative(), 6);
    h.type = d.getAddress().getType();
    h.rssi = (int8_t) d.getRSSI();
    snprintf(h.name, sizeof h.name, "%s", d.getName().c_str());
  }
  s->clearResults();
  return n;
}

bool connected() { return gClient && gClient->isConnected() && gTo && gFrom; }
const char *lastError() { return gErr; }

void disconnect() {
  if (gClient) {
    if (gClient->isConnected()) gClient->disconnect();
    NimBLEDevice::deleteClient(gClient);
    gClient = nullptr;
  }
  gTo = gFrom = gNum = nullptr;
}

void abort() {
  NimBLEClient *c = gClient;
  if (c && c->isConnected()) c->disconnect();
}

void forget(const uint8_t addr[6], uint8_t type) {
  NimBLEDevice::deleteBond(nativeAddress(addr, type));
}

// One try. mitm: ask for an authenticated pairing (a PIN). A node set to "no PIN" can only do an unauthenticated
// ("just works") pairing, which a MITM request would refuse, so the caller tries again without it.
static bool tryConnect(const uint8_t addr[6], uint8_t type, uint32_t pin, bool mitm) {
  gErr[0] = 0;
  disconnect();
  gPin = pin; gAuthDone = false; gAuthOk = false; gNotify = false; gPinAsked = false;
  NimBLEDevice::setSecurityAuth(true, mitm, true);
  LOGI("Mesh BLE: connecting to %02X:%02X:%02X:%02X:%02X:%02X type %u (%s)", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
       (unsigned) type, mitm ? "PIN pairing" : "no-PIN pairing");
  gClient = NimBLEDevice::createClient();
  gClient->setClientCallbacks(&gCb, false);
  gClient->setConnectTimeout(RelayTiming::BLE_CONNECT_TIMEOUT_S);
  if (!gClient->connect(nativeAddress(addr, type))) { fail("The node did not answer"); disconnect(); return false; }
  LOGV("Mesh BLE: connected, starting pairing");
  if (!gWatchdog) gWatchdog = xTimerCreate("meshwd", pdMS_TO_TICKS(RelayTiming::BLE_PAIRING_WATCHDOG_MS), pdFALSE, nullptr, watchdogCb);
  if (gWatchdog) xTimerReset(gWatchdog, 0);
  bool secured = gClient->secureConnection();
  if (gWatchdog) xTimerStop(gWatchdog, 0);
  if (!secured) {
    int rc = gClient->getLastError();
    LOGW("Mesh BLE: pairing failed, rc=%d (PIN asked: %s)", rc, gPinAsked ? "yes" : "no");
    char t[40];
    snprintf(t, sizeof t, gPinAsked ? "Pairing failed (rc %d). Wrong PIN?" : "Pairing failed (rc %d)", rc);
    fail(t);
    disconnect();
    return false;
  }
  LOGV("Mesh BLE: paired (%s), looking for the Meshtastic service", gPinAsked ? "PIN" : "no PIN asked");
  NimBLERemoteService *svc = gClient->getService(NimBLEUUID(SVC_UUID));
  if (!svc) { fail("Not a Meshtastic node"); disconnect(); return false; }
  gTo = svc->getCharacteristic(NimBLEUUID(TORADIO_UUID));
  gFrom = svc->getCharacteristic(NimBLEUUID(FROMRADIO_UUID));
  gNum = svc->getCharacteristic(NimBLEUUID(FROMNUM_UUID));
  if (!gTo || !gFrom) { fail("The node's service is incomplete"); disconnect(); return false; }
  if (gNum && gNum->canNotify()) gNum->subscribe(true, notifyCb);
  LOGI("Mesh BLE: ready");
  return true;
}

// Is the node advertising? A connect to a node that is not (out of range, restarting, held by a phone) runs into the library's
// connect timeout, and that path can crash (a null function call on core 0, seen after several failures in a row). A scan of a
// fixed time ends by itself; stopping one by hand crashes too, so none is.
static bool nodeVisible(const uint8_t addr[6]) {
  NimBLEScan *sc = NimBLEDevice::getScan();
  sc->setActiveScan(false);
  sc->setInterval(100);
  sc->setWindow(100);
  NimBLEScanResults r = sc->start(2, false);
  bool seen = false;
  for (int i = 0; i < r.getCount() && !seen; i++) {
    NimBLEAdvertisedDevice d = r.getDevice(i);
    if (memcmp(d.getAddress().getNative(), addr, 6) == 0) seen = true;
  }
  sc->clearResults();
  return seen;
}

bool connect(const uint8_t addr[6], uint8_t type, uint32_t pin) {
  if (!begin()) { fail("Bluetooth did not start"); return false; }
  if (!nodeVisible(addr)) { fail("The node did not answer"); return false; }
  if (tryConnect(addr, type, pin, true)) return true;
  // Failed before the node asked for any PIN: a node set to "no PIN" refuses the authenticated pairing.
  if (!gPinAsked && strncmp(gErr, "The node did not answer", 23) != 0) {
    LOGI("Mesh BLE: trying again without the PIN requirement");
    return tryConnect(addr, type, pin, false);
  }
  return false;
}

bool write(const uint8_t *buf, size_t len) {
  if (!connected()) return false;
  bool ok = gTo->writeValue(buf, len, true);
  if (ok) ledActivity(LED_ACT_NET);
  return ok;
}

bool read(uint8_t *buf, size_t cap, size_t &len) {
  len = 0;
  if (!connected()) return false;
  std::string v = gFrom->readValue();
  if (v.size() > cap) return false;
  memcpy(buf, v.data(), v.size());
  len = v.size();
  if (len) ledActivity(LED_ACT_NET);
  return true;
}

bool takeNotify() { bool n = gNotify; gNotify = false; return n; }

}  // namespace MeshBle

#endif
