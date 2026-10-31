// slip_nat_tcp.cpp - TCP: the Palm's packets, the sockets to the world, the segments back. Part of the SLIP NAT; include/slip_nat_internal.h says how the files divide the work.
#include "slip_nat_internal.h"

// ---- big buffers in PSRAM ----
// What is large and touched only from the loop task lives in PSRAM: the queue of what the Palm sent towards the server and the read-ahead of what the server sent. The serial
// driver's own buffer stays in internal RAM (its interrupt uses it). Falls back to the heap without PSRAM.
uint8_t* natBigAlloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n);
    return (uint8_t*) p;
}

void natFreeTxq(NatTcpEntry* e) {
    if (e->txq) { free(e->txq); e->txq = nullptr; }
    e->txqSize = e->txqHead = e->txqLen = 0;
}

uint32_t natTxqFree(const NatTcpEntry* e) {
    return e->txq ? e->txqSize - e->txqLen : 0;
}

void natTxqPush(NatTcpEntry* e, const uint8_t* d, uint32_t n) {
    uint32_t tail = (e->txqHead + e->txqLen) % e->txqSize;
    uint32_t first = e->txqSize - tail;
    if (first > n) first = n;
    memcpy(e->txq + tail, d, first);
    if (n > first) memcpy(e->txq, d + first, n - first);
    e->txqLen += n;
}

// Hands the server socket what it will take now, never waiting.
void natTxqDrain(NatTcpEntry* e) {
    while (e->txqLen > 0 && e->client) {
        uint32_t chunk = e->txqSize - e->txqHead;
        if (chunk > e->txqLen) chunk = e->txqLen;
        ssize_t r = send(e->client->fd(), e->txq + e->txqHead, chunk, MSG_DONTWAIT);
        if (r <= 0) break;
        e->txqHead = (e->txqHead + (uint32_t) r) % e->txqSize;
        e->txqLen -= (uint32_t) r;
        if ((uint32_t) r < chunk) break;
    }
}

// Before the connection closes for good: the queue goes out first (waits up to ms).
void natTxqFlush(NatTcpEntry* e, uint32_t ms) {
    uint32_t t0 = millis();
    while (e->txqLen > 0 && e->client && (millis() - t0) < ms) {
        natTxqDrain(e);
        if (e->txqLen > 0) delay(2);
    }
}

// The window to advertise to the Palm: what the queue can still take (up to a cap), or a small fixed one.
uint16_t natAdvWindow(const NatTcpEntry* e) {
    if (!e->txq) return NAT_TCP_ADVERTISED_WINDOW;
    uint32_t w = natTxqFree(e);
    if (w > NAT_TCP_TXQ_WINDOW_MAX) w = NAT_TCP_TXQ_WINDOW_MAX;
    return (uint16_t) w;
}

// ---- Process TCP packet ----

// Scans a SYN's TCP options for a Maximum Segment Size option (kind 2, length 4, 2-byte value); 0 if absent or malformed. optionsLen is tcpHeaderLen - 20.
uint16_t natParseMss(const uint8_t* options, uint8_t optionsLen) {
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

void natProcessTcp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen) {
    // minimum TCP header is 20 bytes
    if (length < ipHeaderLen + 20) {
        ctx->droppedPackets++;
        return;
    }

    TcpHeader* tcp = (TcpHeader*)(packet + ipHeaderLen);
    uint16_t srcPort = ntohs(tcp->srcPort);
    uint16_t dstPort = ntohs(tcp->dstPort);
    uint8_t* dstBytes = (uint8_t*)&ip->dstIP;
    uint8_t* srcBytes = (uint8_t*)&ip->srcIP;
    IPAddress dstIP(dstBytes[0], dstBytes[1], dstBytes[2], dstBytes[3]);
    IPAddress srcIP(srcBytes[0], srcBytes[1], srcBytes[2], srcBytes[3]);

    uint8_t tcpHeaderLen = ((tcp->dataOffset >> 4) & 0x0F) * 4;
    if (tcpHeaderLen < 20 || ipHeaderLen + tcpHeaderLen > length) {
        ctx->droppedPackets++;
        return;
    }

    // Verify the TCP checksum. natProcessPacket checks only the IP header's, so a bit flip anywhere in the other ~96% of a packet used to be acted on: corrupted payload was
    // forwarded to the server (uploads of the right size, wrong content) and a corrupted ackNum desynced flow control (seen: serverAck jumping backwards). TCP checksums are mandatory.
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
            ctx->statTcpBadCksum++;
            return;
        }
    }

    uint8_t flags = tcp->flags;

    uint16_t payloadLen = length - ipHeaderLen - tcpHeaderLen;
    uint8_t* payload = packet + ipHeaderLen + tcpHeaderLen;

    // find the connection, or create one on SYN
    NatTcpEntry* entry = natFindTcpEntry(ctx, srcIP, srcPort, dstIP, dstPort);

    NAT_DEBUG_F("NAT: TCP %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d flags=0x%02X entry=%s",
                srcIP[0], srcIP[1], srcIP[2], srcIP[3], srcPort,
                dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort,
                flags, entry ? "found" : "null");

    if (flags & TCP_FLAG_SYN) {
        NAT_DEBUG_F("NAT: TCP SYN state=%d", entry ? entry->state : -1);

        // a SYN-ACK from the Palm (port forwarding)?
        if ((flags & TCP_FLAG_ACK) && entry && entry->state == NAT_TCP_ESTABLISHED) {
            // The vintage computer answers our SYN (port forwarding): complete the 3-way handshake.
            entry->clientSeq = ntohl(tcp->seqNum) + 1;
            entry->clientAck = ntohl(tcp->ackNum);

            // The Palm acknowledged our SYN; flow control needs this.
            entry->serverAck = ntohl(tcp->ackNum);

            entry->lastActivity = millis();
            entry->lastKeepalive = millis();

            NAT_DEBUG_F("NAT: Port forward SYN-ACK received, sending ACK (clientSeq=%u, serverAck=%u)",
                        entry->clientSeq, entry->serverAck);

            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
            ctx->tcpConnections++;
            return;
        }

        // new connection request
        if (!entry) {
            // a stuck connection to the same destination: reuse (helps reconnecting)
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
                        natFreeTxq(e);
                        e->rxBufferLen = 0;
                        e->rxBufferSize = 0;
                    }
                    e->active = false;
                }
            }

            entry = natCreateTcpEntry(ctx, srcIP, srcPort, dstIP, dstPort);
            if (!entry) {
                // table full
                ctx->droppedPackets++;
                NAT_DEBUG("NAT: TCP table full");
                natDumpTcpTable(ctx);
                return;
            }
        }

        // initial sequence number (+1: the SYN consumes one)
        entry->clientSeq = ntohl(tcp->seqNum) + 1;
        entry->clientAck = ntohl(tcp->ackNum);
        // A window of 0 is legitimate ("don't send yet") and must be honoured (see the natPollConnections send gate).
        entry->clientWindow = ntohs(tcp->window);

        // The client's MSS from the SYN's options. A fixed 1000-byte cap broke clients with a smaller MSS (RFC 879 default 536 without the option): a correct receiver may drop
        // segments over its MSS, so the handshake worked but ~1000-byte segments were never acknowledged.
        {
            uint8_t optionsLen = tcpHeaderLen - 20;
            uint16_t parsedMss = (optionsLen > 0)
                ? natParseMss(packet + ipHeaderLen + 20, optionsLen)
                : 0;
            entry->clientMss = (parsedMss > 0) ? parsedMss : NAT_TCP_DEFAULT_MSS;
            NAT_DEBUG_F("NAT: TCP SYN clientMss=%u (parsed=%u)", entry->clientMss, parsedMss);
        }

        // Start the connection to the server without blocking. A blocking WiFiClient::connect() (3 s) froze the whole gateway (no UART draining, no relaying), and a failed attempt
        // marked the entry inactive so the Palm's retransmitted SYN started a full new attempt. Now a non-blocking socket is handed to natPollConnections (the NAT_TCP_SYN_SENT branch),
        // and a retransmitted SYN while pendingSockFd is valid is absorbed by the state check below.
        if (entry->state == NAT_TCP_CLOSED) {
            // Hairpin: a connect to an address that is WiRSa itself (its WiFi IP, AP IP or the SLIP-side gateway address ctx->slipIP) goes to loopback. Over the WiFi radio it would
            // have to be reflected back by the AP to the same station, which most APs and drivers do not do (client isolation, or the driver dropping looped frames): the handshake sometimes
            // worked and the replies never came (seen against the FTP server over SLIP). WiFiServer/FtpServer bind INADDR_ANY, so loopback reaches the same listening socket.
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

            // The connect is in progress (normal) or already done (rare): natPollConnections' NAT_TCP_SYN_SENT branch finishes the handshake without blocking.
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
        // no connection for this packet
        ctx->droppedPackets++;
        NAT_DEBUG_F("NAT: TCP no entry for %d.%d.%d.%d:%d",
                    dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort);
        return;
    }

    entry->lastActivity = millis();

    if (flags & TCP_FLAG_FIN) {
        // True if WE closed first (natPollConnections' "closed by remote" path already sent our FIN): a simultaneous close, or a late client FIN. Capture it BEFORE the state
        // assignment below overwrites it.
        bool alreadyClosing = (entry->state == NAT_TCP_FIN_WAIT ||
                                entry->state == NAT_TCP_CLOSING);

        entry->state = NAT_TCP_FIN_WAIT;
        // A FIN consumes one sequence number, like a SYN. A client whose FIN is not acked at the right number retransmits it forever.
        entry->clientSeq = ntohl(tcp->seqNum) + 1;
        if (entry->client) {
            natTxqFlush(entry, 2000);   // what the Palm sent before closing still goes to the server
            entry->client->stop();
        }
        natFreeTxq(entry);
        if (entry->pendingSockFd >= 0) {
            // The client closes a connection whose async connect() never finished: abandon it so the fd does not leak.
            close(entry->pendingSockFd);
            entry->pendingSockFd = -1;
        }
        if (alreadyClosing) {
            // We already sent our FIN. A retransmitted peer segment must get the identical segment back (PalmOS's TCP does not take a bare ACK or a new FIN as acking its FIN): resend
            // our FIN unchanged by saving and restoring serverSeq around natSendTcpToClient's advance.
            uint32_t savedServerSeq = entry->serverSeq;
            NAT_DEBUG_F("NAT: TCP FIN (already closing) -> retransmit FIN|ACK, serverSeq=%u",
                        entry->serverSeq);
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
            entry->serverSeq = savedServerSeq;
        } else {
            // First close seen: ACK the client's FIN and send ours in the same reply.
            entry->closingStartTime = millis();
            NAT_DEBUG_F("NAT: TCP FIN (first close) -> FIN|ACK, serverSeq=%u",
                        entry->serverSeq);
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
        }
        return;
    }

    if (flags & TCP_FLAG_RST) {
        if (entry->client) {
            entry->client->stop();
            delete entry->client;
            entry->client = nullptr;
        }
        if (entry->pendingSockFd >= 0) {
            // The client aborted while the async connect() was running: close the fd rather than leak it.
            close(entry->pendingSockFd);
            entry->pendingSockFd = -1;
        }
        if (entry->rxBuffer) {
            free(entry->rxBuffer);
            entry->rxBuffer = nullptr;
            entry->rxBufferLen = 0;
            entry->rxBufferSize = 0;
        }
        natFreeTxq(entry);
        entry->active = false;
        return;
    }

    // data from the client
    if (payloadLen > 0 && entry->state == NAT_TCP_ESTABLISHED) {
        uint32_t segSeq = ntohl(tcp->seqNum);

        if (segSeq != entry->clientSeq) {
            // Not the next byte expected: a stale retransmit (the client never saw our ACK; likelier now that bad checksums are dropped) or out-of-order arrival. There is no reorder
            // buffer, so forwarding it would duplicate or misorder bytes with no way for the server to notice. Drop it and re-ACK the current position; the client's retransmit timer recovers.
            int32_t delta = (int32_t)(segSeq - entry->clientSeq);
            NAT_DEBUG_F("NAT: TCP client->server out-of-sequence: got seq=%u expected=%u (delta=%ld) - dropping, re-ACKing",
                        segSeq, entry->clientSeq, (long) delta);
            entry->lastActivity = millis();
            ctx->statOutOfSeq++;
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
        } else if (entry->client && entry->client->connected() && entry->txq) {
            // Queue it and acknowledge at once; the loop hands the queue to the server socket without waiting.
            if (payloadLen <= natTxqFree(entry)) {
                natTxqPush(entry, payload, payloadLen);
                if (entry->txqLen > ctx->statQueueMax) ctx->statQueueMax = entry->txqLen;
                entry->clientSeq = segSeq + payloadLen;
                ctx->packetsToInternet++;
            } else {
                ctx->statQueueFull++;      // more than the window allowed: the Palm sends it again
            }
            entry->lastActivity = millis();
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
        } else if (entry->client && entry->client->connected()) {
            size_t written = entry->client->write(payload, payloadLen);
            if (written != payloadLen) {
                NAT_DEBUG_F("NAT: TCP client->server write failed: %d/%d bytes", written, payloadLen);
                ctx->statPartialWrites++;
            } else {
                NAT_DEBUG_F("NAT: TCP client->server: %d bytes", payloadLen);
            }
            // Acknowledge only what the server socket took: a short write (the server stopped reading past the send timeout) used to be acked in full, losing the rest. The Palm resends it.
            entry->clientSeq = segSeq + (uint32_t) written;
            ctx->packetsToInternet++;
            entry->lastActivity = millis();

            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
        } else {
            NAT_DEBUG("NAT: TCP client->server failed: not connected");
        }
    }

    // the client's ACK covers data WE sent
    if (flags & TCP_FLAG_ACK) {
        entry->lastActivity = millis();

        // NAT_TCP_ESTABLISHED is set when OUR leg to the server connects (natPollPendingConnect); that does not show the Palm finished its handshake. The first ACK-flagged
        // packet after it is the real "client connected" event; logged once per connection.
        if (!entry->clientHandshakeAcked && entry->state == NAT_TCP_ESTABLISHED) {
            entry->clientHandshakeAcked = true;
            NAT_DEBUG_F("NAT: *** Client CONNECTED: %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d ***",
                        srcIP[0], srcIP[1], srcIP[2], srcIP[3], srcPort,
                        dstIP[0], dstIP[1], dstIP[2], dstIP[3], dstPort);
        }

        uint32_t newAck = ntohl(tcp->ackNum);
        // Fast retransmit: three ACKs repeating the same point with the same window, no data and bytes outstanding mean the Palm saw a gap (a segment damaged on the line).
        // Send again at once instead of waiting out the timeout.
        if (newAck == entry->serverAck && payloadLen == 0 && !(flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) &&
            entry->serverSeq != entry->serverAck && ntohs(tcp->window) == entry->lastAckWindow) {
            ctx->statDupAcks++;
            if (++entry->dupAcks >= 3) {
                entry->serverSeq = entry->serverAck;
                entry->dupAcks = 0;
                entry->lastAckTime = millis();
                ctx->statFastRetx++;
                slipPaceBackoff();
                NAT_DEBUG_F("NAT: TCP fast retransmit from %u", entry->serverAck);
            }
        } else {
            entry->dupAcks = 0;
        }
        entry->lastAckWindow = ntohs(tcp->window);
        if (newAck != entry->serverAck) {
            // Signed delta (modular, as TCP) so a stale or reordered ACK after a later one cannot pass for progress and corrupt the buffer trim below.
            int32_t delta = (int32_t)(newAck - entry->serverAck);
            if (delta < 0) {
                // A stale, reordered or duplicate ACK (possible on any network, with a valid checksum) must NOT roll serverAck backwards: serverAck feeds the unsigned
                // inFlight = serverSeq - serverAck, so a regression inflates inFlight, and one past serverSeq would wrap to a huge value. Ignore it.
                NAT_DEBUG_F("NAT: TCP stale ACK ignored: serverAck=%u, got=%u (delta=%ld)",
                            entry->serverAck, newAck, (long) delta);
            } else {
                NAT_DEBUG_F("NAT: TCP ACK update: serverAck %u -> %u", entry->serverAck, newAck);
                entry->serverAck = newAck;
                slipPaceAcked((uint32_t) delta);

                if (entry->rxBuffer) {
                    // Real progress: delta more bytes confirmed, so only now discard them from the front of rxBuffer. The send path in natPollConnections leaves sent-but-unacked bytes in
                    // place so they can be retransmitted. Clamp: the client should never ack more than we buffered.
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
        // Track the client's advertised window so natPollConnections sizes its sliding window from it. A real 0 (the client's buffer is full) must be honoured: ignoring it let the
        // gateway send past what the client could hold (no retransmit buffer to recover from), a permanent inFlight/serverAck desync only the 60 s stall timeout cleared.
        entry->clientWindow = ntohs(tcp->window);
    }
}

// ---- Poll connections for incoming data ----

// ---- Poll a non-blocking connect in progress ----
// Called from natPollConnections for every NAT_TCP_SYN_SENT entry with a pending raw socket. Never blocks: a zero-timeout select() asks whether the fd is writable or errored.
// On success the fd becomes a WiFiClient and a SYN-ACK goes out; on failure or timeout it fails as the old blocking path did (RST, deactivate) without freezing the gateway.

void natPollPendingConnect(NatContext* ctx, NatTcpEntry* entry, int idx) {
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

    // Connected: promote the raw fd to a WiFiClient so the existing read/write/available/connected calls work. Clear O_NONBLOCK and set the read/write timeouts that
    // WiFiClient::connect() would have set.
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) & ~O_NONBLOCK);

    entry->client = new WiFiClient(sockfd);
    entry->pendingSockFd = -1;

    struct timeval sockTv;
    sockTv.tv_sec = NAT_TCP_CONNECT_TIMEOUT_MS / 1000;
    sockTv.tv_usec = (NAT_TCP_CONNECT_TIMEOUT_MS % 1000) * 1000;
    entry->client->setSocketOption(SOL_SOCKET, SO_RCVTIMEO, &sockTv, sizeof(sockTv));
    // A write to the server must not hold the loop for seconds (the Palm's bytes pile up in the serial buffer meanwhile). 400 ms is plenty for a socket with room; a short write is acked only as far as it went.
    sockTv.tv_sec = 0;
    sockTv.tv_usec = NAT_TCP_WRITE_TIMEOUT_MS * 1000;
    entry->client->setSocketOption(SOL_SOCKET, SO_SNDTIMEO, &sockTv, sizeof(sockTv));
    entry->client->setNoDelay(true);   // small request/response exchanges (HotSync) must not wait for Nagle
    entry->txq = natBigAlloc(NAT_TCP_TXQ_SIZE);
    entry->txqSize = entry->txq ? NAT_TCP_TXQ_SIZE : 0;
    entry->txqHead = entry->txqLen = 0;

    entry->state = NAT_TCP_ESTABLISHED;
    ctx->packetsToInternet++;
    ctx->tcpConnections++;
    NAT_DEBUG_F("NAT: TCP[%d] async connect succeeded", idx);

    // serverAck baseline: the SYN-ACK below uses one sequence number, so start one above serverSeq or it counts as a data byte in flight. With serverSeq, data a server sends at once
    // (an FTP greeting) went out from its second byte, and the Palm's ACK of the SYN-ACK trimmed one real byte.
    entry->serverAck = entry->serverSeq + 1;
    entry->lastProbe = millis();
    entry->lastAckTime = millis();

    natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_SYN | TCP_FLAG_ACK);
}

int natPollConnections(NatContext* ctx) {
    int packetsSent = 0;

    for (int i = 0; i < NAT_MAX_TCP_CONNECTIONS; i++) {
        NatTcpEntry* entry = &ctx->tcpTable[i];
        if (!entry->active) continue;

        // Finish a non-blocking connect() still running (natProcessTcp's SYN handling starts it).
        if (entry->state == NAT_TCP_SYN_SENT && entry->pendingSockFd >= 0) {
            natPollPendingConnect(ctx, entry, i);
            continue;  // re-evaluated (possibly as ESTABLISHED) next iteration
        }

        if (!entry->client) continue;

        // The Palm's queue goes to the server socket, and the Palm is told when room opens (it stops at a zero window and would wait for a probe).
        if (entry->txq) {
            if (entry->txqLen > 0) natTxqDrain(entry);
            uint16_t adv = natAdvWindow(entry);
            if (entry->state == NAT_TCP_ESTABLISHED && adv > entry->lastAdvWindow &&
                (adv - entry->lastAdvWindow >= 2048 || entry->lastAdvWindow < NAT_TCP_DEFAULT_MSS)) {
                natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
                packetsSent++;
            }
        }

        // Data relay: read greedily from the server socket into a per-connection heap buffer (rxBuffer), and drain it to the client paced strictly by the client's advertised window
        // (clientWindow), a genuine 0 included. There is no retransmit buffer: clamping the window up to a minimum once caused silent data loss when the real window was smaller (serverSeq
        // advanced as if delivered, the ACKs could never catch up, and the 60 s stall timeout tore the connection down mid-transfer). Reading into our own buffer also decouples the fast
        // WiFi side from the slow serial side.
        if (entry->state == NAT_TCP_ESTABLISHED) {
            if (!entry->rxBuffer) {
                entry->rxBuffer = natBigAlloc(NAT_TCP_RXBUF_SIZE);
                if (entry->rxBuffer) {
                    entry->rxBufferSize = NAT_TCP_RXBUF_SIZE;
                    entry->rxBufferLen = 0;
                } else {
                    NAT_DEBUG_F("NAT: TCP[%d] rxBuffer alloc failed (low heap?)", i);
                }
            }

            // Top up from the socket, independent of the client's window, bounded by our buffer.
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

            // Drain to the client, paced by its real window. Bytes leave rxBuffer only when the client's ACK confirms them (the trim in natProcessTcp), so a segment lost on the
            // flow-control-free serial link can be retransmitted. Discarding on send meant one dropped byte stalled a large transfer until the 60 s stall timeout (large FTP listings bailed partway).
            if (entry->rxBuffer && entry->rxBufferLen > 0) {
                uint32_t inFlight = entry->serverSeq - entry->serverAck;
                uint32_t effectiveWindow = entry->clientWindow;
                if (effectiveWindow > NAT_TCP_MAX_WINDOW) effectiveWindow = NAT_TCP_MAX_WINDOW;

                // Retransmit on timeout: if bytes are outstanding and no ACK progress for NAT_TCP_RETRANSMIT_MS, assume loss and rewind serverSeq to the last confirmed ACK; the send logic
                // below then re-emits the same bytes from the still-buffered unacked region. Safe if the cause is a full window: a receiver without room re-acks the same point, as in TCP.
                // The timeout follows the line speed: the time for what is outstanding plus the ACK to cross the serial line twice, on a base for the Palm's own delay. A fixed 3 s made every lost
                // segment cost 3 s however fast the line.
                uint32_t baud = slipPaceRate();     // the pace the Palm gets, not the line rate
                uint32_t rto = NAT_TCP_RETRANSMIT_BASE_MS + (baud ? (uint32_t) ((uint64_t) (inFlight + 80) * 20000ULL / baud) : 0);
                if (rto > NAT_TCP_RETRANSMIT_MS) rto = NAT_TCP_RETRANSMIT_MS;
                if (inFlight > 0 && (millis() - entry->lastAckTime) > rto) {
                    ctx->statRtoRewinds++;
                    slipPaceBackoff();           // the Palm lost something: send slower
                    NAT_DEBUG_F("NAT: TCP[%d] retransmit: no ACK progress for %lums (inFlight=%u), rewinding",
                                i, millis() - entry->lastAckTime, inFlight);
                    entry->serverSeq = entry->serverAck;
                    inFlight = 0;
                    entry->lastAckTime = millis();
                }

                if (inFlight < effectiveWindow) {
                    // Segments are capped at 1000 bytes, and never above the MSS the client's SYN gave (entry->clientMss): a larger one may be silently dropped.
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
                        // Not trimming rxBuffer: these bytes stay for retransmission until natProcessTcp's ACK handling confirms them.
                        entry->lastActivity = millis();
                        entry->lastServerData = millis();
                        entry->lastAckTime = millis();
                        packetsSent++;
                    }
                } else {
                    // The client's window is used up (maybe a genuine 0): wait for it to ACK or reopen. A stall only if it lasts too long.
                    unsigned long now = millis();
                    unsigned long waitTime = now - entry->lastActivity;

                    static unsigned long lastWaitDebug = 0;
                    if (now - lastWaitDebug > 5000) {
                        NAT_DEBUG_F("NAT: TCP[%d] WAITING: inFlight=%u, window=%u, buffered=%u, wait=%lums",
                                    i, inFlight, entry->clientWindow, entry->rxBufferLen, waitTime);
                        lastWaitDebug = now;
                    }

                    // Zero-window probe (RFC 1122 4.2.2.17): a receiver need not send an unsolicited window update once its buffer drains, and a client stack that does not leaves a silent
                    // deadlock that only the stall timeout ends. Push 1 byte of the next unsent data past the stated window; the ACK it must send back shows the real state. Only if there is a next
                    // unsent byte; if everything buffered is already outstanding, the retransmit timeout covers re-poking.
                    uint32_t notYetSent = entry->rxBufferLen - inFlight;
                    if (notYetSent > 0 && now - entry->lastProbe > NAT_TCP_PROBE_INTERVAL_MS) {
                        NAT_DEBUG_F("NAT: TCP[%d] zero-window probe (window=%u, buffered=%u)",
                                    i, entry->clientWindow, entry->rxBufferLen);
                        natSendTcpToClient(ctx, entry, entry->rxBuffer + inFlight, 1,
                                           TCP_FLAG_PSH | TCP_FLAG_ACK);
                        // Not trimmed: the same retransmit-until-acked rule as the main send path.
                        entry->lastProbe = now;
                    }

                    // stall detection: close if the window has not moved for too long
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
                        natFreeTxq(entry);
                        entry->active = false;
                        continue;
                    }
                }
            }
        }

        // Closed by the server: only once the data already pulled off the closed socket is fully drained to the client, so a server closing right after its last write does not truncate a listing or download.
        if (entry->client && !entry->client->connected() && entry->state == NAT_TCP_ESTABLISHED
            && (!entry->rxBuffer || entry->rxBufferLen == 0)) {
            NAT_DEBUG_F("NAT: TCP[%d] remote closed -> sending our own FIN|ACK, serverSeq=%u",
                        i, entry->serverSeq);
            natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_FIN | TCP_FLAG_ACK);
            entry->state = NAT_TCP_CLOSING;
            entry->closingStartTime = millis();
        }

        // TCP keepalive: a periodic ACK
        if (entry->state == NAT_TCP_ESTABLISHED) {
            unsigned long now = millis();
            if (now - entry->lastKeepalive > NAT_TCP_KEEPALIVE_MS) {
                NAT_DEBUG_F("NAT: TCP[%d] sending keepalive", i);
                natSendTcpToClient(ctx, entry, nullptr, 0, TCP_FLAG_ACK);
                entry->lastKeepalive = now;
                entry->lastActivity = now;
            }

            // server silence: warn after 5+ minutes without data
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

    // Poll each active UDP entry's own socket for a reply (each entry owns a socket, NatContext::udpSockets), so what arrives belongs to that entry. A shared socket could match
    // a reply only by remote address and misrouted it to a stale entry with the same dst:port (e.g. an NTP retry opening a new entry before the old one expired).
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

        // A plain UDP socket accepts a datagram from any sender to this port: reject anything not from the server this entry queried.
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

        // Release the entry and its socket after one response, for every protocol (not only DNS). Request/response protocols do not need the socket after the answer, and holding
        // non-DNS sockets for the idle timeout risks CONFIG_LWIP_MAX_SOCKETS, a system-wide ceiling shared with TCP.
        entry->active = false;
        ctx->udpSockets[i].stop();
        ctx->udpSocketBound[i] = false;
        NAT_DEBUG_F("NAT: Released UDP entry %d after response (dst port %d)", i, entry->dstPort);
    }

    // pending ICMP replies
    natProcessPendingIcmp(ctx);

    return packetsSent;
}

// ---- Send a TCP packet to the client (vintage computer) ----

void natSendTcpToClient(NatContext* ctx, NatTcpEntry* entry,
                                uint8_t* data, uint16_t length, uint8_t flags) {
    uint16_t ipHeaderLen = 20;
    uint16_t tcpHeaderLen = 20;
    uint16_t totalLen = ipHeaderLen + tcpHeaderLen + length;
    uint8_t packet[60 + 1460];  // Max IP header + TCP header + data

    if (totalLen > sizeof(packet)) {
        return;
    }

    memset(packet, 0, totalLen);

    IpHeader* ip = (IpHeader*)packet;
    ip->versionIhl = 0x45;  // IPv4, 5 words header
    ip->tos = 0;
    ip->totalLength = htons(totalLen);
    ip->id = htons(random(1, 65535));
    ip->flagsFragment = htons(0x4000);  // Don't fragment
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    // IP addresses in network byte order from the IPAddress bytes
    ip->srcIP = entry->dstIP[0] | ((uint32_t)entry->dstIP[1] << 8) |
                ((uint32_t)entry->dstIP[2] << 16) | ((uint32_t)entry->dstIP[3] << 24);
    ip->dstIP = entry->srcIP[0] | ((uint32_t)entry->srcIP[1] << 8) |
                ((uint32_t)entry->srcIP[2] << 16) | ((uint32_t)entry->srcIP[3] << 24);

    TcpHeader* tcp = (TcpHeader*)(packet + ipHeaderLen);
    tcp->srcPort = htons(entry->dstPort);
    tcp->dstPort = htons(entry->srcPort);
    tcp->seqNum = htonl(entry->serverSeq);
    tcp->ackNum = htonl(entry->clientSeq);  // clientSeq is already "next expected byte"
    tcp->dataOffset = 0x50;  // 5 words header
    tcp->flags = flags;
    tcp->window = htons(natAdvWindow(entry));
    entry->lastAdvWindow = natAdvWindow(entry);
    tcp->urgentPtr = 0;

    if (data && length > 0) {
        memcpy(packet + ipHeaderLen + tcpHeaderLen, data, length);
    }

    // advance the sequence for the next packet
    if (flags & TCP_FLAG_SYN) {
        entry->serverSeq++;
    }
    if (flags & TCP_FLAG_FIN) {
        entry->serverSeq++;
    }
    if (length > 0) {
        entry->serverSeq += length;
    }

    ipRecalcChecksum(ip);
    tcp->checksum = 0;
    tcp->checksum = htons(tcpUdpChecksum(ip, (uint8_t*)tcp, tcpHeaderLen + length));

    natSendToClient(ctx, packet, totalLen);
    ctx->packetsFromInternet++;
}
