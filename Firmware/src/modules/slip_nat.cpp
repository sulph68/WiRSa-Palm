// SLIP NAT engine: translation between the vintage computer (SLIP) and WiFi. A TCP proxy: WiFiClient carries the real TCP connections.

#include "slip_nat_internal.h"

// context pointer for the ICMP callback (lwIP callbacks carry no user data)
NatContext* g_icmpNatCtx = nullptr;

// ---- Initialize NAT Context ----

void natInit(NatContext* ctx) {
    // default SLIP subnet (192.168.7.0/24)
    ctx->slipIP = IPAddress(192, 168, 7, 1);
    ctx->clientIP = IPAddress(192, 168, 7, 2);
    ctx->subnetMask = IPAddress(255, 255, 255, 0);
    ctx->dnsServer = IPAddress(8, 8, 8, 8);  // Google DNS

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
        ctx->tcpTable[i].txq = nullptr;
        ctx->tcpTable[i].txqSize = ctx->tcpTable[i].txqHead = ctx->tcpTable[i].txqLen = 0;
    }

    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        ctx->udpTable[i].active = false;
    }

    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        ctx->icmpTable[i].active = false;
    }

    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        ctx->portForwards[i].active = false;
        ctx->tcpForwardServers[i] = nullptr;
    }

    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        ctx->udpSocketBound[i] = false;
    }
    ctx->nextPort = NAT_PORT_START;

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

    g_icmpNatCtx = ctx;

    ctx->packetsToInternet = 0;
    ctx->packetsFromInternet = 0;
    ctx->droppedPackets = 0;
    ctx->tcpConnections = 0;
    ctx->udpSessions = 0;
    ctx->icmpPackets = 0;
    ctx->statIpBadCksum = ctx->statTcpBadCksum = ctx->statOutOfSeq = 0;
    ctx->statRtoRewinds = ctx->statFastRetx = ctx->statDupAcks = ctx->statPartialWrites = ctx->statQueueFull = ctx->statQueueMax = 0;
}

// ---- Shutdown NAT: close all connections ----

void natShutdown(NatContext* ctx) {
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
                natFreeTxq(&ctx->tcpTable[i]);
            }
            ctx->tcpTable[i].active = false;
        }
    }

    // clear UDP sessions and stop each entry's own socket
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        ctx->udpTable[i].active = false;
        if (ctx->udpSocketBound[i]) {
            ctx->udpSockets[i].stop();
            ctx->udpSocketBound[i] = false;
        }
    }

    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        ctx->icmpTable[i].active = false;
    }

    for (int i = 0; i < NAT_MAX_PORT_FORWARDS; i++) {
        if (ctx->tcpForwardServers[i]) {
            ctx->tcpForwardServers[i]->stop();
            delete ctx->tcpForwardServers[i];
            ctx->tcpForwardServers[i] = nullptr;
        }
    }

    if (ctx->icmpPcb) {
        raw_remove((struct raw_pcb*)ctx->icmpPcb);
        ctx->icmpPcb = nullptr;
    }
    g_icmpNatCtx = nullptr;
}

// ---- IP Checksum Calculation ----

uint16_t ipChecksum(const uint8_t* data, uint16_t length) {
    uint32_t sum = 0;

    for (uint16_t i = 0; i < length - 1; i += 2) {
        sum += (data[i] << 8) | data[i + 1];
    }

    if (length & 1) {
        sum += data[length - 1] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

// ---- Recalculate IP Header Checksum ----

void ipRecalcChecksum(IpHeader* ip) {
    uint8_t headerLen = (ip->versionIhl & 0x0F) * 4;
    ip->checksum = 0;
    ip->checksum = htons(ipChecksum((uint8_t*)ip, headerLen));
}

// ---- TCP/UDP Pseudo-header Checksum ----

uint16_t tcpUdpChecksum(IpHeader* ip, const uint8_t* payload, uint16_t payloadLen) {
    uint32_t sum = 0;

    sum += (ntohl(ip->srcIP) >> 16) & 0xFFFF;
    sum += ntohl(ip->srcIP) & 0xFFFF;

    sum += (ntohl(ip->dstIP) >> 16) & 0xFFFF;
    sum += ntohl(ip->dstIP) & 0xFFFF;

    sum += ip->protocol;
    sum += payloadLen;

    for (uint16_t i = 0; i < payloadLen - 1; i += 2) {
        sum += (payload[i] << 8) | payload[i + 1];
    }
    if (payloadLen & 1) {
        sum += payload[payloadLen - 1] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

// ---- Process Incoming IP Packet from Vintage Computer ----

void natProcessPacket(NatContext* ctx, uint8_t* packet, uint16_t length) {
    if (length < 20) {
        NAT_DEBUG_F("NAT: Packet too short (%d bytes)", length);
        ctx->droppedPackets++;
        return;
    }

    IpHeader* ip = (IpHeader*)packet;

    uint8_t version = (ip->versionIhl >> 4) & 0x0F;
    if (version != 4) {
        NAT_DEBUG_F("NAT: Invalid IP version %d", version);
        ctx->droppedPackets++;
        return;
    }

    uint8_t headerLen = (ip->versionIhl & 0x0F) * 4;
    if (headerLen < 20 || headerLen > length) {
        NAT_DEBUG_F("NAT: Invalid header length %d", headerLen);
        ctx->droppedPackets++;
        return;
    }

    uint16_t originalChecksum = ip->checksum;
    ip->checksum = 0;
    uint16_t calculatedChecksum = htons(ipChecksum(packet, headerLen));
    ip->checksum = originalChecksum;

    if (originalChecksum != calculatedChecksum) {
        NAT_DEBUG_F("NAT: IP checksum mismatch: got 0x%04X, calc 0x%04X",
                    ntohs(originalChecksum), ntohs(calculatedChecksum));
        ctx->droppedPackets++;
        ctx->statIpBadCksum++;
        return;
    }

    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    NAT_DEBUG_F("NAT: RX proto=%d %d.%d.%d.%d -> %d.%d.%d.%d len=%d",
                ip->protocol,
                srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3],
                dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3],
                length);

    uint16_t totalLength = ntohs(ip->totalLength);
    if (totalLength > length) {
        ctx->droppedPackets++;
        return;
    }

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
            ctx->droppedPackets++;
            break;
    }
}

// ---- Find TCP Entry ----

NatTcpEntry* natFindTcpEntry(NatContext* ctx, IPAddress srcIP,
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

// ---- Create TCP Entry ----

// Dumps every occupied slot's state, endpoints and age when the table is full: "TCP table full" alone does not say whether all 16 are really busy or several are stuck in CLOSING/FIN_WAIT and not
// draining. Called from natProcessTcp's SYN handler on table-full.
void natDumpTcpTable(NatContext* ctx) {
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

NatTcpEntry* natCreateTcpEntry(NatContext* ctx, IPAddress srcIP,
                                       uint16_t srcPort, IPAddress dstIP,
                                       uint16_t dstPort) {
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
            entry->dupAcks = 0;
            entry->lastAckWindow = 0;
            entry->txq = nullptr;
            entry->txqSize = entry->txqHead = entry->txqLen = 0;
            entry->lastAdvWindow = 0;
            entry->created = millis();
            return entry;
        }
    }
    return nullptr;
}

// ---- Find or Create UDP Entry ----

NatUdpEntry* natFindOrCreateUdp(NatContext* ctx, IPAddress srcIP,
                                        uint16_t srcPort, IPAddress dstIP,
                                        uint16_t dstPort) {
    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        if (ctx->udpTable[i].active &&
            ctx->udpTable[i].srcIP == srcIP &&
            ctx->udpTable[i].srcPort == srcPort &&
            ctx->udpTable[i].dstIP == dstIP &&
            ctx->udpTable[i].dstPort == dstPort) {
            return &ctx->udpTable[i];
        }
    }

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

// ---- Clean Up Expired Connections ----

void natCleanupExpired(NatContext* ctx) {
    unsigned long now = millis();

    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        NatTcpEntry* entry = &ctx->tcpTable[i];
        if (!entry->active) continue;

        bool expired;
        unsigned long elapsed;
        if (entry->state == NAT_TCP_SYN_SENT) {
            elapsed = now - entry->lastActivity;
            expired = elapsed > NAT_TCP_SYN_TIMEOUT_MS;
        } else if (entry->state == NAT_TCP_CLOSING || entry->state == NAT_TCP_FIN_WAIT) {
            // Measured from closingStartTime (set once, on entering this state), never lastActivity: every incoming packet refreshes lastActivity, including a client endlessly retransmitting its FIN, which would
            // reset the expiry on every retry, never expire, and eventually exhaust the 16-slot table ("TCP table full" for every new connection).
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
                // Outer safety net for a non-blocking connect that natPollConnections' much faster timeout missed: close the raw fd before it leaks.
                close(entry->pendingSockFd);
                entry->pendingSockFd = -1;
            }
            if (entry->rxBuffer) {
                free(entry->rxBuffer);
                entry->rxBuffer = nullptr;
            }
            natFreeTxq(entry);
            entry->active = false;
        }
    }

    for (int i = 0; i < NAT_MAX_UDP_SESSIONS; i++) {
        NatUdpEntry* entry = &ctx->udpTable[i];
        if (entry->active && now - entry->lastActivity > NAT_UDP_TIMEOUT_MS) {
            NAT_DEBUG_F("NAT: Closing expired UDP %d", i);
            entry->active = false;
            // Release this entry's socket here too, not only mark the entry inactive: otherwise natFindOrCreateUdp reusing the slot skips re-binding (udpSocketBound[i] still true), the socket stays bound to
            // the OLD natPort and entry->natPort gets a new one, silently desyncing them.
            if (ctx->udpSocketBound[i]) {
                ctx->udpSockets[i].stop();
                ctx->udpSocketBound[i] = false;
            }
        }
    }

    for (int i = 0; i < NAT_MAX_ICMP_SESSIONS; i++) {
        NatIcmpEntry* entry = &ctx->icmpTable[i];
        if (entry->active && now - entry->lastActivity > NAT_ICMP_TIMEOUT_MS) {
            NAT_DEBUG_F("NAT: Closing expired ICMP %d", i);
            entry->active = false;
        }
    }
}

// ---- Send IP Packet to Client via SLIP ----

void natSendToClient(NatContext* ctx, uint8_t* packet, uint16_t length) {
    extern SlipContext slipCtx;
    slipSendFrame(&slipCtx, packet, length);
}

// ---- Check Port Forwards for Incoming Connections ----

// ---- Get Connection Counts ----

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
