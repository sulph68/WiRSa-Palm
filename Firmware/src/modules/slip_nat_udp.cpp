// slip_nat_udp.cpp - UDP: datagrams out and back. Part of the SLIP NAT; include/slip_nat_internal.h says how the files divide the work.
#include "slip_nat_internal.h"

// ---- process a UDP packet ----

void natProcessUdp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen) {
    if (length < ipHeaderLen + 8) {
        ctx->droppedPackets++;
        return;
    }

    UdpHeader* udp = (UdpHeader*)(packet + ipHeaderLen);
    uint16_t srcPort = ntohs(udp->srcPort);
    uint16_t dstPort = ntohs(udp->dstPort);
    uint16_t udpLen = ntohs(udp->length);
    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    IPAddress dstIP(dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3]);
    IPAddress srcIP(srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3]);

    if (udpLen < 8 || ipHeaderLen + udpLen > length) {
        ctx->droppedPackets++;
        return;
    }

    // Verify the UDP checksum, for the reason of the TCP check in natProcessTcp (only the IP header was validated, leaving the UDP header and payload, nearly the whole packet, unverified). UDP checksums
    // are optional (RFC 768, 0 = not computed): unlike TCP, a 0 is accepted as is, not a mismatch.
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

    // Always log the request side (it was silent, unlike TCP, which made it impossible to tell whether a client packet, e.g. a DNS query, ever reached the gateway).
    NAT_DEBUG_F("NAT: UDP %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d len=%d%s",
                srcIP[0], srcIP[1], srcIP[2], srcIP[3], srcPort,
                dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort, payloadLen,
                dstPort == 53 ? " (DNS)" : "");

    NatUdpEntry* entry = natFindOrCreateUdp(ctx, srcIP, srcPort, dstIP, dstPort);
    if (!entry) {
        NAT_DEBUG("NAT: UDP table full - dropping request");
        ctx->droppedPackets++;
        return;
    }
    int udpIdx = (int)(entry - ctx->udpTable);

    entry->lastActivity = millis();

    // Bind this entry's own UDP socket if not already (see NatContext::udpSockets for why each entry has one), on the natPort allocated in natFindOrCreateUdp.
    if (!ctx->udpSocketBound[udpIdx]) {
        if (!ctx->udpSockets[udpIdx].begin(entry->natPort)) {
            NAT_DEBUG_F("NAT: UDP socket begin(%u) FAILED", entry->natPort);
            ctx->droppedPackets++;
            return;
        }
        ctx->udpSocketBound[udpIdx] = true;
    }

    // Send the UDP packet to the remote, checking every step (none was checked, so a routing or WiFi failure here made the packet vanish with no counter or log).
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

// ---- send a UDP packet to the client (vintage computer) ----

void natSendUdpToClient(NatContext* ctx, NatUdpEntry* entry,
                                uint8_t* data, uint16_t length) {
    uint16_t totalLen = 20 + 8 + length;  // IP header + UDP header + data
    uint8_t packet[totalLen];
    memset(packet, 0, totalLen);

    IpHeader* ip = (IpHeader*)packet;
    ip->versionIhl = 0x45;
    ip->tos = 0;
    ip->totalLength = htons(totalLen);
    ip->id = htons(random(1, 65535));
    ip->flagsFragment = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    // IP addresses in network byte order from the IPAddress bytes
    ip->srcIP = entry->dstIP[0] | ((uint32_t)entry->dstIP[1] << 8) |
                ((uint32_t)entry->dstIP[2] << 16) | ((uint32_t)entry->dstIP[3] << 24);
    ip->dstIP = entry->srcIP[0] | ((uint32_t)entry->srcIP[1] << 8) |
                ((uint32_t)entry->srcIP[2] << 16) | ((uint32_t)entry->srcIP[3] << 24);
    ipRecalcChecksum(ip);

    UdpHeader* udp = (UdpHeader*)(packet + 20);
    udp->srcPort = htons(entry->dstPort);
    udp->dstPort = htons(entry->srcPort);
    udp->length = htons(8 + length);

    if (data && length > 0) {
        memcpy(packet + 28, data, length);
    }

    udp->checksum = 0;
    udp->checksum = htons(tcpUdpChecksum(ip, (uint8_t*)udp, 8 + length));

    natSendToClient(ctx, packet, totalLen);
}
