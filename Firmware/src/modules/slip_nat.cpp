// ============================================================================
// SLIP NAT Engine Implementation
// ============================================================================
// Handles NAT translation between vintage computer (SLIP) and WiFi
// Uses TCP proxy approach - WiFiClient handles actual TCP connections
// ============================================================================

#include "slip_nat.h"
#include "slip.h"
#include "globals.h"
#include "serial_io.h"
#include <EEPROM.h>
#include <errno.h>
#include <lwip/sockets.h>  // socket/connect/select/fcntl for non-blocking connect

// lwIP includes for raw ICMP socket
extern "C" {
#include "lwip/raw.h"
#include "lwip/ip_addr.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
}

// Global context pointer for ICMP callback (lwIP callbacks don't have user data)
static NatContext* g_icmpNatCtx = nullptr;

// ============================================================================
// Debug output (controlled by usbDebug setting)
// ============================================================================

extern bool usbDebug;

#define NAT_DEBUG(msg) if (usbDebug) { UsbDebugPrintLn(msg); }
#define NAT_DEBUG_F(fmt, ...) if (usbDebug) { UsbDebugPrint(""); Serial.printf(fmt "\r\n", ##__VA_ARGS__); }

// Forward declarations for internal functions
static void natProcessTcp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen);
static void natProcessUdp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen);
static void natProcessIcmp(NatContext* ctx, uint8_t* packet, uint16_t length,
                           IpHeader* ip, uint8_t ipHeaderLen);
static NatTcpEntry* natFindTcpEntry(NatContext* ctx, IPAddress srcIP,
                                     uint16_t srcPort, IPAddress dstIP,
                                     uint16_t dstPort);
static NatTcpEntry* natCreateTcpEntry(NatContext* ctx, IPAddress srcIP,
                                       uint16_t srcPort, IPAddress dstIP,
                                       uint16_t dstPort);
static void natDumpTcpTable(NatContext* ctx);
static NatUdpEntry* natFindOrCreateUdp(NatContext* ctx, IPAddress srcIP,
                                        uint16_t srcPort, IPAddress dstIP,
                                        uint16_t dstPort);
static void natSendTcpToClient(NatContext* ctx, NatTcpEntry* entry,
                                uint8_t* data, uint16_t length, uint8_t flags);
static void natSendUdpToClient(NatContext* ctx, NatUdpEntry* entry,
                                uint8_t* data, uint16_t length);
static void natSendIcmpToClient(NatContext* ctx, NatIcmpEntry* entry,
                                 uint8_t type, uint8_t code,
                                 uint8_t* data, uint16_t length);
static u8_t natIcmpRecvCallback(void* arg, struct raw_pcb* pcb,
                                 struct pbuf* p, const ip_addr_t* addr);

// External SLIP context (defined in slip_mode.cpp)
extern SlipContext slipCtx;

// ============================================================================
// Initialize NAT Context
// ============================================================================

void natInit(NatContext* ctx) {
    // Set default SLIP subnet (192.168.7.0/24)
    ctx->slipIP = IPAddress(192, 168, 7, 1);
    ctx->clientIP = IPAddress(192, 168, 7, 2);
    ctx->subnetMask = IPAddress(255, 255, 255, 0);
    ctx->dnsServer = IPAddress(8, 8, 8, 8);  // Google DNS

    // Initialize TCP table
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        ctx->tcpTable[i].active = false;
        ctx->tcpTable[i].client = nullptr;
        ctx->tcpTable[i].pendingSockFd = -1;
        ctx->tcpTable[i].clientWindow = NAT_TCP_MAX_WINDOW;
        ctx->tcpTable[i].clientMss = NAT_TCP_DEFAULT_MSS;
        ctx->tcpTable[i].lastAckTime = 0;
        ctx->tcpTable[i].rxBuffer = nullptr;
        ctx->tcpTable[i].rxBufferLen = 0;
        ctx->tcpTable[i].rxBufferSize = 0;
    }

    // Initialize UDP table
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        ctx->udpTable[i].active = false;
    }

    // Initialize ICMP table
    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        ctx->icmpTable[i].active = false;
    }

    // Initialize port forwards
    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        ctx->portForwards[i].active = false;
        ctx->tcpForwardServers[i] = nullptr;
    }

    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        ctx->udpSocketBound[i] = false;
    }
    ctx->nextPort = NAT_PORT_START;

    // Create ICMP raw socket for ping NAT
    ctx->icmpPcb = nullptr;
    struct raw_pcb* pcb = raw_new(IP_PROTO_ICMP);
    if (pcb) {
        raw_recv(pcb, natIcmpRecvCallback, ctx);
        raw_bind(pcb, IP_ADDR_ANY);
        ctx->icmpPcb = pcb;
        NAT_DEBUG("NAT: ICMP raw socket created");
    } else {
        NAT_DEBUG("NAT: Failed to create ICMP raw socket");
    }

    // Set global context for ICMP callback
    g_icmpNatCtx = ctx;

    // Statistics
    ctx->packetsToInternet = 0;
    ctx->packetsFromInternet = 0;
    ctx->droppedPackets = 0;
    ctx->tcpConnections = 0;
    ctx->udpSessions = 0;
    ctx->icmpPackets = 0;
}

// ============================================================================
// Shutdown NAT - Clean up all connections
// ============================================================================

void natShutdown(NatContext* ctx) {
    // Close all TCP connections
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        if (ctx->tcpTable[i].active) {
            if (ctx->tcpTable[i].client) {
                ctx->tcpTable[i].client->stop();
                delete ctx->tcpTable[i].client;
                ctx->tcpTable[i].client = nullptr;
            }
            if (ctx->tcpTable[i].pendingSockFd >= 0) {
                close(ctx->tcpTable[i].pendingSockFd);
                ctx->tcpTable[i].pendingSockFd = -1;
            }
            if (ctx->tcpTable[i].rxBuffer) {
                free(ctx->tcpTable[i].rxBuffer);
                ctx->tcpTable[i].rxBuffer = nullptr;
            }
            ctx->tcpTable[i].active = false;
        }
    }

    // Clear UDP sessions and stop each entry's own socket
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        ctx->udpTable[i].active = false;
        if (ctx->udpSocketBound[i]) {
            ctx->udpSockets[i].stop();
            ctx->udpSocketBound[i] = false;
        }
    }

    // Clear ICMP sessions
    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        ctx->icmpTable[i].active = false;
    }

    // Stop port forward servers
    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        if (ctx->tcpForwardServers[i]) {
            ctx->tcpForwardServers[i]->stop();
            delete ctx->tcpForwardServers[i];
            ctx->tcpForwardServers[i] = nullptr;
        }
    }

    // Close ICMP raw socket
    if (ctx->icmpPcb) {
        raw_remove((struct raw_pcb*)ctx->icmpPcb);
        ctx->icmpPcb = nullptr;
    }
    g_icmpNatCtx = nullptr;
}

// ============================================================================
// IP Checksum Calculation
// ============================================================================

uint16_t ipChecksum(const uint8_t* data, uint16_t length) {
    uint32_t sum = 0;

    // Sum all 16-bit words
    for (uint16_t i = 0; i < length - 1; i += 2) {
        sum += (data[i] << 8) | data[i + 1];
    }

    // Handle odd byte
    if (length & 1) {
        sum += data[length - 1] << 8;
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

// ============================================================================
// Recalculate IP Header Checksum
// ============================================================================

void ipRecalcChecksum(IpHeader* ip) {
    uint8_t headerLen = (ip->versionIhl & 0x0F) * 4;
    ip->checksum = 0;
    ip->checksum = htons(ipChecksum((uint8_t*)ip, headerLen));
}

// ============================================================================
// TCP/UDP Pseudo-header Checksum
// ============================================================================

uint16_t tcpUdpChecksum(IpHeader* ip, const uint8_t* payload, uint16_t payloadLen) {
    uint32_t sum = 0;

    // Pseudo-header: source IP
    sum += (ntohl(ip->srcIP) >> 16) & 0xFFFF;
    sum += ntohl(ip->srcIP) & 0xFFFF;

    // Pseudo-header: destination IP
    sum += (ntohl(ip->dstIP) >> 16) & 0xFFFF;
    sum += ntohl(ip->dstIP) & 0xFFFF;

    // Pseudo-header: protocol and length
    sum += ip->protocol;
    sum += payloadLen;

    // Sum payload
    for (uint16_t i = 0; i < payloadLen - 1; i += 2) {
        sum += (payload[i] << 8) | payload[i + 1];
    }
    if (payloadLen & 1) {
        sum += payload[payloadLen - 1] << 8;
    }

    // Fold and complement
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

// ============================================================================
// Process Incoming IP Packet from Vintage Computer
// ============================================================================

void natProcessPacket(NatContext* ctx, uint8_t* packet, uint16_t length) {
    // Minimum IP header is 20 bytes
    if (length < 20) {
        NAT_DEBUG_F("NAT: Packet too short (%d bytes)", length);
        ctx->droppedPackets++;
        return;
    }

    IpHeader* ip = (IpHeader*)packet;

    // Check IP version (must be 4)
    uint8_t version = (ip->versionIhl >> 4) & 0x0F;
    if (version != 4) {
        NAT_DEBUG_F("NAT: Invalid IP version %d", version);
        ctx->droppedPackets++;
        return;
    }

    // Get header length
    uint8_t headerLen = (ip->versionIhl & 0x0F) * 4;
    if (headerLen < 20 || headerLen > length) {
        NAT_DEBUG_F("NAT: Invalid header length %d", headerLen);
        ctx->droppedPackets++;
        return;
    }

    // Verify checksum
    uint16_t originalChecksum = ip->checksum;
    ip->checksum = 0;
    uint16_t calculatedChecksum = htons(ipChecksum(packet, headerLen));
    ip->checksum = originalChecksum;

    if (originalChecksum != calculatedChecksum) {
        // Bad checksum - drop packet
        NAT_DEBUG_F("NAT: IP checksum mismatch: got 0x%04X, calc 0x%04X",
                    ntohs(originalChecksum), ntohs(calculatedChecksum));
        ctx->droppedPackets++;
        return;
    }

    // Log incoming packet info
    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    NAT_DEBUG_F("NAT: RX proto=%d %d.%d.%d.%d -> %d.%d.%d.%d len=%d",
                ip->protocol,
                srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3],
                dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3],
                length);

    // Get total length from header
    uint16_t totalLength = ntohs(ip->totalLength);
    if (totalLength > length) {
        ctx->droppedPackets++;
        return;
    }

    // Route based on protocol
    switch (ip->protocol) {
        case IP_PROTO_TCP:
            natProcessTcp(ctx, packet, totalLength, ip, headerLen);
            break;
        case IP_PROTO_UDP:
            natProcessUdp(ctx, packet, totalLength, ip, headerLen);
            break;
        case IP_PROTO_ICMP:
            natProcessIcmp(ctx, packet, totalLength, ip, headerLen);
            break;
        default:
            // Unknown protocol - drop
            ctx->droppedPackets++;
            break;
    }
}

// ============================================================================
// Process TCP Packet
// ============================================================================

// Scan a SYN packet's TCP options for a Maximum Segment Size option
// (kind=2, length=4, 2-byte value) and return it, or 0 if absent/malformed.
// optionsLen is tcpHeaderLen-20 (the fixed header is never part of this scan).
static uint16_t natParseMss(const uint8_t* options, uint8_t optionsLen) {
    uint8_t i = 0;
    while (i < optionsLen) {
        uint8_t kind = options[i];
        if (kind == 0) {
            break;  // end of options list
        }
        if (kind == 1) {
            i++;  // no-op, 1 byte, no length field
            continue;
        }
        if (i + 1 >= optionsLen) {
            break;  // truncated option
        }
        uint8_t optLen = options[i + 1];
        if (optLen < 2 || i + optLen > optionsLen) {
            break;  // malformed
        }
        if (kind == 2 && optLen == 4) {
            uint16_t mss = ((uint16_t)options[i + 2] << 8) | options[i + 3];
            return mss;
        }
        i += optLen;
    }
    return 0;
}

static void natProcessTcp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen) {
    // Minimum TCP header is 20 bytes
    if (length < ipHeaderLen + 20) {
        ctx->droppedPackets++;
        return;
    }

    TcpHeader* tcp = (TcpHeader*)(packet + ipHeaderLen);
    uint16_t srcPort = ntohs(tcp->srcPort);
    uint16_t dstPort = ntohs(tcp->dstPort);
    // Construct IPs from network byte order (bytes are already in correct order)
    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    IPAddress dstIP(dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3]);
    IPAddress srcIP(srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3]);

    // Get TCP header length
    uint8_t tcpHeaderLen = ((tcp->dataOffset >> 4) & 0x0F) * 4;
    if (tcpHeaderLen < 20 || ipHeaderLen + tcpHeaderLen > length) {
        ctx->droppedPackets++;
        return;
    }

    // Verify TCP checksum. natProcessPacket only validates the IP HEADER's
    // own checksum (20 bytes) - everything past it, including seqNum/
    // ackNum/flags/window and the actual payload, was previously trusted
    // completely unverified. On a real, physically noisy serial link
    // (confirmed by the IP-checksum-mismatch and "Invalid IP version"
    // drops already logged elsewhere in this function's caller), a bit
    // flip landing anywhere in that ~96%-of-the-packet region (a typical
    // 576-byte data segment is only 20 bytes of IP header) sailed straight
    // through and got acted on as if genuine - corrupted payload bytes
    // forwarded to the real server as-is (explains uploads landing at the
    // right SIZE with the wrong CONTENT), and a corrupted ackNum/seqNum
    // silently desyncing this connection's flow-control state (observed
    // live: a serverAck that jumped BACKWARDS - 1306060478 -> 1306060338 -
    // which the ACK-update code below accepted unconditionally). Unlike
    // the IP header check, TCP checksums are NOT optional - always verify.
    {
        uint16_t segLen = (uint16_t)(length - ipHeaderLen);  // TCP header + payload
        uint16_t origChecksum = tcp->checksum;
        tcp->checksum = 0;
        uint16_t calcChecksum = htons(tcpUdpChecksum(ip, (uint8_t*)tcp, segLen));
        tcp->checksum = origChecksum;
        if (origChecksum != calcChecksum) {
            NAT_DEBUG_F("NAT: TCP checksum mismatch: got 0x%04X, calc 0x%04X - dropping",
                        ntohs(origChecksum), ntohs(calcChecksum));
            ctx->droppedPackets++;
            return;
        }
    }

    // Get flags
    uint8_t flags = tcp->flags;

    // Calculate payload length
    uint16_t payloadLen = length - ipHeaderLen - tcpHeaderLen;
    uint8_t* payload = packet + ipHeaderLen + tcpHeaderLen;

    // Find existing connection or create new one on SYN
    NatTcpEntry* entry = natFindTcpEntry(ctx, srcIP, srcPort, dstIP, dstPort);

    // Debug: always log TCP packet details
    NAT_DEBUG_F("NAT: TCP %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d flags=0x%02X entry=%s",
                srcIP[0], srcIP[1], srcIP[2], srcIP[3], srcPort,
                dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort,
                flags, entry ? "found" : "null");

    if (flags & TCP_FLAG_SYN) {
        // Debug: log SYN packet details
        NAT_DEBUG_F("NAT: TCP SYN state=%d", entry ? entry->state : -1);

        // Check if this is a SYN-ACK response (for port forwarding)
        if ((flags & TCP_FLAG_ACK) && entry && entry->state == NAT_TCP_ESTABLISHED) {
            // This is a SYN-ACK from the vintage computer responding to our SYN
            // (port forwarding case). Complete the 3-way handshake.
            entry->clientSeq = ntohl(tcp->seqNum) + 1;
            entry->clientAck = ntohl(tcp->ackNum);

            // Update serverAck - vintage computer acknowledged our SYN
            // This is critical for flow control to work
            entry->serverAck = ntohl(tcp->ackNum);

            entry->lastActivity = millis();
            entry->lastKeepalive = millis();

            NAT_DEBUG_F("NAT: Port forward SYN-ACK received, sending ACK (clientSeq=%u, serverAck=%u)",
                        entry->clientSeq, entry->serverAck);

            // Send ACK to complete 3-way handshake with vintage computer
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
            ctx->tcpConnections++;
            return;
        }

        // New connection request
        if (!entry) {
            // Check for stuck connection to same destination - helps with reconnection
            for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
                NatTcpEntry* e = &ctx->tcpTable[i];
                if (e->active && e->srcIP == srcIP && e->dstIP == dstIP && e->dstPort == dstPort) {
                    NAT_DEBUG_F("NAT: Closing stuck connection to same dest (port %d)", e->srcPort);
                    if (e->client) {
                        e->client->stop();
                        delete e->client;
                        e->client = nullptr;
                    }
                    if (e->pendingSockFd >= 0) {
                        close(e->pendingSockFd);
                        e->pendingSockFd = -1;
                    }
                    if (e->rxBuffer) {
                        free(e->rxBuffer);
                        e->rxBuffer = nullptr;
                        e->rxBufferLen = 0;
                        e->rxBufferSize = 0;
                    }
                    e->active = false;
                }
            }

            entry = natCreateTcpEntry(ctx, srcIP, srcPort, dstIP, dstPort);
            if (!entry) {
                // Table full
                ctx->droppedPackets++;
                NAT_DEBUG("NAT: TCP table full");
                natDumpTcpTable(ctx);
                return;
            }
        }

        // Store initial sequence number (+1 because SYN consumes 1 seq number)
        entry->clientSeq = ntohl(tcp->seqNum) + 1;
        entry->clientAck = ntohl(tcp->ackNum);
        // A window of 0 here is legitimate (client says "don't send yet") and
        // must be honored, not skipped - see the natPollConnections send-gate.
        entry->clientWindow = ntohs(tcp->window);

        // Parse the client's advertised MSS from the SYN's TCP options, if
        // present. Real-device evidence: this NAT used to hardcode a
        // 1000-byte segment cap regardless of what the client actually
        // agreed to accept - a real TCP protocol violation once the client's
        // true MSS is smaller (RFC 879 defaults to 536 when no MSS option is
        // sent at all). A correct receiver may silently drop any segment
        // larger than what it advertised, which matched the observed
        // pattern exactly: the handshake (header-only, no payload) always
        // worked, but segments actually carrying ~1000 bytes never got
        // acknowledged, regardless of pacing or burst size.
        {
            uint8_t optionsLen = tcpHeaderLen - 20;
            uint16_t parsedMss = (optionsLen > 0)
                ? natParseMss(packet + ipHeaderLen + 20, optionsLen)
                : 0;
            entry->clientMss = (parsedMss > 0) ? parsedMss : NAT_TCP_DEFAULT_MSS;
            NAT_DEBUG_F("NAT: TCP SYN clientMss=%u (parsed=%u)", entry->clientMss, parsedMss);
        }

        // Initiate connection to remote server. NOTE: this used to be a
        // blocking WiFiClient::connect() (default 3s timeout), which froze
        // the ENTIRE gateway - no UART draining, no relay of any other
        // already-open connection's data - for the whole call. Worse, a
        // failed/slow attempt marked the entry inactive, so the vintage
        // computer's own TCP retransmitted SYN (sent because it never got a
        // timely SYN-ACK) was treated as a brand new connection request,
        // re-triggering another full blocking attempt - a cascade visible as
        // a burst of connection attempts in gateway traffic. Now we open a
        // non-blocking socket and hand it to natPollConnections to finish
        // asynchronously (see the NAT_TCP_SYN_SENT branch there); a
        // retransmitted SYN arriving while pendingSockFd is still valid just
        // falls through the "entry->state == NAT_TCP_CLOSED" check below
        // (state is SYN_SENT by then) and is silently absorbed, same as it
        // would be for an already-established connection.
        if (entry->state == NAT_TCP_CLOSED) {
            // Hairpin/self-connect fixup: if the vintage computer is
            // connecting to an address that identifies WiRSa itself (its
            // real WiFi IP, its AP IP, or the SLIP-side gateway address
            // presented to the client as ctx->slipIP), redirect the actual
            // outbound connect() to loopback (127.0.0.1) instead of the
            // real IP. Without this, connecting to WiRSa's own WiFi STA IP
            // requires the packet to leave over the WiFi radio and be
            // reflected back by the AP to the same station that sent it -
            // most APs/WiFi drivers don't support that (client isolation,
            // or the station's own driver dropping frames that loop back to
            // itself), so the handshake can partially/sporadically succeed
            // while actual response payloads never make the round trip -
            // this was the observed symptom talking to the newly-added FTP
            // server over the SLIP gateway (server-side logs showed it
            // responding, but nothing reached natSendTcpToClient()).
            // WiFiServer/FtpServer bind INADDR_ANY, so a loopback connection
            // reaches the exact same listening socket without ever touching
            // the WiFi driver.
            IPAddress connectIP = dstIP;
            bool isSelf = (dstIP == ctx->slipIP) ||
                          (WiFi.status() == WL_CONNECTED && dstIP == WiFi.localIP()) ||
                          (WiFi.getMode() != WIFI_STA && dstIP == WiFi.softAPIP());
            if (isSelf) {
                connectIP = IPAddress(127, 0, 0, 1);
                NAT_DEBUG_F("NAT: TCP dst %d.%d.%d.%d:%d is WiRSa itself - redirecting connect to loopback",
                            dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort);
            }

            NAT_DEBUG_F("NAT: TCP connecting (async) to %d.%d.%d.%d:%d",
                        connectIP[0], connectIP[1], connectIP[2], connectIP[3], dstPort);

            int sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                NAT_DEBUG("NAT: TCP socket() failed");
                natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_RST);
                entry->active = false;
                return;
            }
            fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = (uint32_t)connectIP;
            addr.sin_port = htons(dstPort);

            int res = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
            if (res < 0 && errno != EINPROGRESS) {
                NAT_DEBUG_F("NAT: TCP connect() failed immediately, errno=%d", errno);
                close(sockfd);
                natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_RST);
                entry->active = false;
                return;
            }

            // Connect is in progress (the normal case) or completed
            // immediately (rare, e.g. loopback-like cases) - either way,
            // natPollConnections' NAT_TCP_SYN_SENT branch finishes the
            // handshake without ever blocking this loop.
            entry->state = NAT_TCP_SYN_SENT;
            entry->created = millis();
            entry->lastActivity = millis();
            entry->lastKeepalive = millis();
            entry->pendingSockFd = sockfd;
            entry->connectStartTime = millis();
        }
        return;
    }

    if (!entry) {
        // No connection for this packet
        ctx->droppedPackets++;
        NAT_DEBUG_F("NAT: TCP no entry for %d.%d.%d.%d:%d",
                    dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort);
        return;
    }

    entry->lastActivity = millis();

    // Handle FIN
    if (flags & TCP_FLAG_FIN) {
        // Was WE who already initiated closing this connection (e.g. the
        // "connection closed by remote" path in natPollConnections already
        // sent our own FIN), before this client FIN - possibly a genuinely
        // simultaneous close, possibly the client's FIN just took a while to
        // arrive/get retransmitted. Must be captured BEFORE the state
        // assignment just below overwrites it.
        bool alreadyClosing = (entry->state == NAT_TCP_FIN_WAIT ||
                                entry->state == NAT_TCP_CLOSING);

        entry->state = NAT_TCP_FIN_WAIT;
        // A FIN consumes exactly one sequence number, just like a SYN (RFC
        // 793) - matching the SYN handler above (`clientSeq = seqNum + 1`).
        // A client whose FIN isn't acked at the correct sequence number
        // will retransmit it indefinitely rather than treat the ack as valid.
        entry->clientSeq = ntohl(tcp->seqNum) + 1;
        if (entry->client) {
            entry->client->stop();
        }
        if (entry->pendingSockFd >= 0) {
            // Client is closing a connection whose async connect() never
            // finished - abandon it so the fd isn't leaked.
            close(entry->pendingSockFd);
            entry->pendingSockFd = -1;
        }
        if (alreadyClosing) {
            // We already sent our own FIN earlier. A peer retransmitting a
            // segment we apparently didn't ack must get the identical
            // segment back, not a bare ACK or a fresh FIN with a different
            // sequence number - PalmOS's TCP stack doesn't treat either
            // substitute as acknowledging its FIN. Re-send our original FIN
            // byte-for-byte unchanged (same sequence number) by saving/
            // restoring serverSeq around the call, undoing
            // natSendTcpToClient's usual advance.
            uint32_t savedServerSeq = entry->serverSeq;
            NAT_DEBUG_F("NAT: TCP FIN (already closing) -> retransmit FIN|ACK, serverSeq=%u",
                        entry->serverSeq);
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
            entry->serverSeq = savedServerSeq;
        } else {
            // First time seeing a close on this connection - ACK the
            // client's FIN and send ours in the same reply.
            entry->closingStartTime = millis();
            NAT_DEBUG_F("NAT: TCP FIN (first close) -> FIN|ACK, serverSeq=%u",
                        entry->serverSeq);
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
        }
        return;
    }

    // Handle RST
    if (flags & TCP_FLAG_RST) {
        if (entry->client) {
            entry->client->stop();
            delete entry->client;
            entry->client = nullptr;
        }
        if (entry->pendingSockFd >= 0) {
            // Client aborted a connection whose async connect() was still
            // in progress - close the fd rather than leaking it.
            close(entry->pendingSockFd);
            entry->pendingSockFd = -1;
        }
        if (entry->rxBuffer) {
            free(entry->rxBuffer);
            entry->rxBuffer = nullptr;
            entry->rxBufferLen = 0;
            entry->rxBufferSize = 0;
        }
        entry->active = false;
        return;
    }

    // Handle data from client
    if (payloadLen > 0 && entry->state == NAT_TCP_ESTABLISHED) {
        uint32_t segSeq = ntohl(tcp->seqNum);

        if (segSeq != entry->clientSeq) {
            // Not the next byte expected - a stale retransmit (the client
            // resent because it never saw our ACK - very plausible now
            // that the checksum check above drops more corrupted packets,
            // including ACK-only ones, than before) or a genuinely out-of-
            // order arrival. This relay has no reorder/reassembly buffer
            // for this direction, so there's no safe place to splice an
            // out-of-position segment into the outgoing stream - blindly
            // forwarding it (the old behavior) duplicates/misorders bytes
            // in what the real server receives, with no way for it to
            // detect this since it just sees a plain byte stream. Drop and
            // re-ACK our current cumulative position instead, so the
            // client's own (correct) retransmit timer does the recovery.
            int32_t delta = (int32_t)(segSeq - entry->clientSeq);
            NAT_DEBUG_F("NAT: TCP client->server out-of-sequence: got seq=%u expected=%u (delta=%ld) - dropping, re-ACKing",
                        segSeq, entry->clientSeq, (long) delta);
            entry->lastActivity = millis();
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
        } else if (entry->client && entry->client->connected()) {
            size_t written = entry->client->write(payload, payloadLen);
            if (written != payloadLen) {
                NAT_DEBUG_F("NAT: TCP client->server write failed: %d/%d bytes", written, payloadLen);
            } else {
                NAT_DEBUG_F("NAT: TCP client->server: %d bytes", payloadLen);
            }
            entry->clientSeq = segSeq + payloadLen;
            ctx->packetsToInternet++;
            entry->lastActivity = millis();

            // Send ACK back
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
        } else {
            NAT_DEBUG("NAT: TCP client->server failed: not connected");
        }
    }

    // Update ACK tracking - client's ACK acknowledges data WE sent
    if (flags & TCP_FLAG_ACK) {
        entry->lastActivity = millis();

        // `state == NAT_TCP_ESTABLISHED` gets set as soon as the GATEWAY's
        // own outbound leg to the real server connects (natPollPendingConnect)
        // - that only proves WE reached the server, not that the vintage
        // computer has actually completed ITS side of the handshake yet.
        // This is the first ACK-flagged packet seen from the client after
        // that point - the genuine "client connected" event answering
        // exactly that question, logged once per connection (not on every
        // subsequent ACK, which would just be repeat noise).
        if (!entry->clientHandshakeAcked && entry->state == NAT_TCP_ESTABLISHED) {
            entry->clientHandshakeAcked = true;
            NAT_DEBUG_F("NAT: *** Client CONNECTED: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d ***",
                        srcIP[0], srcIP[1], srcIP[2], srcIP[3], srcPort,
                        dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort);
        }

        uint32_t newAck = ntohl(tcp->ackNum);
        if (newAck != entry->serverAck) {
            // Signed delta (TCP-style modular comparison) so a stale/
            // reordered ACK arriving after a later one can't be mistaken
            // for forward progress and corrupt the buffer trim below.
            int32_t delta = (int32_t)(newAck - entry->serverAck);
            if (delta < 0) {
                // A stale/reordered/duplicate ACK - can happen on any real
                // network even with a valid checksum (simple reordering),
                // not just corruption. MUST NOT be allowed to roll
                // serverAck backwards: entry->serverAck feeds
                // `inFlight = serverSeq - serverAck` (unsigned) elsewhere -
                // regressing it makes inFlight read larger than reality,
                // and if a bad enough regression ever pushed serverAck
                // *above* serverSeq that subtraction would wrap to a huge
                // value instead of going negative. Ignore it entirely
                // rather than accept a going-backwards "update".
                NAT_DEBUG_F("NAT: TCP stale ACK ignored: serverAck=%u, got=%u (delta=%ld)",
                            entry->serverAck, newAck, (long) delta);
            } else {
                NAT_DEBUG_F("NAT: TCP ACK update: serverAck %u -> %u", entry->serverAck, newAck);
                entry->serverAck = newAck;

                if (entry->rxBuffer) {
                    // Real forward progress: the client has now confirmed
                    // receipt of `delta` more bytes. Only NOW is it safe to
                    // discard them from the front of rxBuffer - see the
                    // natPollConnections send path, which deliberately leaves
                    // sent-but-unacknowledged bytes in place (instead of
                    // discarding on send) so they can be retransmitted if the
                    // client never acks them. Clamp defensively: the client
                    // should never ack more than we've actually buffered.
                    uint32_t ackedBytes = (uint32_t)delta;
                    if (ackedBytes > entry->rxBufferLen) ackedBytes = entry->rxBufferLen;
                    if (ackedBytes > 0) {
                        uint16_t remaining = entry->rxBufferLen - (uint16_t)ackedBytes;
                        if (remaining > 0) {
                            memmove(entry->rxBuffer, entry->rxBuffer + ackedBytes, remaining);
                        }
                        entry->rxBufferLen = remaining;
                    }
                }
                entry->lastAckTime = millis();
            }
        }
        // Track the client's advertised receive window so natPollConnections
        // can size its sliding window from what the client actually says it
        // can buffer, instead of a single fixed guess. A real 0 here (the
        // client's own buffer is full) must be honored, not skipped - a
        // previous version of this code ignored zero windows, which let the
        // gateway keep sending past what the client could actually hold
        // (there's no retransmit buffer to recover the loss from), causing a
        // permanent inFlight/serverAck desync that only a 60s stall-timeout
        // teardown would ever clear.
        entry->clientWindow = ntohs(tcp->window);
    }
}

// ============================================================================
// Process UDP Packet
// ============================================================================

static void natProcessUdp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen) {
    // UDP header is 8 bytes
    if (length < ipHeaderLen + 8) {
        ctx->droppedPackets++;
        return;
    }

    UdpHeader* udp = (UdpHeader*)(packet + ipHeaderLen);
    uint16_t srcPort = ntohs(udp->srcPort);
    uint16_t dstPort = ntohs(udp->dstPort);
    uint16_t udpLen = ntohs(udp->length);
    // Construct IPs from network byte order (bytes are already in correct order)
    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    IPAddress dstIP(dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3]);
    IPAddress srcIP(srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3]);

    if (udpLen < 8 || ipHeaderLen + udpLen > length) {
        ctx->droppedPackets++;
        return;
    }

    // Verify UDP checksum, same rationale as the TCP check in natProcessTcp -
    // only the IP header was ever validated before, leaving the UDP header/
    // payload (almost the entire packet) trusted unverified. UDP checksums
    // ARE optional per RFC 768 (0 = "not computed") - unlike TCP, a 0 must
    // be accepted as-is, not treated as a mismatch.
    if (udp->checksum != 0) {
        uint16_t origChecksum = udp->checksum;
        udp->checksum = 0;
        uint16_t calcChecksum = htons(tcpUdpChecksum(ip, (uint8_t*)udp, udpLen));
        udp->checksum = origChecksum;
        if (origChecksum != calcChecksum) {
            NAT_DEBUG_F("NAT: UDP checksum mismatch: got 0x%04X, calc 0x%04X - dropping",
                        ntohs(origChecksum), ntohs(calcChecksum));
            ctx->droppedPackets++;
            return;
        }
    }

    uint16_t payloadLen = udpLen - 8;
    uint8_t* payload = packet + ipHeaderLen + 8;

    // Always log the request side (previously silent - TCP has this kind of
    // logging throughout, UDP had none at all, making it impossible to tell
    // whether a client packet - e.g. a DNS query - ever reached the gateway).
    NAT_DEBUG_F("NAT: UDP %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d len=%d%s",
                srcIP[0], srcIP[1], srcIP[2], srcIP[3], srcPort,
                dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort, payloadLen,
                dstPort == 53 ? " (DNS)" : "");

    // Find or create UDP session
    NatUdpEntry* entry = natFindOrCreateUdp(ctx, srcIP, srcPort, dstIP, dstPort);
    if (!entry) {
        NAT_DEBUG("NAT: UDP table full - dropping request");
        ctx->droppedPackets++;
        return;
    }
    int udpIdx = (int)(entry - ctx->udpTable);

    entry->lastActivity = millis();

    // Bind this entry's OWN UDP socket if not already - see NatContext::
    // udpSockets for why every entry gets its own socket instead of sharing
    // one, keyed to the natPort already allocated for it in natFindOrCreateUdp.
    if (!ctx->udpSocketBound[udpIdx]) {
        if (!ctx->udpSockets[udpIdx].begin(entry->natPort)) {
            NAT_DEBUG_F("NAT: UDP socket begin(%u) FAILED", entry->natPort);
            ctx->droppedPackets++;
            return;
        }
        ctx->udpSocketBound[udpIdx] = true;
    }

    // Send UDP packet to remote - check every step; previously none of these
    // were checked, so a routing/WiFi failure here failed completely
    // silently (packet just vanished, no counter, no log, nothing to show
    // the request was even attempted).
    if (WiFi.status() != WL_CONNECTED) {
        NAT_DEBUG_F("NAT: UDP forward to %d.%d.%d.%d:%d ABORTED - WiFi not connected (status=%d)",
                    dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort, (int) WiFi.status());
        ctx->droppedPackets++;
        return;
    }
    if (!ctx->udpSockets[udpIdx].beginPacket(dstIP, dstPort)) {
        NAT_DEBUG_F("NAT: UDP beginPacket to %d.%d.%d.%d:%d FAILED",
                    dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort);
        ctx->droppedPackets++;
        return;
    }
    size_t written = ctx->udpSockets[udpIdx].write(payload, payloadLen);
    if (written != payloadLen) {
        NAT_DEBUG_F("NAT: UDP write short: %u/%u bytes", (unsigned) written, payloadLen);
    }
    if (!ctx->udpSockets[udpIdx].endPacket()) {
        NAT_DEBUG_F("NAT: UDP endPacket to %d.%d.%d.%d:%d FAILED (send did not complete)",
                    dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort);
        ctx->droppedPackets++;
        return;
    }

    ctx->packetsToInternet++;
}

// ============================================================================
// Pending ICMP Reply Buffer (for deferred processing from callback)
// ============================================================================

static struct {
    volatile bool pending;  // volatile: written by lwIP callback (Core 0), read by main loop (Core 1)
    IPAddress srcIP;
    uint16_t id;
    uint16_t seq;
    uint8_t data[64];   // Ping payload (typically 32 bytes)
    uint16_t dataLen;
} g_pendingIcmpReply = {false};

// ============================================================================
// ICMP Raw Socket Receive Callback
// ============================================================================

static u8_t natIcmpRecvCallback(void* arg, struct raw_pcb* pcb,
                                 struct pbuf* p, const ip_addr_t* addr) {
    (void)arg;  // Use global context instead

    // pbuf includes IP header - we need to skip it
    if (p->tot_len < 28) {  // Min IP (20) + Min ICMP (8)
        return 0;
    }

    uint8_t* ipData = (uint8_t*)p->payload;
    uint8_t ipHeaderLen = (ipData[0] & 0x0F) * 4;

    if (p->tot_len < ipHeaderLen + 8) {
        return 0;
    }

    // Get ICMP header after IP header
    struct icmp_echo_hdr* icmpHdr = (struct icmp_echo_hdr*)(ipData + ipHeaderLen);

    // Only handle echo reply (type 0)
    if (icmpHdr->type != 0) {
        return 0;
    }

    uint16_t icmpId = ntohs(icmpHdr->id);
    uint16_t icmpSeq = ntohs(icmpHdr->seqno);

    // Get source IP from addr
    const ip4_addr_t* ip4 = &addr->u_addr.ip4;
    IPAddress srcIP((ip4->addr >> 0) & 0xFF, (ip4->addr >> 8) & 0xFF,
                    (ip4->addr >> 16) & 0xFF, (ip4->addr >> 24) & 0xFF);

    // Store reply info for deferred processing
    if (!g_pendingIcmpReply.pending) {
        g_pendingIcmpReply.srcIP = srcIP;
        g_pendingIcmpReply.id = icmpId;
        g_pendingIcmpReply.seq = icmpSeq;

        // Copy ping payload (after 8-byte ICMP header)
        uint16_t payloadLen = p->tot_len - ipHeaderLen - 8;
        if (payloadLen > sizeof(g_pendingIcmpReply.data)) {
            payloadLen = sizeof(g_pendingIcmpReply.data);
        }
        g_pendingIcmpReply.dataLen = payloadLen;
        if (payloadLen > 0) {
            pbuf_copy_partial(p, g_pendingIcmpReply.data, payloadLen, ipHeaderLen + 8);
        }
        g_pendingIcmpReply.pending = true;  // Set AFTER all fields populated (volatile ordering)
    }

    // CRITICAL: Must free pbuf when returning 1 (consumed).
    // lwIP raw_recv contract: callback owns the pbuf when it returns non-zero.
    // Missing this causes progressive memory leak that exhausts lwIP pbuf pool.
    pbuf_free(p);
    return 1;  // Consumed - we handle all echo replies
}

// ============================================================================
// Process Pending ICMP Reply (called from main loop)
// ============================================================================

void natProcessPendingIcmp(NatContext* ctx) {
    if (!g_pendingIcmpReply.pending) {
        return;
    }

    NAT_DEBUG_F("NAT: Processing ICMP reply from %d.%d.%d.%d id=%d seq=%d",
                g_pendingIcmpReply.srcIP[0], g_pendingIcmpReply.srcIP[1],
                g_pendingIcmpReply.srcIP[2], g_pendingIcmpReply.srcIP[3],
                g_pendingIcmpReply.id, g_pendingIcmpReply.seq);

    // Find matching NAT entry
    NatIcmpEntry* entry = nullptr;
    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        if (ctx->icmpTable[i].active &&
            ctx->icmpTable[i].dstIP == g_pendingIcmpReply.srcIP &&
            ctx->icmpTable[i].id == g_pendingIcmpReply.id) {
            entry = &ctx->icmpTable[i];
            NAT_DEBUG_F("NAT: Matched ICMP entry %d", i);
            break;
        }
    }

    if (!entry) {
        NAT_DEBUG("NAT: No matching ICMP entry for reply");
        g_pendingIcmpReply.pending = false;
        return;
    }

    // Update entry with sequence from reply
    entry->sequence = g_pendingIcmpReply.seq;

    // Send reply to SLIP client
    natSendIcmpToClient(ctx, entry, ICMP_ECHO_REPLY, 0,
                         g_pendingIcmpReply.data, g_pendingIcmpReply.dataLen);

    NAT_DEBUG_F("NAT: Forwarded ping reply to client, %d bytes payload",
                g_pendingIcmpReply.dataLen);
    ctx->icmpPackets++;

    // Mark entry inactive after reply
    entry->active = false;

    g_pendingIcmpReply.pending = false;
}

// ============================================================================
// Process ICMP Packet (Echo Request/Reply)
// ============================================================================

static void natProcessIcmp(NatContext* ctx, uint8_t* packet, uint16_t length,
                           IpHeader* ip, uint8_t ipHeaderLen) {
    // ICMP header is 8 bytes minimum
    if (length < ipHeaderLen + 8) {
        ctx->droppedPackets++;
        return;
    }

    IcmpHeader* icmp = (IcmpHeader*)(packet + ipHeaderLen);

    // Only handle Echo Request (ping)
    if (icmp->type != ICMP_ECHO_REQUEST) {
        NAT_DEBUG_F("NAT: ICMP type %d dropped", icmp->type);
        ctx->droppedPackets++;
        return;
    }

    // Construct IPs from network byte order (bytes are already in correct order)
    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    IPAddress dstIP(dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3]);
    IPAddress srcIP(srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3]);

    NAT_DEBUG_F("NAT: ICMP echo request to %d.%d.%d.%d",
                dstIP[0], dstIP[1], dstIP[2], dstIP[3]);

    // Check if ping is for our gateway IP
    if (dstIP == ctx->slipIP) {
        // Respond directly
        uint16_t icmpLen = length - ipHeaderLen;

        // Swap IPs
        uint32_t tmp = ip->srcIP;
        ip->srcIP = ip->dstIP;
        ip->dstIP = tmp;

        // Change to echo reply
        icmp->type = ICMP_ECHO_REPLY;
        icmp->checksum = 0;
        icmp->checksum = htons(ipChecksum((uint8_t*)icmp, icmpLen));

        // Recalc IP checksum
        ip->ttl = 64;
        ipRecalcChecksum(ip);

        // Send back
        natSendToClient(ctx, packet, length);
        ctx->packetsFromInternet++;
        ctx->icmpPackets++;

        NAT_DEBUG("NAT: Responded to gateway ping");
        return;
    }

    // External ping - forward via ICMP NAT
    if (!ctx->icmpPcb) {
        NAT_DEBUG("NAT: No ICMP socket, dropping ping");
        ctx->droppedPackets++;
        return;
    }

    uint16_t icmpId = ntohs(icmp->id);
    uint16_t icmpSeq = ntohs(icmp->sequence);

    // Find or create ICMP NAT entry
    NatIcmpEntry* entry = nullptr;
    int freeSlot = -1;

    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        if (ctx->icmpTable[i].active &&
            ctx->icmpTable[i].dstIP == dstIP &&
            ctx->icmpTable[i].id == icmpId) {
            entry = &ctx->icmpTable[i];
            break;
        }
        if (!ctx->icmpTable[i].active && freeSlot < 0) {
            freeSlot = i;
        }
    }

    if (!entry && freeSlot >= 0) {
        entry = &ctx->icmpTable[freeSlot];
        entry->active = true;
        entry->srcIP = srcIP;
        entry->dstIP = dstIP;
        entry->id = icmpId;
        NAT_DEBUG_F("NAT: Created ICMP entry %d for ping to %d.%d.%d.%d",
                    freeSlot, dstIP[0], dstIP[1], dstIP[2], dstIP[3]);
    }

    if (!entry) {
        NAT_DEBUG("NAT: ICMP table full");
        ctx->droppedPackets++;
        return;
    }

    entry->sequence = icmpSeq;
    entry->lastActivity = millis();

    // Build ICMP packet to send via raw socket
    uint16_t icmpLen = length - ipHeaderLen;
    struct pbuf* p = pbuf_alloc(PBUF_IP, icmpLen, PBUF_RAM);
    if (!p) {
        NAT_DEBUG("NAT: Failed to allocate pbuf for ICMP");
        ctx->droppedPackets++;
        return;
    }

    // Copy ICMP data (header + payload)
    memcpy(p->payload, icmp, icmpLen);

    // Send via raw socket
    ip_addr_t dest;
    IP_ADDR4(&dest, dstIP[0], dstIP[1], dstIP[2], dstIP[3]);

    err_t err = raw_sendto((struct raw_pcb*)ctx->icmpPcb, p, &dest);
    pbuf_free(p);

    if (err == ERR_OK) {
        NAT_DEBUG_F("NAT: Forwarded ping to %d.%d.%d.%d id=%d seq=%d",
                    dstIP[0], dstIP[1], dstIP[2], dstIP[3], icmpId, icmpSeq);
        ctx->packetsToInternet++;
        ctx->icmpPackets++;
    } else {
        NAT_DEBUG_F("NAT: Failed to send ICMP, err=%d", err);
        ctx->droppedPackets++;
    }
}

// ============================================================================
// Send ICMP Packet to Client (Vintage Computer)
// ============================================================================

static void natSendIcmpToClient(NatContext* ctx, NatIcmpEntry* entry,
                                 uint8_t type, uint8_t code,
                                 uint8_t* data, uint16_t length) {
    uint8_t packet[60 + 1472];  // Max IP + ICMP header + data
    uint16_t ipHeaderLen = 20;
    uint16_t icmpHeaderLen = 8;
    uint16_t totalLen = ipHeaderLen + icmpHeaderLen + length;

    if (totalLen > sizeof(packet)) {
        return;
    }

    // Build IP header
    IpHeader* ip = (IpHeader*)packet;
    ip->versionIhl = 0x45;
    ip->tos = 0;
    ip->totalLength = htons(totalLen);
    ip->id = htons(random(1, 65535));
    ip->flagsFragment = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    // Construct IP addresses in network byte order from IPAddress bytes
    ip->srcIP = entry->dstIP[0] | ((uint32_t)entry->dstIP[1] << 8) |
                ((uint32_t)entry->dstIP[2] << 16) | ((uint32_t)entry->dstIP[3] << 24);
    ip->dstIP = entry->srcIP[0] | ((uint32_t)entry->srcIP[1] << 8) |
                ((uint32_t)entry->srcIP[2] << 16) | ((uint32_t)entry->srcIP[3] << 24);

    // Build ICMP header
    IcmpHeader* icmp = (IcmpHeader*)(packet + ipHeaderLen);
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->id = htons(entry->id);
    icmp->sequence = htons(entry->sequence);

    // Copy data
    if (data && length > 0) {
        memcpy(packet + ipHeaderLen + icmpHeaderLen, data, length);
    }

    // Calculate ICMP checksum
    uint16_t icmpTotalLen = icmpHeaderLen + length;
    icmp->checksum = htons(ipChecksum((uint8_t*)icmp, icmpTotalLen));

    // Calculate IP checksum
    ipRecalcChecksum(ip);

    // Send via SLIP
    natSendToClient(ctx, packet, totalLen);
    ctx->packetsFromInternet++;
}

// ============================================================================
// Find TCP Entry
// ============================================================================

static NatTcpEntry* natFindTcpEntry(NatContext* ctx, IPAddress srcIP,
                                     uint16_t srcPort, IPAddress dstIP,
                                     uint16_t dstPort) {
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        if (ctx->tcpTable[i].active &&
            ctx->tcpTable[i].srcIP == srcIP &&
            ctx->tcpTable[i].srcPort == srcPort &&
            ctx->tcpTable[i].dstIP == dstIP &&
            ctx->tcpTable[i].dstPort == dstPort) {
            return &ctx->tcpTable[i];
        }
    }
    return nullptr;
}

// ============================================================================
// Create TCP Entry
// ============================================================================

// Dumps every occupied slot's state/endpoints/age when the table is full -
// on its own, "TCP table full" says nothing about WHY (all 16 genuinely
// busy vs. several stuck in CLOSING/FIN_WAIT and not draining fast enough).
// Called from the SYN handler's table-full path in natProcessTcp.
static void natDumpTcpTable(NatContext* ctx) {
    unsigned long now = millis();
    NAT_DEBUG("NAT: --- TCP table dump ---");
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        NatTcpEntry* e = &ctx->tcpTable[i];
        if (!e->active) continue;
        NAT_DEBUG_F("NAT:  [%d] state=%d %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d "
                    "age=%lums lastActivity=%lums closingFor=%lums",
                    i, e->state,
                    e->srcIP[0], e->srcIP[1], e->srcIP[2], e->srcIP[3], e->srcPort,
                    e->dstIP[0], e->dstIP[1], e->dstIP[2], e->dstIP[3], e->dstPort,
                    now - e->created, now - e->lastActivity,
                    (e->state == NAT_TCP_CLOSING || e->state == NAT_TCP_FIN_WAIT)
                        ? (now - e->closingStartTime) : 0);
    }
    NAT_DEBUG("NAT: --- end dump ---");
}

static NatTcpEntry* natCreateTcpEntry(NatContext* ctx, IPAddress srcIP,
                                       uint16_t srcPort, IPAddress dstIP,
                                       uint16_t dstPort) {
    // Find free slot
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        if (!ctx->tcpTable[i].active) {
            NatTcpEntry* entry = &ctx->tcpTable[i];
            entry->active = true;
            entry->state = NAT_TCP_CLOSED;
            entry->srcIP = srcIP;
            entry->srcPort = srcPort;
            entry->dstIP = dstIP;
            entry->dstPort = dstPort;
            entry->natPort = ctx->nextPort++;
            if (ctx->nextPort > NAT_PORT_END) {
                ctx->nextPort = NAT_PORT_START;
            }
            entry->client = nullptr;
            entry->clientHandshakeAcked = false;
            entry->pendingSockFd = -1;
            entry->clientWindow = NAT_TCP_MAX_WINDOW;  // optimistic until a real value is seen
            entry->clientMss = NAT_TCP_DEFAULT_MSS;    // RFC 879 default until the SYN's options say otherwise
            entry->clientSeq = 0;
            entry->clientAck = 0;
            entry->serverSeq = random(1, 0x7FFFFFFF);  // Random initial sequence
            entry->serverAck = 0;
            entry->rxBuffer = nullptr;
            entry->rxBufferLen = 0;
            entry->rxBufferSize = 0;
            entry->lastActivity = millis();
            entry->lastKeepalive = millis();
            entry->lastServerData = millis();
            entry->lastProbe = millis();
            entry->lastAckTime = millis();
            entry->closingStartTime = 0;
            entry->created = millis();
            return entry;
        }
    }
    return nullptr;
}

// ============================================================================
// Find or Create UDP Entry
// ============================================================================

static NatUdpEntry* natFindOrCreateUdp(NatContext* ctx, IPAddress srcIP,
                                        uint16_t srcPort, IPAddress dstIP,
                                        uint16_t dstPort) {
    // Find existing
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        if (ctx->udpTable[i].active &&
            ctx->udpTable[i].srcIP == srcIP &&
            ctx->udpTable[i].srcPort == srcPort &&
            ctx->udpTable[i].dstIP == dstIP &&
            ctx->udpTable[i].dstPort == dstPort) {
            return &ctx->udpTable[i];
        }
    }

    // Create new
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        if (!ctx->udpTable[i].active) {
            ctx->udpTable[i].active = true;
            ctx->udpTable[i].srcIP = srcIP;
            ctx->udpTable[i].srcPort = srcPort;
            ctx->udpTable[i].dstIP = dstIP;
            ctx->udpTable[i].dstPort = dstPort;
            ctx->udpTable[i].natPort = ctx->nextPort++;
            if (ctx->nextPort > NAT_PORT_END) {
                ctx->nextPort = NAT_PORT_START;
            }
            ctx->udpTable[i].lastActivity = millis();
            ctx->udpSessions++;
            return &ctx->udpTable[i];
        }
    }
    return nullptr;
}

// ============================================================================
// Poll Connections for Incoming Data
// ============================================================================

// ============================================================================
// Poll a Non-Blocking Connect in Progress
// ============================================================================
// Called from natPollConnections for every entry still in NAT_TCP_SYN_SENT
// with a pending raw socket. Never blocks: a zero-timeout select() just asks
// "is this fd writable (or errored) right now", which is the standard way to
// check non-blocking connect() completion. On success the fd is promoted to
// a real WiFiClient (matching what the old blocking connect() would have
// produced) and a SYN-ACK goes out; on failure or timeout we fail exactly
// like the old blocking path did (RST + deactivate), just without ever
// freezing the rest of the gateway while getting there.

static void natPollPendingConnect(NatContext* ctx, NatTcpEntry* entry, int idx) {
    int sockfd = entry->pendingSockFd;

    fd_set writeSet, errSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&errSet);
    FD_SET(sockfd, &writeSet);
    FD_SET(sockfd, &errSet);
    struct timeval tv = {0, 0};  // poll, never block

    int res = select(sockfd + 1, nullptr, &writeSet, &errSet, &tv);

    bool ready = res > 0 && (FD_ISSET(sockfd, &writeSet) || FD_ISSET(sockfd, &errSet));
    bool timedOut = (millis() - entry->connectStartTime) > NAT_TCP_CONNECT_TIMEOUT_MS;

    if (!ready && !timedOut) {
        return;  // still connecting - check again next loop iteration
    }

    int sockerr = 0;
    if (ready) {
        socklen_t len = sizeof(sockerr);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sockerr, &len) < 0) {
            sockerr = errno;
        }
    } else {
        sockerr = ETIMEDOUT;  // timedOut with nothing ready yet
    }

    if (sockerr != 0) {
        NAT_DEBUG_F("NAT: TCP[%d] async connect failed, err=%d", idx, sockerr);
        close(sockfd);
        entry->pendingSockFd = -1;
        natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_RST);
        entry->active = false;
        return;
    }

    // Connected. Promote the raw fd to a WiFiClient so the rest of this
    // file's existing read()/write()/available()/connected() calls work
    // unchanged. Clear O_NONBLOCK first and set read/write timeouts to match
    // what WiFiClient::connect() itself configures on success, since we
    // bypassed that path entirely.
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) & ~O_NONBLOCK);

    entry->client = new WiFiClient(sockfd);
    entry->pendingSockFd = -1;

    struct timeval sockTv;
    sockTv.tv_sec = NAT_TCP_CONNECT_TIMEOUT_MS / 1000;
    sockTv.tv_usec = (NAT_TCP_CONNECT_TIMEOUT_MS % 1000) * 1000;
    entry->client->setSocketOption(SOL_SOCKET, SO_SNDTIMEO, &sockTv, sizeof(sockTv));
    entry->client->setSocketOption(SOL_SOCKET, SO_RCVTIMEO, &sockTv, sizeof(sockTv));

    entry->state = NAT_TCP_ESTABLISHED;
    ctx->packetsToInternet++;
    ctx->tcpConnections++;
    NAT_DEBUG_F("NAT: TCP[%d] async connect succeeded", idx);

    // Initialize serverAck for flow control
    entry->serverAck = entry->serverSeq;
    entry->lastProbe = millis();
    entry->lastAckTime = millis();

    // Send SYN-ACK back to client
    natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_SYN | TCP_FLAG_ACK);
}

int natPollConnections(NatContext* ctx) {
    int packetsSent = 0;

    // Poll TCP connections
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        NatTcpEntry* entry = &ctx->tcpTable[i];
        if (!entry->active) continue;

        // Finish a non-blocking connect() still in progress (see
        // natProcessTcp's SYN handling, which starts it and hands off here).
        if (entry->state == NAT_TCP_SYN_SENT && entry->pendingSockFd >= 0) {
            natPollPendingConnect(ctx, entry, i);
            continue;  // re-evaluated (possibly as ESTABLISHED) next iteration
        }

        if (!entry->client) continue;

        // Data relay: read greedily from the remote socket into a
        // per-connection heap buffer (entry->rxBuffer), then drain that
        // buffer to the client paced STRICTLY by the client's own advertised
        // TCP window (entry->clientWindow, updated in natProcessTcp) - never
        // more, including a genuine 0 (client's buffer is full - wait, don't
        // force it). This NAT has no retransmit buffer of its own, so an
        // earlier version of this code that clamped the effective window up
        // to a fixed minimum caused silent, unrecoverable data loss whenever
        // the client's real window was smaller than that floor: serverSeq
        // kept advancing as if the data had arrived, the client's real ACKs
        // could never catch up, and the connection eventually hit the 60s
        // stall timeout below and got torn down mid-transfer (visible as
        // "waiting in flight" then "expired"/"no entry" once the client kept
        // referencing a connection the gateway had already dropped). Reading
        // into our own heap buffer (the ESP32-S3 has plenty to spare) also
        // decouples draining the fast WiFi/socket side from the slow,
        // small-window serial side, instead of only ever reading exactly as
        // much as the client's window happened to allow on a given poll.
        if (entry->state == NAT_TCP_ESTABLISHED) {
            // Lazily allocate the per-connection buffer on first use.
            if (!entry->rxBuffer) {
                entry->rxBuffer = (uint8_t*)malloc(NAT_TCP_RXBUF_SIZE);
                if (entry->rxBuffer) {
                    entry->rxBufferSize = NAT_TCP_RXBUF_SIZE;
                    entry->rxBufferLen = 0;
                } else {
                    NAT_DEBUG_F("NAT: TCP[%d] rxBuffer alloc failed (low heap?)", i);
                }
            }

            // Top up the buffer from the socket - independent of the
            // client's window, bounded only by our own buffer space.
            if (entry->rxBuffer) {
                int avail = entry->client->available();
                while (avail > 0 && entry->rxBufferLen < entry->rxBufferSize) {
                    uint16_t space = entry->rxBufferSize - entry->rxBufferLen;
                    uint16_t toRead = ((uint32_t)avail < space) ? (uint16_t)avail : space;
                    int got = entry->client->read(entry->rxBuffer + entry->rxBufferLen, toRead);
                    if (got <= 0) break;
                    entry->rxBufferLen += got;
                    avail = entry->client->available();
                }
            }

            // Drain buffered data to the client, paced by its real window.
            // Bytes are NOT discarded from rxBuffer merely because we sent
            // them - only once the client's ACK actually confirms receipt
            // (see the trim in natProcessTcp's ACK handling). This lets a
            // segment lost/corrupted on the flow-control-free serial link be
            // retransmitted instead of gone forever; the old discard-on-send
            // design meant a single dropped byte anywhere in a large
            // transfer stalled the connection permanently until the 60s
            // stall timeout gave up (observed real-world: large FTP
            // directory listings bailing partway through with zero further
            // data ever arriving, worse the larger/longer the transfer).
            if (entry->rxBuffer && entry->rxBufferLen > 0) {
                uint32_t inFlight = entry->serverSeq - entry->serverAck;
                uint32_t effectiveWindow = entry->clientWindow;
                if (effectiveWindow > NAT_TCP_MAX_WINDOW) effectiveWindow = NAT_TCP_MAX_WINDOW;

                // Retransmit-on-timeout: if bytes are outstanding and no real
                // ACK progress has happened in NAT_TCP_RETRANSMIT_MS, assume
                // loss and rewind serverSeq back to the last confirmed ACK
                // point. The send logic below then naturally re-emits the
                // same bytes from the front of the still-buffered unacked
                // region (nothing was discarded on send) - a real
                // retransmission, not a guess. Safe even if the real cause
                // turns out to be a still-full window rather than loss: a
                // receiver with no room simply re-acks its same cumulative
                // point again, same as real TCP.
                if (inFlight > 0 && (millis() - entry->lastAckTime) > NAT_TCP_RETRANSMIT_MS) {
                    NAT_DEBUG_F("NAT: TCP[%d] retransmit: no ACK progress for %lums (inFlight=%u), rewinding",
                                i, millis() - entry->lastAckTime, inFlight);
                    entry->serverSeq = entry->serverAck;
                    inFlight = 0;
                    entry->lastAckTime = millis();
                }

                if (inFlight < effectiveWindow) {
                    // Cap each segment at 1000 bytes for throughput/reliability
                    // balance, but never exceed what the client's SYN actually
                    // told us it would accept (entry->clientMss) - sending more
                    // than that is a protocol violation the client is entitled
                    // to silently drop.
                    uint32_t room = effectiveWindow - inFlight;
                    uint32_t notYetSent = entry->rxBufferLen - inFlight;
                    uint32_t canSend = 1000;
                    if (canSend > entry->clientMss) canSend = entry->clientMss;
                    if (canSend > room) canSend = room;
                    if (canSend > notYetSent) canSend = notYetSent;

                    if (canSend > 0) {
                        NAT_DEBUG_F("NAT: TCP[%d] sending %u bytes, seq=%u, buffered=%u",
                                    i, canSend, entry->serverSeq, entry->rxBufferLen);
                        natSendTcpToClient(ctx, entry, entry->rxBuffer + inFlight, (uint16_t)canSend,
                                           TCP_FLAG_PSH | TCP_FLAG_ACK);
                        // Deliberately NOT trimming rxBuffer here - these
                        // bytes stay available for retransmission until the
                        // ACK-handling trim in natProcessTcp confirms them.
                        entry->lastActivity = millis();
                        entry->lastServerData = millis();
                        entry->lastAckTime = millis();
                        packetsSent++;
                    }
                } else {
                    // Client's window is fully used (possibly a genuine 0) -
                    // wait for it to ACK/reopen. Not a problem by itself;
                    // only an actual stall if it stays this way too long.
                    unsigned long now = millis();
                    unsigned long waitTime = now - entry->lastActivity;

                    static unsigned long lastWaitDebug = 0;
                    if (now - lastWaitDebug > 5000) {
                        NAT_DEBUG_F("NAT: TCP[%d] WAITING: inFlight=%u, window=%u, buffered=%u, wait=%lums",
                                    i, inFlight, entry->clientWindow, entry->rxBufferLen, waitTime);
                        lastWaitDebug = now;
                    }

                    // Zero-window probe (RFC 1122 4.2.2.17): a real TCP
                    // receiver is only obligated to ACK an incoming byte
                    // (accept it or reject it), not to proactively send an
                    // unsolicited window update once its buffer drains. If
                    // the client's stack doesn't bother with that optional
                    // behavior, and we've correctly stopped sending because
                    // we think its window is exhausted, nothing will ever
                    // prompt a fresh ACK on its own - a silent deadlock that
                    // would otherwise only ever end via the stall timeout
                    // below giving up on the whole connection. Push 1 byte of
                    // the next not-yet-sent data past the client's stated
                    // window anyway; whatever it does with that byte, the ACK
                    // it's required to send back tells us the real current
                    // state. Only meaningful if there IS a next unsent byte -
                    // if everything buffered is already sent and outstanding,
                    // the retransmit-timeout logic above already covers
                    // re-poking the client instead.
                    uint32_t notYetSent = entry->rxBufferLen - inFlight;
                    if (notYetSent > 0 && now - entry->lastProbe > NAT_TCP_PROBE_INTERVAL_MS) {
                        NAT_DEBUG_F("NAT: TCP[%d] zero-window probe (window=%u, buffered=%u)",
                                    i, entry->clientWindow, entry->rxBufferLen);
                        natSendTcpToClient(ctx, entry, entry->rxBuffer + inFlight, 1,
                                           TCP_FLAG_PSH | TCP_FLAG_ACK);
                        // Not trimmed - same retransmit-until-acked rule as
                        // the main send path above.
                        entry->lastProbe = now;
                    }

                    // Stall detection - close if no window movement for too long
                    if (waitTime > NAT_TCP_STALL_TIMEOUT_MS) {
                        NAT_DEBUG_F("NAT: TCP[%d] stalled (no window for %lums), closing", i, waitTime);
                        if (entry->client) {
                            entry->client->stop();
                            delete entry->client;
                            entry->client = nullptr;
                        }
                        if (entry->rxBuffer) {
                            free(entry->rxBuffer);
                            entry->rxBuffer = nullptr;
                            entry->rxBufferLen = 0;
                            entry->rxBufferSize = 0;
                        }
                        entry->active = false;
                        continue;
                    }
                }
            }
        }

        // Check if connection closed by remote - but only once any data we
        // already pulled off the (now-closed) socket has been fully drained
        // to the client, so a server closing right after the last write
        // doesn't truncate the tail of a listing/download.
        if (entry->client && !entry->client->connected() && entry->state == NAT_TCP_ESTABLISHED
            && (!entry->rxBuffer || entry->rxBufferLen == 0)) {
            // Send FIN to client
            NAT_DEBUG_F("NAT: TCP[%d] remote closed -> sending our own FIN|ACK, serverSeq=%u",
                        i, entry->serverSeq);
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
            entry->state = NAT_TCP_CLOSING;
            entry->closingStartTime = millis();
        }

        // TCP keepalive - send periodic ACK to keep connection alive
        if (entry->state == NAT_TCP_ESTABLISHED) {
            unsigned long now = millis();
            if (now - entry->lastKeepalive > NAT_TCP_KEEPALIVE_MS) {
                NAT_DEBUG_F("NAT: TCP[%d] sending keepalive", i);
                natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
                entry->lastKeepalive = now;
                entry->lastActivity = now;
            }

            // Check for server silence - warn if no data from server for 5+ minutes
            unsigned long serverSilence = now - entry->lastServerData;
            if (serverSilence > 300000) {
                static unsigned long lastSilenceWarn = 0;
                if (now - lastSilenceWarn > 60000) {
                    NAT_DEBUG_F("NAT: TCP[%d] server silent for %lu seconds", i, serverSilence / 1000);
                    lastSilenceWarn = now;
                }
            }
        }
    }

    // Poll each active UDP entry's OWN socket for a reply. Each entry owns
    // its own socket (see NatContext::udpSockets) so whatever arrives on
    // entry i's socket unambiguously belongs to entry i - fixes a confirmed
    // bug where a shared socket could only match a response to an entry by
    // remote address, misrouting a reply to a stale entry sharing the same
    // remote dst:port (e.g. an NTP retry opening a new entry before the
    // prior sync attempt's entry had expired).
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        if (!ctx->udpTable[i].active || !ctx->udpSocketBound[i]) continue;

        int packetSize = ctx->udpSockets[i].parsePacket();
        if (packetSize <= 0) continue;

        uint8_t buf[1472];
        int len = ctx->udpSockets[i].read(buf, sizeof(buf));
        if (len <= 0) continue;

        NatUdpEntry* entry = &ctx->udpTable[i];
        IPAddress remoteIP = ctx->udpSockets[i].remoteIP();
        uint16_t remotePort = ctx->udpSockets[i].remotePort();

        // Defensive check: a plain (unconnected) UDP socket will accept a
        // datagram from ANY sender to this local port, not just the server
        // this entry actually queried. Reject anything that doesn't match
        // who we asked, rather than treating it as this entry's answer.
        if (remoteIP != entry->dstIP || remotePort != entry->dstPort) {
            NAT_DEBUG_F("NAT: UDP entry %d got a reply from %d.%d.%d.%d:%d, expected %d.%d.%d.%d:%d - ignoring",
                        i, remoteIP[0], remoteIP[1], remoteIP[2], remoteIP[3], remotePort,
                        entry->dstIP[0], entry->dstIP[1], entry->dstIP[2], entry->dstIP[3], entry->dstPort);
            continue;
        }

        NAT_DEBUG_F("NAT: UDP response from %d.%d.%d.%d:%d len=%d (entry %d)",
                    remoteIP[0], remoteIP[1], remoteIP[2], remoteIP[3],
                    remotePort, len, i);

        natSendUdpToClient(ctx, entry, buf, len);
        packetsSent++;

        // Release the entry and its socket right after one response, for
        // every protocol, not just DNS. UDP request/response protocols
        // don't need the socket held open after the one answer arrives - a
        // later retry just opens a fresh entry+socket. Keeping a non-DNS
        // entry's socket open for the full idle timeout (as DNS-only release
        // once did) risks exhausting CONFIG_LWIP_MAX_SOCKETS, a hard
        // system-wide socket ceiling shared with TCP.
        entry->active = false;
        ctx->udpSockets[i].stop();
        ctx->udpSocketBound[i] = false;
        NAT_DEBUG_F("NAT: Released UDP entry %d after response (dst port %d)", i, entry->dstPort);
    }

    // Process any pending ICMP replies
    natProcessPendingIcmp(ctx);

    return packetsSent;
}

// ============================================================================
// Clean Up Expired Connections
// ============================================================================

void natCleanupExpired(NatContext* ctx) {
    unsigned long now = millis();

    // Clean TCP connections
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        NatTcpEntry* entry = &ctx->tcpTable[i];
        if (!entry->active) continue;

        bool expired;
        unsigned long elapsed;
        if (entry->state == NAT_TCP_SYN_SENT) {
            elapsed = now - entry->lastActivity;
            expired = elapsed > NAT_TCP_SYN_TIMEOUT_MS;
        } else if (entry->state == NAT_TCP_CLOSING || entry->state == NAT_TCP_FIN_WAIT) {
            // Measured from closingStartTime (set once, on the transition
            // into this state), never lastActivity - lastActivity is
            // refreshed by every incoming packet, including a client
            // endlessly retransmitting its own FIN. Using lastActivity here
            // would let such a connection's expiry clock reset on every
            // retry and never actually expire, eventually exhausting the
            // 16-slot table ("TCP table full" for every new connection
            // attempt).
            elapsed = now - entry->closingStartTime;
            expired = elapsed > NAT_TCP_CLOSING_TIMEOUT_MS;
        } else {
            elapsed = now - entry->lastActivity;
            expired = elapsed > NAT_TCP_TIMEOUT_MS;
        }

        if (expired) {
            NAT_DEBUG_F("NAT: Closing expired TCP %d (state=%d, %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d, elapsed=%lums)",
                        i, entry->state,
                        entry->srcIP[0], entry->srcIP[1], entry->srcIP[2], entry->srcIP[3], entry->srcPort,
                        entry->dstIP[0], entry->dstIP[1], entry->dstIP[2], entry->dstIP[3], entry->dstPort,
                        elapsed);
            if (entry->client) {
                entry->client->stop();
                delete entry->client;
                entry->client = nullptr;
            }
            if (entry->pendingSockFd >= 0) {
                // Outer safety net for a non-blocking connect that
                // natPollConnections' own (much faster) timeout somehow
                // missed - closes the raw fd before it leaks.
                close(entry->pendingSockFd);
                entry->pendingSockFd = -1;
            }
            if (entry->rxBuffer) {
                free(entry->rxBuffer);
                entry->rxBuffer = nullptr;
            }
            entry->active = false;
        }
    }

    // Clean UDP sessions
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        NatUdpEntry* entry = &ctx->udpTable[i];
        if (entry->active && now - entry->lastActivity > NAT_UDP_TIMEOUT_MS) {
            NAT_DEBUG_F("NAT: Closing expired UDP %d", i);
            entry->active = false;
            // Must release this entry's own socket here too, not just mark
            // the entry inactive - otherwise natFindOrCreateUdp reusing this
            // slot skips re-binding (udpSocketBound[i] still true) and the
            // socket stays bound to the OLD natPort while entry->natPort
            // gets a new value, silently desyncing the two.
            if (ctx->udpSocketBound[i]) {
                ctx->udpSockets[i].stop();
                ctx->udpSocketBound[i] = false;
            }
        }
    }

    // Clean ICMP sessions
    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        NatIcmpEntry* entry = &ctx->icmpTable[i];
        if (entry->active && now - entry->lastActivity > NAT_ICMP_TIMEOUT_MS) {
            NAT_DEBUG_F("NAT: Closing expired ICMP %d", i);
            entry->active = false;
        }
    }
}

// ============================================================================
// Send TCP Packet to Client (Vintage Computer)
// ============================================================================

static void natSendTcpToClient(NatContext* ctx, NatTcpEntry* entry,
                                uint8_t* data, uint16_t length, uint8_t flags) {
    // Build IP + TCP packet
    uint16_t ipHeaderLen = 20;
    uint16_t tcpHeaderLen = 20;
    uint16_t totalLen = ipHeaderLen + tcpHeaderLen + length;
    uint8_t packet[60 + 1460];  // Max IP header + TCP header + data

    if (totalLen > sizeof(packet)) {
        return;
    }

    memset(packet, 0, totalLen);

    // Build IP header
    IpHeader* ip = (IpHeader*)packet;
    ip->versionIhl = 0x45;  // IPv4, 5 words header
    ip->tos = 0;
    ip->totalLength = htons(totalLen);
    ip->id = htons(random(1, 65535));
    ip->flagsFragment = htons(0x4000);  // Don't fragment
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    // Construct IP addresses in network byte order from IPAddress bytes
    ip->srcIP = entry->dstIP[0] | ((uint32_t)entry->dstIP[1] << 8) |
                ((uint32_t)entry->dstIP[2] << 16) | ((uint32_t)entry->dstIP[3] << 24);
    ip->dstIP = entry->srcIP[0] | ((uint32_t)entry->srcIP[1] << 8) |
                ((uint32_t)entry->srcIP[2] << 16) | ((uint32_t)entry->srcIP[3] << 24);

    // Build TCP header
    TcpHeader* tcp = (TcpHeader*)(packet + ipHeaderLen);
    tcp->srcPort = htons(entry->dstPort);
    tcp->dstPort = htons(entry->srcPort);
    tcp->seqNum = htonl(entry->serverSeq);
    tcp->ackNum = htonl(entry->clientSeq);  // clientSeq is already "next expected byte"
    tcp->dataOffset = 0x50;  // 5 words header
    tcp->flags = flags;
    tcp->window = htons(8192);
    tcp->urgentPtr = 0;

    // Copy data
    if (data && length > 0) {
        memcpy(packet + ipHeaderLen + tcpHeaderLen, data, length);
    }

    // Update sequence for next packet
    if (flags & TCP_FLAG_SYN) {
        entry->serverSeq++;
    }
    if (flags & TCP_FLAG_FIN) {
        entry->serverSeq++;
    }
    if (length > 0) {
        entry->serverSeq += length;
    }

    // Calculate checksums
    ipRecalcChecksum(ip);
    tcp->checksum = 0;
    tcp->checksum = htons(tcpUdpChecksum(ip, (uint8_t*)tcp, tcpHeaderLen + length));

    // Send via SLIP
    natSendToClient(ctx, packet, totalLen);
    ctx->packetsFromInternet++;
}

// ============================================================================
// Send UDP Packet to Client (Vintage Computer)
// ============================================================================

static void natSendUdpToClient(NatContext* ctx, NatUdpEntry* entry,
                                uint8_t* data, uint16_t length) {
    // Build IP + UDP packet
    uint16_t totalLen = 20 + 8 + length;  // IP header + UDP header + data
    uint8_t packet[totalLen];
    memset(packet, 0, totalLen);

    // Build IP header
    IpHeader* ip = (IpHeader*)packet;
    ip->versionIhl = 0x45;
    ip->tos = 0;
    ip->totalLength = htons(totalLen);
    ip->id = htons(random(1, 65535));
    ip->flagsFragment = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    // Construct IP addresses in network byte order from IPAddress bytes
    ip->srcIP = entry->dstIP[0] | ((uint32_t)entry->dstIP[1] << 8) |
                ((uint32_t)entry->dstIP[2] << 16) | ((uint32_t)entry->dstIP[3] << 24);
    ip->dstIP = entry->srcIP[0] | ((uint32_t)entry->srcIP[1] << 8) |
                ((uint32_t)entry->srcIP[2] << 16) | ((uint32_t)entry->srcIP[3] << 24);
    ipRecalcChecksum(ip);

    // Build UDP header
    UdpHeader* udp = (UdpHeader*)(packet + 20);
    udp->srcPort = htons(entry->dstPort);
    udp->dstPort = htons(entry->srcPort);
    udp->length = htons(8 + length);

    // Copy data
    if (data && length > 0) {
        memcpy(packet + 28, data, length);
    }

    // Calculate UDP checksum
    udp->checksum = 0;
    udp->checksum = htons(tcpUdpChecksum(ip, (uint8_t*)udp, 8 + length));

    // Send via SLIP
    natSendToClient(ctx, packet, totalLen);
}

// ============================================================================
// Send IP Packet to Client via SLIP
// ============================================================================

void natSendToClient(NatContext* ctx, uint8_t* packet, uint16_t length) {
    extern SlipContext slipCtx;
    slipSendFrame(&slipCtx, packet, length);
}

// ============================================================================
// Port Forwarding Management
// ============================================================================

int natAddPortForward(NatContext* ctx, uint8_t proto, uint16_t extPort,
                      IPAddress intIP, uint16_t intPort, bool startServer) {
    // Find free slot
    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        if (!ctx->portForwards[i].active) {
            ctx->portForwards[i].active = true;
            ctx->portForwards[i].protocol = proto;
            ctx->portForwards[i].externalPort = extPort;
            ctx->portForwards[i].internalIP = intIP;
            ctx->portForwards[i].internalPort = intPort;

            // Start listener for TCP port forwards only if requested
            if (startServer && proto == IP_PROTO_TCP) {
                ctx->tcpForwardServers[i] = new WiFiServer(extPort);
                ctx->tcpForwardServers[i]->begin();
            }
            return i;
        }
    }
    return -1;  // Table full
}

void natStartPortForwardServers(NatContext* ctx) {
    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        if (ctx->portForwards[i].active &&
            ctx->portForwards[i].protocol == IP_PROTO_TCP &&
            !ctx->tcpForwardServers[i]) {
            ctx->tcpForwardServers[i] = new WiFiServer(ctx->portForwards[i].externalPort);
            ctx->tcpForwardServers[i]->begin();
        }
    }
}

void natStopPortForwardServers(NatContext* ctx) {
    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        if (ctx->tcpForwardServers[i]) {
            ctx->tcpForwardServers[i]->stop();
            delete ctx->tcpForwardServers[i];
            ctx->tcpForwardServers[i] = nullptr;
        }
    }
}

void natRemovePortForward(NatContext* ctx, int index) {
    if (index < 0 || index >= NAT_MAX_PORT_FORWARDS) return;
    if (!ctx->portForwards[index].active) return;

    if (ctx->tcpForwardServers[index]) {
        ctx->tcpForwardServers[index]->stop();
        delete ctx->tcpForwardServers[index];
        ctx->tcpForwardServers[index] = nullptr;
    }
    ctx->portForwards[index].active = false;
}

// ============================================================================
// Check Port Forwards for Incoming Connections
// ============================================================================

void natCheckPortForwards(NatContext* ctx) {
    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        if (!ctx->portForwards[i].active) continue;
        if (ctx->portForwards[i].protocol != IP_PROTO_TCP) continue;
        if (!ctx->tcpForwardServers[i]) continue;

        WiFiClient newClient = ctx->tcpForwardServers[i]->available();
        if (newClient) {
            // Create NAT entry for this inbound connection
            NatTcpEntry* entry = natCreateTcpEntry(ctx,
                ctx->portForwards[i].internalIP,
                ctx->portForwards[i].internalPort,
                newClient.remoteIP(),
                newClient.remotePort());

            if (entry) {
                entry->client = new WiFiClient(newClient);
                entry->state = NAT_TCP_ESTABLISHED;

                NAT_DEBUG_F("NAT: Port forward entry created: src=%d.%d.%d.%d:%d dst=%d.%d.%d.%d:%d",
                            entry->srcIP[0], entry->srcIP[1], entry->srcIP[2], entry->srcIP[3], entry->srcPort,
                            entry->dstIP[0], entry->dstIP[1], entry->dstIP[2], entry->dstIP[3], entry->dstPort);

                // Send SYN to internal host
                natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_SYN);
            } else {
                // No room - reject connection
                newClient.stop();
            }
        }
    }
}

// ============================================================================
// Get Connection Counts
// ============================================================================

int natGetActiveTcpCount(NatContext* ctx) {
    int count = 0;
    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        if (ctx->tcpTable[i].active) count++;
    }
    return count;
}

int natGetActiveUdpCount(NatContext* ctx) {
    int count = 0;
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        if (ctx->udpTable[i].active) count++;
    }
    return count;
}

// ============================================================================
// Port Forward Persistence (EEPROM)
// ============================================================================

void savePortForwards(NatContext* ctx) {
    for (int i = 0; i < PORTFWD_COUNT; i++) {
        int addr = PORTFWD_BASE + (i * PORTFWD_SIZE);

        if (ctx->portForwards[i].active) {
            EEPROM.write(addr, 1);  // active flag
            EEPROM.write(addr + 1, ctx->portForwards[i].protocol);
            EEPROM.write(addr + 2, (ctx->portForwards[i].externalPort >> 8) & 0xFF);
            EEPROM.write(addr + 3, ctx->portForwards[i].externalPort & 0xFF);
            EEPROM.write(addr + 4, (ctx->portForwards[i].internalPort >> 8) & 0xFF);
            EEPROM.write(addr + 5, ctx->portForwards[i].internalPort & 0xFF);
            EEPROM.write(addr + 6, ctx->portForwards[i].internalIP[0]);
            EEPROM.write(addr + 7, ctx->portForwards[i].internalIP[1]);
            EEPROM.write(addr + 8, ctx->portForwards[i].internalIP[2]);
            EEPROM.write(addr + 9, ctx->portForwards[i].internalIP[3]);
        } else {
            EEPROM.write(addr, 0);  // inactive
        }
    }
    EEPROM.commit();
}

void loadPortForwards(NatContext* ctx) {
    for (int i = 0; i < PORTFWD_COUNT; i++) {
        int addr = PORTFWD_BASE + (i * PORTFWD_SIZE);

        uint8_t active = EEPROM.read(addr);
        if (active == 1) {
            ctx->portForwards[i].active = true;
            ctx->portForwards[i].protocol = EEPROM.read(addr + 1);
            ctx->portForwards[i].externalPort = (EEPROM.read(addr + 2) << 8) | EEPROM.read(addr + 3);
            ctx->portForwards[i].internalPort = (EEPROM.read(addr + 4) << 8) | EEPROM.read(addr + 5);
            ctx->portForwards[i].internalIP = IPAddress(
                EEPROM.read(addr + 6),
                EEPROM.read(addr + 7),
                EEPROM.read(addr + 8),
                EEPROM.read(addr + 9)
            );
        } else {
            ctx->portForwards[i].active = false;
        }
    }
}
