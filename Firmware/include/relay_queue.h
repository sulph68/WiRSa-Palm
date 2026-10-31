#ifndef RELAY_QUEUE_H
#define RELAY_QUEUE_H

// Download-ahead queue for the relay modes (used by caldav_relay.cpp).
//
// A sync runs as a producer (the sync task: network, parsing, frame building)
// and a consumer (the thread that started it, the only one that touches the
// Palm line while the stream runs). The producer's frames wait in a FIFO in
// PSRAM; the consumer sends each with the usual ack wait. The network side
// never waits for the Palm. Frames are transient: zeroed when acked or
// discarded, never written to flash. Without PSRAM begin() returns false and
// send()/emitFinal() fall straight through to the Palm line (the old path).
//
// Order of use: the consumer thread calls begin(), starts the producer task,
// then consume() (returns when the producer calls end()). The producer uses
// send() for acked frames and emitFinal() for the closing frames.

#include <Arduino.h>

namespace RelayQueue {

struct Config {
  uint8_t ackType, byeType;
  uint8_t *ackScratch; size_t ackCap;
  int retries; uint32_t retryDelayMs, ackTimeoutMs, byteTimeoutMs;
  uint8_t retryRoundBefore;   // final frame type (SYNC_DONE) that first gets a second round for unacked frames; 0 = none
};

bool begin(const Config &cfg);   // true = queue active (PSRAM present)
void disable();                  // producer runs inline on the consumer's thread: no queue
void end();                      // producer: finished, wake the consumer
void consume();                  // consumer: send queued frames until end()
bool active();
struct Stats { uint32_t acked, lost; };   // frames the Palm acked / frames it never got (skipped or discarded); finals excluded
Stats stats();                   // valid once consume() has returned
bool cancelled();                // the Palm stopped acking during the stream

// Producer side. send() has writeFrameAcked's contract: false = the Palm is gone.
// label: short text for the log (an event title), never sent.
// A frame the Palm doesn't ack after cfg.retries tries is kept and the next
// ones go out; before the closing frame (cfg.retryRoundBefore) the kept ones
// get one more round. Two unacked frames in a row mean the Palm is gone.
bool send(uint8_t type, const uint8_t *body, size_t len, const char *label = nullptr);
// Closing frames (SYNC_DONE, BYE, ERR): queued behind the items once
// streaming has begun, otherwise written directly. Never acked.
void emitFinal(uint8_t type, const uint8_t *body, size_t len);

}  // namespace RelayQueue

#endif  // RELAY_QUEUE_H
