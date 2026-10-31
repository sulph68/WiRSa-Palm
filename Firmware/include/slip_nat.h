#ifndef SLIP_NAT_H
#define SLIP_NAT_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

// ---- NAT Configuration Constants ----

// Connection table sizes (RAM is limited, ~320 KB in all)
#define NAT_MAX_TCP_CONNECTIONS  16    // Max concurrent TCP connections
#define NAT_MAX_UDP_SESSIONS     32    // Max concurrent UDP sessions (increased for DNS)
#define NAT_MAX_ICMP_SESSIONS    8     // Max concurrent ICMP (ping) sessions
#define NAT_MAX_PORT_FORWARDS    8     // Max port forwarding rules

// Timeout values (milliseconds)
#define NAT_TCP_TIMEOUT_MS       1800000 // 30 minutes for established TCP (IRC, etc)
// Shrunk from 30 s: each NatUdpEntry owns a bound socket, and CONFIG_LWIP_MAX_SOCKETS is a system-wide 16-socket ceiling shared with TCP. An entry that never gets an answer
// held its socket 30 s, and several overlapping starved TCP too. DNS/NTP round trips are under 150 ms, so 5 s leaves margin for a slow server without letting dead queries pile up.
#define NAT_UDP_TIMEOUT_MS       5000    // 5 seconds for UDP
#define NAT_TCP_SYN_TIMEOUT_MS   30000   // 30 seconds for connection setup (outer safety net -
                                          // natPollConnections' NAT_TCP_CONNECT_TIMEOUT_MS check reacts much faster than this in normal operation
#define NAT_ICMP_TIMEOUT_MS      10000   // 10 seconds for ICMP
#define NAT_TCP_KEEPALIVE_MS     300000  // 5 minutes between keepalives
#define NAT_TCP_STALL_TIMEOUT_MS 60000   // 60s stall detection

// How long a NAT_TCP_CLOSING/NAT_TCP_FIN_WAIT connection stays before natCleanupExpired reaps it. The Palm's FTP client (lazyFTP) does not notice a closed connection until its
// NetLibReceive blocking read times out (up to 60 s); reaping sooner makes its delayed FIN or retry hit "no entry" and the transfer look broken though the data arrived. This exceeds that wait.
#define NAT_TCP_CLOSING_TIMEOUT_MS 75000

// Non-blocking connect() poll timeout, checked every loop pass in natPollConnections: the real deadline for a stuck attempt (more generous than WiFiClient's blocking 3 s, since waiting no longer freezes the gateway).
#define NAT_TCP_CONNECT_TIMEOUT_MS 6000

// How often natPollConnections sends a zero-window probe (RFC 1122 4.2.2.17) while the client's window is used up and data waits. A receiver need only ACK an incoming byte, not send an
// unsolicited window update when its buffer drains; if PalmOS's stack does not, nothing prompts an ACK and the connection deadlocks until the 60 s stall timeout. Sending 1 byte past the stated window
// forces an ACK and window either way, so we learn the real state instead of guessing for a minute.
#define NAT_TCP_PROBE_INTERVAL_MS 2000

// How long natPollConnections waits for ACK progress on outstanding data before assuming it was lost on the serial link and re-emitting it from the buffered unacked region (see
// NatTcpEntry::lastAckTime). The link has no flow control or error correction, so a byte can be lost; without this one lost segment stalls the connection until NAT_TCP_STALL_TIMEOUT_MS
// (seen: large FTP listings stopping partway). Well above a plausible round trip (a full SLIP-framed segment plus ACK) so live but slow traffic is not retransmitted, well below the 60 s stall timeout.
#define NAT_TCP_RETRANSMIT_MS 3000          // ceiling of the retransmit timeout
#define NAT_TCP_RETRANSMIT_BASE_MS 700      // floor; the line time of what is outstanding is added (slip_nat.cpp)

// Receive window advertised to the Palm: what the gateway can take from the serial line without waiting for the server. It was 8192, twice the 4096-byte serial buffer, so a Palm sending
// a full window during a slow server write could overrun the buffer.
#define NAT_TCP_ADVERTISED_WINDOW 2048      // when there is no queue (no PSRAM)

// Palm -> server queue per connection, in PSRAM, and the largest window advertised from it: the Palm may have that much in flight, so it can stream at line rate while the server catches up.
#define NAT_TCP_TXQ_SIZE          32768
#define NAT_TCP_TXQ_WINDOW_MAX    8192

// How long one write to the server's socket may hold the main loop (ms).
#define NAT_TCP_WRITE_TIMEOUT_MS 400

// Upper cap (bytes) on unacknowledged data in flight toward the client. A CEILING only: the binding limit is the client's advertised window (entry->clientWindow), a real 0 included.
// There is deliberately no floor: forcing a 1000-byte minimum violated TCP (no retransmit buffer, so bytes past the client's capacity were lost and serverSeq stayed ahead of any ACK until the 60 s
// stall timeout). Never reintroduce a minimum.
// Capped near one segment, not the client's full window: the window bounds outstanding bytes, not how fast the receiver's serial link drains them. With no hardware flow control and a slow CPU,
// an unpaced multi-segment burst can overrun the Palm's UART buffering. This forces an ACK round trip between sends, trading some throughput for reliability.
#define NAT_TCP_MAX_WINDOW       1400

// RFC 879's default MSS for IPv4 without an MSS option: the 576-byte minimum reassembly size minus 40 bytes of IP+TCP headers. The fallback for entry->clientMss.
#define NAT_TCP_DEFAULT_MSS      536

// Per-connection heap buffer (bytes) for data read from the server but not yet relayed to the client. It lets natPollConnections read greedily from the fast WiFi side, apart from the
// slow, small-window serial side. Worst case is NAT_MAX_TCP_CONNECTIONS (16) x this, but the gateway serves ONE vintage computer whose 16 slots cover its several connections (control, PASV data,
// forwards), so 2-4 full buffers at once is the realistic maximum. It does not fix a burst overrunning the Palm's UART buffering (NAT_TCP_MAX_WINDOW's pacing does). Degrades gracefully if allocation fails.
#define NAT_TCP_RXBUF_SIZE       16384

// Ephemeral port range for NAT
#define NAT_PORT_START           10000
#define NAT_PORT_END             60000

// ---- IP Protocol Numbers ----

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

// ---- TCP Flags ----

#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20

// ---- TCP NAT Connection States ----

enum NatTcpState {
    NAT_TCP_CLOSED,         // No connection
    NAT_TCP_SYN_SENT,       // SYN received, connecting to remote
    NAT_TCP_ESTABLISHED,    // Connection active
    NAT_TCP_FIN_WAIT,       // FIN received, closing
    NAT_TCP_CLOSING         // Waiting for final cleanup
};

// ---- NAT TCP Connection Entry ----

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

    // The client's MSS from its SYN's TCP options (kind 2), else RFC 879's 536. A larger segment violates TCP: a correct receiver may drop it however sends are paced.
    uint16_t clientMss;

    // Set when the Palm's own completing ACK of the 3-way handshake arrives (the one after our SYN-ACK). Distinct from state == NAT_TCP_ESTABLISHED, which is set when the GATEWAY's leg to the
    // server connects, before the client has acked anything. Lets natProcessTcp log "client connected" once per connection.
    bool clientHandshakeAcked;

    // WiFiClient for this connection (null while a non-blocking connect is pending, see pendingSockFd)
    WiFiClient* client;

    // Non-blocking connect() state (NAT_TCP_SYN_SENT): the raw socket fd of an attempt in progress, polled from natPollConnections each pass; -1 when none.
    int pendingSockFd;
    unsigned long connectStartTime;

    // The client's latest advertised TCP window, used to size the unacknowledged data allowed in flight (natPollConnections' send gating).
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

    // Last time entry->serverAck advanced (real progress) or a fresh (non-retransmitted) send happened. A segment corrupted or dropped on the flow-control-free link is detected only by the client
    // never acking it (the retransmit logic in natPollConnections, gated by NAT_TCP_RETRANSMIT_MS on this field). Distinct from lastActivity, which unrelated traffic (keepalives, probes, control
    // packets) touches and would mask a stalled stream.
    unsigned long lastAckTime;

    // When the connection FIRST entered NAT_TCP_CLOSING/NAT_TCP_FIN_WAIT: set once and NEVER refreshed (unlike lastActivity, touched by every packet). natCleanupExpired times CLOSING/FIN_WAIT
    // from this: a connection retransmitting its own close forever would refresh lastActivity on every retry and never expire, and the 16 slots (NAT_MAX_TCP_CONNECTIONS) fill fast when a few get stuck.
    unsigned long closingStartTime;

    // Duplicate-ACK count for fast retransmit (natProcessTcp's ACK handling): the same cumulative ACK and window while data is outstanding means the Palm saw a gap.
    uint8_t dupAcks;
    uint16_t lastAckWindow;

    // Palm -> server queue (PSRAM, NAT_TCP_TXQ_SIZE): what the Palm sent that the server socket has not taken. The loop never waits for the server: it queues, acks the Palm at once and
    // drains the queue without blocking, so a slow WiFi moment cannot overflow the serial buffer.
    uint8_t* txq;
    uint32_t txqSize, txqHead, txqLen;
    uint16_t lastAdvWindow;      // the window last advertised to the Palm (to send an update when it opens up)
};

// ---- NAT UDP Session Entry ----

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

// ---- NAT ICMP Session Entry ----

struct NatIcmpEntry {
    bool active;

    IPAddress srcIP;        // Original source (SLIP client)
    IPAddress dstIP;        // Destination (ping target)
    uint16_t id;            // ICMP identifier
    uint16_t sequence;      // ICMP sequence number

    unsigned long lastActivity;
};

// ---- Port Forward Entry ----

struct PortForwardEntry {
    bool active;
    uint8_t protocol;       // IP_PROTO_TCP or IP_PROTO_UDP
    uint16_t externalPort;  // Port on ESP32 (WAN side)
    IPAddress internalIP;   // Destination IP (vintage computer)
    uint16_t internalPort;  // Destination port
};

// ---- NAT Gateway Context ----

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

    // UDP sockets for outbound NAT, one per active NatUdpEntry (indices match udpTable[]), each bound to that entry's natPort. One shared socket could match a reply only by remote address
    // and misdelivered it (e.g. an NTP retry opening a new entry before the old one expired). With one socket per entry, what arrives on entry i's socket belongs to entry i.
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

    // Link-health counters for the SLIP stats line (slip_mode.cpp): what the Palm sent that did not survive, and how often the gateway had to send again.
    uint32_t statIpBadCksum;     // IP header checksum mismatch (corrupted on the serial line)
    uint32_t statTcpBadCksum;    // TCP checksum mismatch
    uint32_t statOutOfSeq;       // client data segments dropped as out of sequence
    uint32_t statRtoRewinds;     // retransmits after a timeout
    uint32_t statFastRetx;       // retransmits after three duplicate ACKs
    uint32_t statDupAcks;        // duplicate ACKs seen
    uint32_t statPartialWrites;  // writes to the server that took less than the whole segment
    uint32_t statQueueFull;      // Palm segments refused because the queue to the server was full
    uint32_t statQueueMax;       // the most bytes the queue to the server held at once
};

// ---- IP Header Structure (20 bytes minimum) ----

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

// ---- TCP Header Structure (20 bytes minimum) ----

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

// ---- UDP Header Structure (8 bytes) ----

struct __attribute__((packed)) UdpHeader {
    uint16_t srcPort;       // Source port (big endian)
    uint16_t dstPort;       // Destination port (big endian)
    uint16_t length;        // Length (big endian)
    uint16_t checksum;      // Checksum
};

// ---- ICMP Header Structure ----

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

// ---- Function Declarations ----

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
