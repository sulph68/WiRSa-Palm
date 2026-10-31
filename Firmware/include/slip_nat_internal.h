// slip_nat_internal.h - what the files of the SLIP NAT share. Not for the rest of the firmware: that sees only slip_nat.h.
//
//   slip_nat.cpp       the NAT context: init and shutdown, checksums, the entry tables, expiry, the packet to the Palm
//   slip_nat_tcp.cpp   TCP: packets from the Palm, the non-blocking sockets to the world, the segments back
//   slip_nat_udp.cpp   UDP
//   slip_nat_icmp.cpp  ICMP echo through a raw lwIP socket
#pragma once
#include "slip_nat.h"
#include "slip.h"
#include "globals.h"
#include "serial_io.h"
#include <EEPROM.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>  // socket/connect/select/fcntl for non-blocking connect

// lwIP includes for raw ICMP socket
extern "C" {
#include "lwip/raw.h"
#include "lwip/ip_addr.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
}

extern bool usbDebug;

#define NAT_DEBUG(msg) LOGV("%s", String(msg).c_str())
#define NAT_DEBUG_F(fmt, ...) LOGV(fmt, ##__VA_ARGS__)

// External SLIP context (defined in slip_mode.cpp)
extern SlipContext slipCtx;

// The ICMP callback has no user data in lwIP: it finds the context here (set by natInit, cleared by natShutdown)
extern NatContext* g_icmpNatCtx;

// Functions the files share (each is defined in the file named)

// ---- slip_nat.cpp ----
NatTcpEntry* natFindTcpEntry(NatContext* ctx, IPAddress srcIP,
                                     uint16_t srcPort, IPAddress dstIP,
                                     uint16_t dstPort);
void natDumpTcpTable(NatContext* ctx);
__attribute__((noinline)) NatTcpEntry* natCreateTcpEntry(NatContext* ctx, IPAddress srcIP,
                                       uint16_t srcPort, IPAddress dstIP,
                                       uint16_t dstPort);
NatUdpEntry* natFindOrCreateUdp(NatContext* ctx, IPAddress srcIP,
                                        uint16_t srcPort, IPAddress dstIP,
                                        uint16_t dstPort);

// ---- slip_nat_tcp.cpp ----
uint8_t* natBigAlloc(size_t n);
void natFreeTxq(NatTcpEntry* e);
uint32_t natTxqFree(const NatTcpEntry* e);
void natTxqPush(NatTcpEntry* e, const uint8_t* d, uint32_t n);
void natTxqDrain(NatTcpEntry* e);
void natTxqFlush(NatTcpEntry* e, uint32_t ms);
uint16_t natAdvWindow(const NatTcpEntry* e);
uint16_t natParseMss(const uint8_t* options, uint8_t optionsLen);
void natProcessTcp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen);
void natPollPendingConnect(NatContext* ctx, NatTcpEntry* entry, int idx);
void natSendTcpToClient(NatContext* ctx, NatTcpEntry* entry,
                                uint8_t* data, uint16_t length, uint8_t flags);

// ---- slip_nat_udp.cpp ----
void natProcessUdp(NatContext* ctx, uint8_t* packet, uint16_t length,
                          IpHeader* ip, uint8_t ipHeaderLen);
void natSendUdpToClient(NatContext* ctx, NatUdpEntry* entry,
                                uint8_t* data, uint16_t length);

// ---- slip_nat_icmp.cpp ----
u8_t natIcmpRecvCallback(void* arg, struct raw_pcb* pcb,
                                 struct pbuf* p, const ip_addr_t* addr);
void natProcessIcmp(NatContext* ctx, uint8_t* packet, uint16_t length,
                           IpHeader* ip, uint8_t ipHeaderLen);
void natSendIcmpToClient(NatContext* ctx, NatIcmpEntry* entry,
                                 uint8_t type, uint8_t code,
                                 uint8_t* data, uint16_t length);
