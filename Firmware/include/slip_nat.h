#ifndef SLIP_NAT_H
#define SLIP_NAT_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

// ============================================================================
// NAT Configuration Constants
// ============================================================================

// Connection table sizes (ESP32 RAM limited - ~320KB total)
#define NAT_MAX_TCP_CONNECTIONS  16    // Max concurrent TCP connections
#define NAT_MAX_UDP_SESSIONS     32    // Max concurrent UDP sessions (increased for DNS)
#define NAT_MAX_ICMP_SESSIONS    8     // Max concurrent ICMP (ping) sessions
#define NAT_MAX_PORT_FORWARDS    8     // Max port forwarding rules

// Timeout values (milliseconds)
#define NAT_TCP_TIMEOUT_MS       1800000 // 30 minutes for established TCP (IRC, etc)
// Was 30000 (30s). Shrunk as part of the per-entry-socket fix below: each
// NatUdpEntry now owns its own bound socket rather than sharing one, and
// CONFIG_LWIP_MAX_SOCKETS is a hard system-wide 16-socket ceiling shared
// with TCP (NAT_MAX_TCP_CONNECTIONS=16 was already sized against that,
// assuming UDP used exactly one socket total). An entry that never gets a
// response used to hold its socket open for the full 30s, and several such
// entries overlapping was enough to exhaust that shared pool and starve TCP
// too. Real observed DNS/NTP round-trips have all been under 150ms, so 5s
// leaves generous margin for a genuinely slow server while making sure a
// dead/unanswered query's socket doesn't linger long enough to accumulate.
#define NAT_UDP_TIMEOUT_MS       5000    // 5 seconds for UDP
#define NAT_TCP_SYN_TIMEOUT_MS   30000   // 30 seconds for connection setup (outer safety net -
                                          // natPollConnections' own NAT_TCP_CONNECT_TIMEOUT_MS
                                          // check reacts much faster than this in normal operation)
#define NAT_ICMP_TIMEOUT_MS      10000   // 10 seconds for ICMP
#define NAT_TCP_KEEPALIVE_MS     300000  // 5 minutes between keepalives
#define NAT_TCP_STALL_TIMEOUT_MS 60000   // 60s stall detection

// How long a connection in NAT_TCP_CLOSING/NAT_TCP_FIN_WAIT stays in the
// table before natCleanupExpired reaps it. The Palm-side FTP client
// (lazyFTP) doesn't notice a closed connection until its own NetLibReceive
// blocking-read timeout (up to 60s) elapses - a FIN with zero payload
// doesn't unblock it early. If the gateway reaps the entry sooner than
// that, anything the client sends afterward (a delayed FIN, or a retry)
// hits "no entry" and the transfer looks broken even though the data
// already arrived intact. This comfortably exceeds that 60s client-side wait.
#define NAT_TCP_CLOSING_TIMEOUT_MS 75000

// Non-blocking connect() poll timeout - checked every loop iteration in
// natPollConnections, so this is the real deadline for a stuck connection
// attempt (a bit more generous than WiFiClient's blocking 3s default since
// waiting longer here no longer freezes the rest of the gateway).
#define NAT_TCP_CONNECT_TIMEOUT_MS 6000

// How often natPollConnections sends a zero-window probe (RFC 1122
// 4.2.2.17) while the client's advertised window is fully used and we have
// buffered data waiting. Real TCP receivers are only *required* to
// acknowledge (accept or reject) an incoming byte, not to proactively send
// an unsolicited window-update once their buffer drains - if PalmOS's own
// TCP stack doesn't bother with the optional proactive update, and we've
// correctly stopped sending because we think the window is exhausted,
// nothing will ever prompt a fresh ACK from it on its own: a genuine silent
// deadlock, only ever broken today by the 60s stall timeout giving up and
// tearing the whole connection down. Probing (send 1 byte past the client's
// stated window anyway) forces a fresh ACK+window one way or another -
// accepted if the client actually had room, or a duplicate ACK re-asserting
// its current (still closed) window otherwise - either way we learn the
// real state instead of guessing blind for up to a full minute.
#define NAT_TCP_PROBE_INTERVAL_MS 2000

// How long natPollConnections will wait for real forward ACK progress on
// outstanding (sent but unacknowledged) data before assuming it was lost on
// the serial link and re-emitting it from the still-buffered unacked region
// (see NatTcpEntry::lastAckTime and the retransmit logic in
// natPollConnections). This link has no hardware flow control and no error
// correction of its own, so a byte can genuinely be dropped/corrupted in
// transit; without this, that single lost segment stalls the connection
// permanently until the much longer NAT_TCP_STALL_TIMEOUT_MS gives up and
// tears the whole thing down (observed real-world: large FTP directory
// listings bailing partway through with no further data ever arriving).
// Deliberately well above a plausible real round-trip time on this link
// (a full segment plus its ACK, SLIP-framed, at typical baud rates) to
// avoid retransmitting live-but-slow traffic, and well below the 60s stall
// timeout so a single loss doesn't sacrifice the rest of the transfer.
#define NAT_TCP_RETRANSMIT_MS 3000

// Upper cap (bytes) on how much unacknowledged data natPollConnections will
// have in flight toward the client at once. This is a CEILING only - the
// real, binding limit is always the client's own advertised TCP window
// (entry->clientWindow), which must still be respected exactly, including a
// real 0 (client's buffer is full - pause). There is deliberately no
// *floor*: an earlier version of this code forced the effective window up
// to a 1000-byte minimum even when the client advertised less, which is a
// real TCP protocol violation - this NAT has no retransmit buffer, so bytes
// sent past what the client can hold are simply lost with no way to
// recover, leaving serverSeq permanently ahead of what the client will ever
// ACK until the 60s stall timeout tears the connection down. Never
// reintroduce a minimum here.
//
// Capped near one segment rather than left at the client's full advertised
// window: the TCP window only bounds outstanding bytes, not how fast the
// receiver's serial link can physically drain them. On a link with no
// hardware flow control and a slow CPU, a multi-segment burst sent with no
// pacing risks overrunning the Palm's UART receive buffering before its
// TCP stack can process it - a link-layer problem TCP-window correctness
// alone can't fix. This cap forces a real ACK round-trip between
// successive sends, trading some throughput for reliability.
#define NAT_TCP_MAX_WINDOW       1400

// RFC 879's default Maximum Segment Size for IPv4 when a TCP peer's SYN
// carries no explicit MSS option - derived from the required minimum IP
// reassembly buffer size of 576 bytes minus 40 bytes of IP+TCP headers.
// Used as entry->clientMss's fallback when a real MSS option isn't found.
#define NAT_TCP_DEFAULT_MSS      536

// Per-connection heap buffer (bytes) for data read from the remote socket
// but not yet relayed to the client. Lets natPollConnections read greedily
// from the (fast) WiFi/socket side, decoupled from the (slow, small-window)
// serial side, instead of only reading exactly as much as the client's
// window allows on each poll. Sized against the ESP32-S3's large heap
// (>200KB free); worst case is NAT_MAX_TCP_CONNECTIONS (16) * this value,
// though in practice this gateway serves ONE vintage computer at a time -
// the 16 slots exist for that ONE client's several simultaneous connections
// (control + PASV data + any port forwards), not many concurrent users, so
// more than 2-4 slots actually holding a full buffer at once is unlikely.
// Sizing this buffer larger doesn't by itself fix a burst overrunning the
// Palm's own fixed UART/SLIP receive buffering - that's what
// NAT_TCP_MAX_WINDOW's own pacing addresses; this is purely read-ahead
// headroom on the fast side. Degrades gracefully on allocation failure
// (see the malloc null-check in natPollConnections) rather than crashing
// if heap ever gets tight.
#define NAT_TCP_RXBUF_SIZE       16384

// Ephemeral port range for NAT
#define NAT_PORT_START           10000
#define NAT_PORT_END             60000

// ============================================================================
// IP Protocol Numbers
// ============================================================================

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

// ============================================================================
// TCP Flags
// ============================================================================

#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20

// ============================================================================
// TCP NAT Connection States
// ============================================================================

enum NatTcpState {
    NAT_TCP_CLOSED,         // No connection
    NAT_TCP_SYN_SENT,       // SYN received, connecting to remote
    NAT_TCP_ESTABLISHED,    // Connection active
    NAT_TCP_FIN_WAIT,       // FIN received, closing
    NAT_TCP_CLOSING         // Waiting for final cleanup
};

// ============================================================================
// NAT TCP Connection Entry
// ============================================================================

struct NatTcpEntry {
    bool active;
    NatTcpState state;

    // Original endpoint (vintage computer side)
    IPAddress srcIP;        // Vintage computer's IP
    uint16_t srcPort;       // Vintage computer's port

    // Remote endpoint (internet side)
    IPAddress dstIP;        // Remote server IP
    uint16_t dstPort;       // Remote server port

    // Local NAT port (ESP32's ephemeral port)
    uint16_t natPort;

    // TCP sequence tracking (for proper ACK handling)
    uint32_t clientSeq;     // Last sequence from client
    uint32_t clientAck;     // Last ACK from client
    uint32_t serverSeq;     // Last sequence from server
    uint32_t serverAck;     // Last ACK from server

    // Client's advertised Maximum Segment Size, parsed from its SYN's TCP
    // options (kind=2/MSS) if present, else RFC 879's default of 536 when
    // no MSS option is sent at all. A segment larger than this is a real
    // TCP protocol violation - a correct receiver is entitled to silently
    // reject/drop it, regardless of how sends are paced or batched.
    uint16_t clientMss;

    // Set once the vintage computer's OWN completing ACK of the 3-way
    // handshake is received (the ACK that follows our SYN-ACK) - distinct
    // from `state == NAT_TCP_ESTABLISHED`, which this code sets as soon as
    // the GATEWAY's own outbound leg to the real server connects, before
    // the client has necessarily acked anything back. Lets natProcessTcp
    // log a real "client connected" event exactly once per connection.
    bool clientHandshakeAcked;

    // WiFiClient for this connection (null while a non-blocking connect is
    // still pending - see pendingSockFd)
    WiFiClient* client;

    // Non-blocking connect() state (NAT_TCP_SYN_SENT): raw socket fd for a
    // connect() attempt in progress, polled from natPollConnections every
    // loop iteration instead of blocking. -1 when no connect is pending.
    int pendingSockFd;
    unsigned long connectStartTime;

    // Most recent TCP window size advertised by the client (vintage
    // computer), used to size how much unacknowledged data we allow
    // in flight toward it in natPollConnections' send-gating logic.
    uint16_t clientWindow;

    // RX buffer for data from server
    uint8_t* rxBuffer;
    uint16_t rxBufferLen;
    uint16_t rxBufferSize;

    // Timestamps
    unsigned long lastActivity;
    unsigned long lastKeepalive;    // Last keepalive sent
    unsigned long lastServerData;   // Last data received from server
    unsigned long lastProbe;        // Last zero-window probe sent
    unsigned long created;

    // Last time entry->serverAck actually advanced (real forward progress),
    // OR a fresh (non-retransmitted) send happened. This NAT has no way to
    // detect a segment was corrupted/dropped on the flow-control-free serial
    // link except by noticing the client never ack'd it - see the retransmit
    // logic in natPollConnections, gated by NAT_TCP_RETRANSMIT_MS against
    // this field. Distinct from lastActivity, which is touched by unrelated
    // traffic (keepalives, probes, control packets) and would mask a stalled
    // data stream if reused here.
    unsigned long lastAckTime;

    // When this connection FIRST entered NAT_TCP_CLOSING/NAT_TCP_FIN_WAIT -
    // set once on that transition, deliberately NEVER refreshed by later
    // packets (unlike lastActivity, which every incoming packet touches).
    // natCleanupExpired measures the CLOSING/FIN_WAIT timeout from this, not
    // lastActivity: a connection stuck endlessly retransmitting its own
    // close (a real symptom seen on real hardware) would otherwise refresh
    // lastActivity on every single retry, meaning it would NEVER actually
    // expire - and 16 slots (NAT_MAX_TCP_CONNECTIONS) fill up fast when a
    // few connections get stuck like that while new ones keep opening.
    unsigned long closingStartTime;
};

// ============================================================================
// NAT UDP Session Entry
// ============================================================================

struct NatUdpEntry {
    bool active;

    // Original endpoint
    IPAddress srcIP;
    uint16_t srcPort;

    // Remote endpoint
    IPAddress dstIP;
    uint16_t dstPort;

    // Local NAT port
    uint16_t natPort;

    // Timestamp
    unsigned long lastActivity;
};

// ============================================================================
// NAT ICMP Session Entry
// ============================================================================

struct NatIcmpEntry {
    bool active;

    IPAddress srcIP;        // Original source (SLIP client)
    IPAddress dstIP;        // Destination (ping target)
    uint16_t id;            // ICMP identifier
    uint16_t sequence;      // ICMP sequence number

    unsigned long lastActivity;
};

// ============================================================================
// Port Forward Entry
// ============================================================================

struct PortForwardEntry {
    bool active;
    uint8_t protocol;       // IP_PROTO_TCP or IP_PROTO_UDP
    uint16_t externalPort;  // Port on ESP32 (WAN side)
    IPAddress internalIP;   // Destination IP (vintage computer)
    uint16_t internalPort;  // Destination port
};

// ============================================================================
// NAT Gateway Context
// ============================================================================

struct NatContext {
    // IP Configuration (SLIP side)
    IPAddress slipIP;       // ESP32's IP on SLIP side (e.g., 192.168.7.1)
    IPAddress clientIP;     // Vintage computer's IP (e.g., 192.168.7.2)
    IPAddress subnetMask;   // Subnet mask (e.g., 255.255.255.0)
    IPAddress dnsServer;    // DNS server to use

    // Connection tables
    NatTcpEntry tcpTable[NAT_MAX_TCP_CONNECTIONS];
    NatUdpEntry udpTable[NAT_MAX_UDP_SESSIONS];
    NatIcmpEntry icmpTable[NAT_MAX_ICMP_SESSIONS];

    // Port forwarding rules
    PortForwardEntry portForwards[NAT_MAX_PORT_FORWARDS];

    // UDP sockets for outbound NAT - one per active NatUdpEntry (indices
    // match udpTable[]), each bound to that entry's own natPort. A single
    // shared socket can't distinguish two entries active toward the same
    // remote dst:port (e.g. an NTP retry opening a new entry before the
    // prior attempt's entry expired) - a response would match whichever
    // entry a lookup-by-remote-address found first, misdelivering it to the
    // wrong client port. One socket per entry makes the socket itself the
    // identity - whatever arrives on entry i's own socket unambiguously
    // belongs to entry i, no address-based guessing.
    WiFiUDP udpSockets[NAT_MAX_UDP_SESSIONS];
    bool udpSocketBound[NAT_MAX_UDP_SESSIONS];

    // ICMP raw socket for ping NAT (lwIP raw socket)
    void* icmpPcb;          // struct raw_pcb* (opaque to avoid header dependency)

    // Port forwarding servers
    WiFiServer* tcpForwardServers[NAT_MAX_PORT_FORWARDS];

    // Ephemeral port counter
    uint16_t nextPort;

    // Statistics
    uint32_t packetsToInternet;
    uint32_t packetsFromInternet;
    uint32_t droppedPackets;
    uint32_t tcpConnections;
    uint32_t udpSessions;
    uint32_t icmpPackets;
};

// ============================================================================
// IP Header Structure (20 bytes minimum)
// ============================================================================

struct __attribute__((packed)) IpHeader {
    uint8_t  versionIhl;    // Version (4 bits) + IHL (4 bits)
    uint8_t  tos;           // Type of service
    uint16_t totalLength;   // Total length (big endian)
    uint16_t id;            // Identification
    uint16_t flagsFragment; // Flags (3 bits) + Fragment offset (13 bits)
    uint8_t  ttl;           // Time to live
    uint8_t  protocol;      // Protocol (TCP=6, UDP=17, ICMP=1)
    uint16_t checksum;      // Header checksum
    uint32_t srcIP;         // Source IP (big endian)
    uint32_t dstIP;         // Destination IP (big endian)
};

// ============================================================================
// TCP Header Structure (20 bytes minimum)
// ============================================================================

struct __attribute__((packed)) TcpHeader {
    uint16_t srcPort;       // Source port (big endian)
    uint16_t dstPort;       // Destination port (big endian)
    uint32_t seqNum;        // Sequence number
    uint32_t ackNum;        // Acknowledgment number
    uint8_t  dataOffset;    // Data offset (4 bits) + reserved (4 bits)
    uint8_t  flags;         // Control flags
    uint16_t window;        // Window size
    uint16_t checksum;      // Checksum
    uint16_t urgentPtr;     // Urgent pointer
};

// ============================================================================
// UDP Header Structure (8 bytes)
// ============================================================================

struct __attribute__((packed)) UdpHeader {
    uint16_t srcPort;       // Source port (big endian)
    uint16_t dstPort;       // Destination port (big endian)
    uint16_t length;        // Length (big endian)
    uint16_t checksum;      // Checksum
};

// ============================================================================
// ICMP Header Structure
// ============================================================================

struct __attribute__((packed)) IcmpHeader {
    uint8_t  type;          // Message type
    uint8_t  code;          // Message code
    uint16_t checksum;      // Checksum
    uint16_t id;            // Identifier
    uint16_t sequence;      // Sequence number
};

// ICMP types
#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8

// ============================================================================
// Function Declarations
// ============================================================================

// Initialize NAT context
void natInit(NatContext* ctx);

// Shutdown NAT - close all connections
void natShutdown(NatContext* ctx);

// Process incoming IP packet from vintage computer
void natProcessPacket(NatContext* ctx, uint8_t* packet, uint16_t length);

// Poll all active connections for incoming data
// Returns number of packets sent back to vintage computer
int natPollConnections(NatContext* ctx);

// Process pending ICMP replies (call from main loop)
void natProcessPendingIcmp(NatContext* ctx);

// Clean up expired connections
void natCleanupExpired(NatContext* ctx);

// Port forwarding management
int natAddPortForward(NatContext* ctx, uint8_t proto, uint16_t extPort,
                      IPAddress intIP, uint16_t intPort, bool startServer = false);
void natRemovePortForward(NatContext* ctx, int index);
void natStartPortForwardServers(NatContext* ctx);
void natStopPortForwardServers(NatContext* ctx);

// Port forward persistence (EEPROM)
void savePortForwards(NatContext* ctx);
void loadPortForwards(NatContext* ctx);

// Check for incoming port-forwarded connections
void natCheckPortForwards(NatContext* ctx);

// Utility functions
uint16_t ipChecksum(const uint8_t* data, uint16_t length);
void ipRecalcChecksum(IpHeader* ip);
uint16_t tcpUdpChecksum(IpHeader* ip, const uint8_t* payload, uint16_t payloadLen);

// Send IP packet back to vintage computer via SLIP
void natSendToClient(NatContext* ctx, uint8_t* packet, uint16_t length);

// Get active connection counts
int natGetActiveTcpCount(NatContext* ctx);
int natGetActiveUdpCount(NatContext* ctx);

#endif // SLIP_NAT_H
