// mesh_pb.h - the few Meshtastic protobuf messages WiRSa's Mesh mode needs, read and written by hand
// (no nanopb: about a dozen fields, and the flash budget is tight). Plain C++, no Arduino: it is
// unit-tested on the host (test_host/mesh_pb_test.cpp). Field numbers are from meshtastic/protobufs
// mesh.proto, channel.proto, telemetry.proto, portnums.proto.
//
// A node talks to a client over BLE with ToRadio (client -> node) and FromRadio (node -> client).
// Unknown fields are skipped, so a newer firmware's extra fields never break a read.
#ifndef MESH_PB_H
#define MESH_PB_H

#include <stddef.h>
#include <stdint.h>

namespace MeshPb {

enum : uint32_t {
  PORT_TEXT = 1, PORT_POSITION = 3, PORT_NODEINFO = 4, PORT_ROUTING = 5, PORT_ADMIN = 6, PORT_WAYPOINT = 8, PORT_ALERT = 11,
  PORT_TELEMETRY = 67, PORT_TRACEROUTE = 70
};
enum : uint32_t { NONCE_ALL = 5, NONCE_CONFIG_ONLY = 69420, NONCE_NODES_ONLY = 69421 };
enum : uint32_t { BROADCAST = 0xFFFFFFFFu };

// Routing.Error values that matter to a sender.
enum : uint8_t { ROUTE_NONE = 0, ROUTE_NO_ROUTE = 1, ROUTE_GOT_NAK = 2, ROUTE_TIMEOUT = 3, ROUTE_MAX_RETRANSMIT = 5,
                 ROUTE_NO_CHANNEL = 6, ROUTE_TOO_LARGE = 7, ROUTE_DUTY_CYCLE = 9 };

enum Kind : uint8_t { K_NONE = 0, K_PACKET, K_MY_INFO, K_NODE_INFO, K_CONFIG_DONE, K_CHANNEL, K_QUEUE, K_METADATA, K_REBOOTED, K_CONFIG, K_MQTT };

struct Packet {
  uint32_t from = 0, to = 0, id = 0, rxTime = 0;    // rxTime 0 = the node did not stamp it
  uint32_t port = 0, requestId = 0, hopLimit = 0, hopStart = 0;
  uint8_t channel = 0;
  bool wantAck = false, decoded = false, hasSnr = false;
  float snr = 0;
  int32_t rssi = 0;
  const uint8_t *payload = nullptr; size_t payloadLen = 0;   // points into the buffer that was parsed
  // port == PORT_ROUTING: the Routing message's error, 0xFF = it carries none (a route request/reply).
  uint8_t routeError = 0xFF;
};

struct NodeInfo {
  uint32_t num = 0, lastHeard = 0;
  char id[12] = "", longName[40] = "", shortName[8] = "";
  uint32_t hw = 0, role = 0;
  bool hasSnr = false; float snr = 0;
  int16_t hops = -1;                         // -1 unknown
  int16_t battery = -1;                      // percent, 101 = powered; -1 unknown
  bool hasPos = false; int32_t latE7 = 0, lonE7 = 0;
  uint32_t uptime = 0;                       // seconds, 0 unknown
  uint16_t chanUtil10 = 0, airUtil10 = 0;    // percent x10
  uint8_t userRaw[112]; uint8_t userRawLen = 0; // the User message as sent (set_owner needs all of it back)
  bool hasKey = false;                       // the node's public key (User.public_key, 32 bytes) is known: a direct message can be encrypted to it
  bool fav = false, favKnown = false;        // favourite on the node itself; known only from a full node record
};

struct Channel {
  uint8_t index = 0, role = 0; char name[16] = ""; bool hasPsk = false; uint8_t psk[32]; uint8_t pskLen = 0;
  bool uplink = false, downlink = false;              // ChannelSettings: send to / take from the public MQTT server
  uint8_t raw[96]; uint8_t rawLen = 0;                // the ChannelSettings as sent (id, position precision... stay as they are on a change)
};

struct QueueStatus { int32_t res = 0; uint32_t free = 0, maxlen = 0, packetId = 0; };

struct FromRadio {
  Kind kind = K_NONE;
  uint32_t id = 0;
  uint32_t myNum = 0;            // K_MY_INFO
  uint32_t configId = 0;         // K_CONFIG_DONE
  Packet packet;                 // K_PACKET
  NodeInfo node;                 // K_NODE_INFO
  Channel channel;               // K_CHANNEL
  QueueStatus queue;             // K_QUEUE
  char firmware[16] = "";        // K_METADATA
  uint8_t cfgWhich = 0;          // K_CONFIG: Config oneof number (1 device, 6 lora, 7 bluetooth)
  uint8_t cfgRaw[96]; uint8_t cfgLen = 0;
  uint8_t mqttRaw[272]; uint16_t mqttLen = 0;   // K_MQTT: the MQTT module's MQTTConfig as sent
};

// Parses one FromRadio. False when the bytes are not a well-formed message.
bool parseFromRadio(const uint8_t *buf, size_t len, FromRadio &out);

// ToRadio builders: return the byte count written, 0 if cap is too small.
size_t buildWantConfig(uint8_t *buf, size_t cap, uint32_t nonce);
size_t buildHeartbeat(uint8_t *buf, size_t cap);
size_t buildDisconnect(uint8_t *buf, size_t cap);
// A text message. id must be non-zero and unique (the node echoes it in the routing ack).
size_t buildText(uint8_t *buf, size_t cap, uint32_t to, uint8_t channel, uint32_t id, bool wantAck,
                 const char *text, size_t textLen);

// Any packet with a Data body: payload may be empty. wantResponse asks the other node to answer (node info,
// position, telemetry and traceroute requests).
size_t buildData(uint8_t *buf, size_t cap, uint32_t to, uint8_t channel, uint32_t id, uint32_t port,
                 const uint8_t *payload, size_t plen, bool wantAck, bool wantResponse);
size_t positionPayload(uint8_t *out, size_t cap, int32_t latE7, int32_t lonE7);
size_t waypointPayload(uint8_t *out, size_t cap, uint32_t id, int32_t latE7, int32_t lonE7, const char *name);
size_t telemetryRequestPayload(uint8_t *out, size_t cap);

// What the answers to those requests carry.
struct Route { uint32_t towards[8], back[8]; uint8_t nTowards = 0, nBack = 0; };
struct Pos { bool hasPos = false, hasAlt = false; int32_t latE7 = 0, lonE7 = 0, alt = 0; };
struct Tel { bool hasDev = false, hasEnv = false; int battery = -1; float volt = 0, chanUtil = 0, airUtil = 0; uint32_t uptime = 0;
             float temp = 0, hum = 0, press = 0; };
struct Way { uint32_t id = 0; int32_t latE7 = 0, lonE7 = 0; char name[32] = ""; };
bool parseRoute(const uint8_t *p, size_t n, Route &out);
bool parsePosition(const uint8_t *p, size_t n, Pos &out);
bool parseTelemetry(const uint8_t *p, size_t n, Tel &out);
bool parseWaypoint(const uint8_t *p, size_t n, Way &out);
bool parseUserNames(const uint8_t *p, size_t n, char *longName, size_t lcap, char *shortName, size_t scap);

// ---- settings: read what the node sent, write it back through admin messages ----------------------------
enum : uint8_t { CFG_DEVICE = 1, CFG_LORA = 6, CFG_BLUETOOTH = 7 };
struct LoraVals { bool usePreset = true; uint32_t preset = 0, region = 0, hop = 3; int32_t txPower = 0; };
struct BtVals { bool enabled = true; uint32_t mode = 0, fixedPin = 0; };
void readLora(const uint8_t *raw, size_t n, LoraVals &v);
uint32_t readDeviceRole(const uint8_t *raw, size_t n);
void readBluetooth(const uint8_t *raw, size_t n, BtVals &v);
void readOwner(const uint8_t *raw, size_t n, char *longName, size_t lcap, char *shortName, size_t scap);
// The MQTT module (ModuleConfig.mqtt): the fields a Palm can change. Strings as the node holds them (UTF-8).
struct MqttVals {
  bool enabled = false, encrypt = false, json = false, tls = false, proxy = false, mapRep = false;
  char address[64] = "", username[64] = "", password[64] = "", root[32] = "";
};
void readMqtt(const uint8_t *raw, size_t n, MqttVals &v);

// A copy of the node's own message with only these fields changed (the node replaces a whole section on a set,
// so everything else must go back as it was).
size_t patchLora(const uint8_t *raw, size_t n, uint32_t preset, uint32_t region, uint32_t hop, int32_t txPower,
                 uint8_t *out, size_t cap);
size_t patchDevice(const uint8_t *raw, size_t n, uint32_t role, uint8_t *out, size_t cap);
size_t patchBluetooth(const uint8_t *raw, size_t n, uint32_t mode, uint32_t fixedPin, uint8_t *out, size_t cap);
size_t patchOwner(const uint8_t *raw, size_t n, const char *longName, const char *shortName, uint8_t *out, size_t cap);
// The node's own ChannelSettings with the key, name and the two MQTT flags replaced; every other field stays as the node sent it.
size_t patchChannelSettings(const uint8_t *raw, size_t n, const uint8_t *psk, size_t pskLen, const char *name, bool uplink, bool downlink,
                            uint8_t *out, size_t cap);
// set_channel with ChannelSettings already built
size_t adminSetChannelRaw(uint8_t *out, size_t cap, uint8_t index, uint8_t role, const uint8_t *settings, size_t sl,
                          const uint8_t *key, size_t keyLen);
// The node's own MQTTConfig with these fields replaced (map_report_settings and anything newer stay as the node sent them).
size_t patchMqtt(const uint8_t *raw, size_t n, const MqttVals &v, uint8_t *out, size_t cap);

// AdminMessage bodies (key = the session passkey the node gave, may be empty), each written to out.
size_t adminGetConfig(uint8_t *out, size_t cap, uint32_t type);                        // 8 = session key
size_t adminSetOwner(uint8_t *out, size_t cap, const uint8_t *user, size_t n, const uint8_t *key, size_t keyLen);
size_t adminSetConfig(uint8_t *out, size_t cap, uint8_t which, const uint8_t *cfg, size_t n, const uint8_t *key, size_t keyLen);
// set_module_config: ModuleConfig { which = cfg } (which 1 = mqtt)
size_t adminSetModuleConfig(uint8_t *out, size_t cap, uint8_t which, const uint8_t *cfg, size_t n, const uint8_t *key, size_t keyLen);
size_t adminSetChannel(uint8_t *out, size_t cap, uint8_t index, uint8_t role, const char *name, const uint8_t *psk,
                       size_t pskLen, const uint8_t *key, size_t keyLen);
size_t adminFlag(uint8_t *out, size_t cap, uint32_t field, const uint8_t *key, size_t keyLen);   // 64 begin, 65 commit
size_t adminInt(uint8_t *out, size_t cap, uint32_t field, uint32_t v, const uint8_t *key, size_t keyLen);  // 97 reboot
size_t adminTime(uint8_t *out, size_t cap, uint32_t utc, const uint8_t *key, size_t keyLen);          // set_time_only
// Wraps an AdminMessage in a ToRadio packet addressed to the node itself.
size_t buildAdmin(uint8_t *buf, size_t cap, uint32_t myNum, uint32_t id, const uint8_t *admin, size_t alen);
// The session passkey in an AdminMessage the node sent; false if it carries none.
bool adminPasskey(const uint8_t *payload, size_t n, uint8_t *key, size_t &keyLen);

}  // namespace MeshPb

#endif
