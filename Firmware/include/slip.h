#ifndef SLIP_H
#define SLIP_H

#include <Arduino.h>

// ---- SLIP constants (RFC 1055) ----

#define SLIP_END     0xC0   // Frame delimiter (192)
#define SLIP_ESC     0xDB   // Escape character (219)
#define SLIP_ESC_END 0xDC   // Escaped END (220) - represents END in data
#define SLIP_ESC_ESC 0xDD   // Escaped ESC (221) - represents ESC in data

#define SLIP_MTU         1500   // Maximum transmission unit (matches Ethernet)
#define SLIP_BUFFER_SIZE 1600   // Buffer size with overhead for escaping

// ---- SLIP state machine ----

enum SlipRxState {
    SLIP_RX_IDLE,       // Waiting for frame start
    SLIP_RX_RECEIVING,  // Receiving frame data
    SLIP_RX_ESCAPE      // Just received ESC, next byte is escaped
};

// ---- SLIP context ----

struct SlipContext {
    SlipRxState rxState;
    uint8_t rxBuffer[SLIP_BUFFER_SIZE];
    uint16_t rxPos;

    uint8_t txBuffer[SLIP_BUFFER_SIZE];
    uint16_t txPos;

    uint32_t framesReceived;
    uint32_t framesSent;
    uint32_t rxErrors;         // Framing errors, overruns
    uint32_t txErrors;
    uint32_t bytesReceived;    // Total bytes (before deframing)
    uint32_t bytesSent;        // Total bytes (after framing)
};

// ---- functions ----

void slipInit(SlipContext* ctx);

// Process a byte from serial: 0 = consumed, keep receiving; > 0 = complete frame length (data in ctx->rxBuffer)
int slipReceiveByte(SlipContext* ctx, uint8_t byte);

// Send a frame (IP packet) SLIP-encoded, straight to PhysicalSerial
void slipSendFrame(SlipContext* ctx, const uint8_t* data, uint16_t length);

// SLIP timeout (slip_mode.cpp): the last time anything went either way between WiRSa and the Palm (bytes from the Palm, or frames sent to it from the WiFi side), so a Palm waiting quietly on a slow
// server is not cut off. millis().
extern unsigned long slipLastActivityMs;

// Transmit accounting for the SLIP stats line: microseconds inside slipSendFrame (the serial write blocks for the time the bytes take on the line) and the bytes it put out, escapes included.
extern uint32_t slipTxMicros;
extern uint32_t slipTxBytes;

// Pacing of what is sent to the Palm. Its serial driver gets no flow control from WiRSa, and at the higher rates it cannot always empty its buffer as fast as a frame arrives: it drops bytes and the
// TCP segment is lost. slipSendFrame therefore sends in small chunks spaced to a target rate that starts at the line rate and adapts: every retransmit (a segment the Palm did not acknowledge) slows it,
// a run of acknowledged data speeds it up. Targets are in bits per second on the wire.
void slipPaceInit(uint32_t lineBaud);   // a session starts: full line rate, floor at a fifth of it
void slipPaceBackoff();                  // the Palm lost something: slow down
void slipPaceAcked(uint32_t bytes);      // the Palm confirmed this many data bytes: speed up after a clean run
uint32_t slipPaceRate();                 // the current target, bits per second
uint32_t slipPaceBackoffs();             // how many times it slowed down this session

// Reset the receiver (after an error or timeout)
void slipReset(SlipContext* ctx);

inline uint8_t* slipGetRxBuffer(SlipContext* ctx) {
    return ctx->rxBuffer;
}

#endif // SLIP_H
