// slip_nat_icmp.cpp - ICMP: echo through a raw lwIP socket. Part of the SLIP NAT; include/slip_nat_internal.h says how the files divide the work.
#include "slip_nat_internal.h"

// ---- pending ICMP reply buffer (deferred processing from the callback) ----

static struct {
    volatile bool pending;  // volatile: written by lwIP callback (Core 0), read by main loop (Core 1)
    IPAddress srcIP;
    uint16_t id;
    uint16_t seq;
    uint8_t data[64];   // Ping payload (typically 32 bytes)
    uint16_t dataLen;
} g_pendingIcmpReply = {false};

// ---- ICMP raw socket receive callback ----

u8_t natIcmpRecvCallback(void* arg, struct raw_pcb* pcb,
                                 struct pbuf* p, const ip_addr_t* addr) {
    (void)arg;  // Use global context instead

    // the pbuf includes the IP header: skip it
    if (p->tot_len < 28) {  // Min IP (20) + Min ICMP (8)
        return 0;
    }

    uint8_t* ipData = (uint8_t*)p->payload;
    uint8_t ipHeaderLen = (ipData[0] & 0x0F) * 4;

    if (p->tot_len < ipHeaderLen + 8) {
        return 0;
    }

    struct icmp_echo_hdr* icmpHdr = (struct icmp_echo_hdr*)(ipData + ipHeaderLen);

    // only echo reply (type 0)
    if (icmpHdr->type != 0) {
        return 0;
    }

    uint16_t icmpId = ntohs(icmpHdr->id);
    uint16_t icmpSeq = ntohs(icmpHdr->seqno);

    const ip4_addr_t* ip4 = &addr->u_addr.ip4;
    IPAddress srcIP((ip4->addr >> 0) & 0xFF, (ip4->addr >> 8) & 0xFF,
                    (ip4->addr >> 16) & 0xFF, (ip4->addr >> 24) & 0xFF);

    if (!g_pendingIcmpReply.pending) {
        g_pendingIcmpReply.srcIP = srcIP;
        g_pendingIcmpReply.id = icmpId;
        g_pendingIcmpReply.seq = icmpSeq;

        // ping payload (after the 8-byte ICMP header)
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

    // CRITICAL: free the pbuf when returning 1 (consumed). lwIP's raw_recv contract: the callback owns the pbuf when it returns non-zero; missing this leaks memory until the pbuf pool is exhausted.
    pbuf_free(p);
    return 1;  // Consumed - we handle all echo replies
}

// ---- process a pending ICMP reply (from the main loop) ----

void natProcessPendingIcmp(NatContext* ctx) {
    if (!g_pendingIcmpReply.pending) {
        return;
    }

    NAT_DEBUG_F("NAT: Processing ICMP reply from %d.%d.%d.%d id=%d seq=%d",
                g_pendingIcmpReply.srcIP[0], g_pendingIcmpReply.srcIP[1],
                g_pendingIcmpReply.srcIP[2], g_pendingIcmpReply.srcIP[3],
                g_pendingIcmpReply.id, g_pendingIcmpReply.seq);

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

    entry->sequence = g_pendingIcmpReply.seq;

    natSendIcmpToClient(ctx, entry, ICMP_ECHO_REPLY, 0,
                         g_pendingIcmpReply.data, g_pendingIcmpReply.dataLen);

    NAT_DEBUG_F("NAT: Forwarded ping reply to client, %d bytes payload",
                g_pendingIcmpReply.dataLen);
    ctx->icmpPackets++;

    entry->active = false;

    g_pendingIcmpReply.pending = false;
}

// ---- process an ICMP packet (echo request/reply) ----

void natProcessIcmp(NatContext* ctx, uint8_t* packet, uint16_t length,
                           IpHeader* ip, uint8_t ipHeaderLen) {
    if (length < ipHeaderLen + 8) {
        ctx->droppedPackets++;
        return;
    }

    IcmpHeader* icmp = (IcmpHeader*)(packet + ipHeaderLen);

    if (icmp->type != ICMP_ECHO_REQUEST) {
        NAT_DEBUG_F("NAT: ICMP type %d dropped", icmp->type);
        ctx->droppedPackets++;
        return;
    }

    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    IPAddress dstIP(dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3]);
    IPAddress srcIP(srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3]);

    NAT_DEBUG_F("NAT: ICMP echo request to %d.%d.%d.%d",
                dstIP[0], dstIP[1], dstIP[2], dstIP[3]);

    // a ping for our gateway IP: answer directly
    if (dstIP == ctx->slipIP) {
        uint16_t icmpLen = length - ipHeaderLen;

        uint32_t tmp = ip->srcIP;
        ip->srcIP = ip->dstIP;
        ip->dstIP = tmp;

        icmp->type = ICMP_ECHO_REPLY;
        icmp->checksum = 0;
        icmp->checksum = htons(ipChecksum((uint8_t*)icmp, icmpLen));

        ip->ttl = 64;
        ipRecalcChecksum(ip);

        natSendToClient(ctx, packet, length);
        ctx->packetsFromInternet++;
        ctx->icmpPackets++;

        NAT_DEBUG("NAT: Responded to gateway ping");
        return;
    }

    // an external ping: forward via ICMP NAT
    if (!ctx->icmpPcb) {
        NAT_DEBUG("NAT: No ICMP socket, dropping ping");
        ctx->droppedPackets++;
        return;
    }

    uint16_t icmpId = ntohs(icmp->id);
    uint16_t icmpSeq = ntohs(icmp->sequence);

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

    uint16_t icmpLen = length - ipHeaderLen;
    struct pbuf* p = pbuf_alloc(PBUF_IP, icmpLen, PBUF_RAM);
    if (!p) {
        NAT_DEBUG("NAT: Failed to allocate pbuf for ICMP");
        ctx->droppedPackets++;
        return;
    }

    memcpy(p->payload, icmp, icmpLen);

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

// ---- send an ICMP packet to the client (vintage computer) ----

void natSendIcmpToClient(NatContext* ctx, NatIcmpEntry* entry,
                                 uint8_t type, uint8_t code,
                                 uint8_t* data, uint16_t length) {
    uint8_t packet[60 + 1472];  // Max IP + ICMP header + data
    uint16_t ipHeaderLen = 20;
    uint16_t icmpHeaderLen = 8;
    uint16_t totalLen = ipHeaderLen + icmpHeaderLen + length;

    if (totalLen > sizeof(packet)) {
        return;
    }

    IpHeader* ip = (IpHeader*)packet;
    ip->versionIhl = 0x45;
    ip->tos = 0;
    ip->totalLength = htons(totalLen);
    ip->id = htons(random(1, 65535));
    ip->flagsFragment = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    // IP addresses in network byte order from the IPAddress bytes
    ip->srcIP = entry->dstIP[0] | ((uint32_t)entry->dstIP[1] << 8) |
                ((uint32_t)entry->dstIP[2] << 16) | ((uint32_t)entry->dstIP[3] << 24);
    ip->dstIP = entry->srcIP[0] | ((uint32_t)entry->srcIP[1] << 8) |
                ((uint32_t)entry->srcIP[2] << 16) | ((uint32_t)entry->srcIP[3] << 24);

    IcmpHeader* icmp = (IcmpHeader*)(packet + ipHeaderLen);
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->id = htons(entry->id);
    icmp->sequence = htons(entry->sequence);

    if (data && length > 0) {
        memcpy(packet + ipHeaderLen + icmpHeaderLen, data, length);
    }

    uint16_t icmpTotalLen = icmpHeaderLen + length;
    icmp->checksum = htons(ipChecksum((uint8_t*)icmp, icmpTotalLen));

    ipRecalcChecksum(ip);

    natSendToClient(ctx, packet, totalLen);
    ctx->packetsFromInternet++;
}
