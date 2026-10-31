// SLIP (RFC 1055): simple framing of IP packets over serial

#include "slip.h"
#include "globals.h"
#include "serial_io.h"

extern bool usbDebug;

// ---- initialise the SLIP context ----

void slipInit(SlipContext* ctx) {
    ctx->rxState = SLIP_RX_IDLE;
    ctx->rxPos = 0;
    ctx->txPos = 0;
    ctx->framesReceived = 0;
    ctx->framesSent = 0;
    ctx->rxErrors = 0;
    ctx->txErrors = 0;
    ctx->bytesReceived = 0;
    ctx->bytesSent = 0;
}

// ---- receive a SLIP-encoded byte ----
// One byte at a time from serial input. Returns 0 to keep receiving, or the frame length (> 0) when a frame is complete (data in rxBuffer).

int slipReceiveByte(SlipContext* ctx, uint8_t byte) {
    ctx->bytesReceived++;

    switch (ctx->rxState) {
        case SLIP_RX_IDLE:
            if (byte == SLIP_END) {
                ctx->rxPos = 0;
                ctx->rxState = SLIP_RX_RECEIVING;
            }
            // other bytes while idle are line noise: ignore
            return 0;

        case SLIP_RX_RECEIVING:
            if (byte == SLIP_END) {
                if (ctx->rxPos > 0) {
                    ctx->framesReceived++;
                    int len = ctx->rxPos;
                    ctx->rxState = SLIP_RX_IDLE;
                    // rxPos is not reset: the caller needs the data
                    return len;
                }
                // empty frame (consecutive ENDs): keep receiving
                ctx->rxPos = 0;
                return 0;
            } else if (byte == SLIP_ESC) {
                ctx->rxState = SLIP_RX_ESCAPE;
                return 0;
            } else {
                if (ctx->rxPos < SLIP_BUFFER_SIZE) {
                    ctx->rxBuffer[ctx->rxPos++] = byte;
                } else {
                    // buffer overflow: discard the frame
                    ctx->rxErrors++;
                    ctx->rxState = SLIP_RX_IDLE;
                    ctx->rxPos = 0;
                }
                return 0;
            }

        case SLIP_RX_ESCAPE:
            ctx->rxState = SLIP_RX_RECEIVING;

            if (byte == SLIP_ESC_END) {
                byte = SLIP_END;
            } else if (byte == SLIP_ESC_ESC) {
                byte = SLIP_ESC;
            } else {
                // Invalid escape sequence, a protocol error: RFC 1055 suggests storing the byte anyway, but flag the error
                ctx->rxErrors++;
                // carry on: some implementations have quirks
            }

            if (ctx->rxPos < SLIP_BUFFER_SIZE) {
                ctx->rxBuffer[ctx->rxPos++] = byte;
            } else {
                ctx->rxErrors++;
                ctx->rxState = SLIP_RX_IDLE;
                ctx->rxPos = 0;
            }
            return 0;
    }

    return 0;
}

// ---- pacing ----
static uint32_t sPaceLine = 0, sPaceRate = 0, sPaceMin = 0, sPaceAcked = 0, sPaceBackoffs = 0;
static unsigned long sPaceLastBackoffMs = 0;
static uint32_t sPaceNextUs = 0;

void slipPaceInit(uint32_t lineBaud) {
    sPaceLine = lineBaud;
    sPaceRate = lineBaud;
    // Pacing is only for the fast rates: at 57600 and below the Palm keeps up, and a timeout there is usually a damaged acknowledgement on the Palm's own line, which slowing down does not cure
    // (measured: 40 s unpaced, 60 s or more paced).
    sPaceMin = lineBaud > 57600 ? lineBaud / 5 : lineBaud;
    sPaceAcked = 0;
    sPaceBackoffs = 0;
    sPaceLastBackoffMs = 0;
    sPaceNextUs = micros();
}

void slipPaceBackoff() {
    if (!sPaceLine) return;
    // one step per burst of losses: a window of lost segments is one event, not several
    if (sPaceLastBackoffMs && millis() - sPaceLastBackoffMs < 500) return;
    sPaceLastBackoffMs = millis();
    if (sPaceMin >= sPaceLine) return;        // pacing is off at this line speed
    sPaceBackoffs++;
    sPaceRate = sPaceRate - sPaceRate / 4;
    if (sPaceRate < sPaceMin) sPaceRate = sPaceMin;
    sPaceAcked = 0;
}

void slipPaceAcked(uint32_t bytes) {
    if (!sPaceLine || sPaceRate >= sPaceLine) return;
    sPaceAcked += bytes;
    if (sPaceAcked >= 4096 && millis() - sPaceLastBackoffMs > 2000) {
        sPaceAcked = 0;
        sPaceRate += sPaceRate / 8;
        if (sPaceRate > sPaceLine) sPaceRate = sPaceLine;
    }
}

uint32_t slipPaceRate() { return sPaceRate ? sPaceRate : (uint32_t) PhysicalSerial.baudRate(); }
uint32_t slipPaceBackoffs() { return sPaceBackoffs; }

// Waits until the target rate allows the next n bytes, then books their time.
static void slipPaceChunk(uint32_t n) {
    if (!sPaceRate || sPaceRate >= sPaceLine) { sPaceNextUs = micros(); return; }
    int32_t wait;
    while ((wait = (int32_t) (sPaceNextUs - micros())) > 0) {
        if (wait > 2000) delay(1); else delayMicroseconds(wait > 100 ? 100 : (uint32_t) wait);
    }
    uint32_t now = micros();
    if ((int32_t) (now - sPaceNextUs) > 20000) sPaceNextUs = now;     // idle a while: no credit to burst with
    sPaceNextUs += (uint32_t) ((uint64_t) n * 10ULL * 1000000ULL / sPaceRate);
}

// ---- send a SLIP-encoded frame ----
// Takes a raw IP packet and sends it SLIP-framed to PhysicalSerial, in chunks spaced to the pace.

unsigned long slipLastActivityMs = 0;
uint32_t slipTxMicros = 0;
uint32_t slipTxBytes = 0;

#define SLIP_TX_CHUNK 64

void slipSendFrame(SlipContext* ctx, const uint8_t* data, uint16_t length) {
    slipLastActivityMs = millis();   // traffic to the Palm keeps the SLIP timeout away
    uint32_t txT0 = micros();
    if (logOn(LOG_VERBOSE)) {
        logPrintf(LOG_VERBOSE, "SLIP: Sending frame, %d bytes\r\n", length);
    }

    // Encode first (RFC 1055: END at the start flushes line noise and delimits back-to-back frames), then send in chunks, each when the pace allows.
    static uint8_t enc[2 * SLIP_BUFFER_SIZE + 4];
    uint32_t n = 0;
    enc[n++] = SLIP_END;
    for (uint16_t i = 0; i < length; i++) {
        switch (data[i]) {
            case SLIP_END: enc[n++] = SLIP_ESC; enc[n++] = SLIP_ESC_END; break;
            case SLIP_ESC: enc[n++] = SLIP_ESC; enc[n++] = SLIP_ESC_ESC; break;
            default:       enc[n++] = data[i]; break;
        }
    }
    enc[n++] = SLIP_END;

    for (uint32_t off = 0; off < n; ) {
        uint32_t c = n - off;
        if (c > SLIP_TX_CHUNK) c = SLIP_TX_CHUNK;
        slipPaceChunk(c);
        PhysicalSerial.write(enc + off, c);
        off += c;
    }
    ctx->bytesSent += n;
    ctx->framesSent++;
    slipTxMicros += micros() - txT0;
    slipTxBytes += n;
}

// ---- reset the SLIP receiver (after a timeout or error, to resync with incoming data) ----

void slipReset(SlipContext* ctx) {
    ctx->rxState = SLIP_RX_IDLE;
    ctx->rxPos = 0;
}
