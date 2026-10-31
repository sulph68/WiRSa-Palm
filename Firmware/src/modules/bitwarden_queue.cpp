// bitwarden_queue.cpp - the download-ahead queue: frames made by the fetch, sent as the Palm takes them. Part of the BitWarden Relay mode; include/bitwarden_internal.h says how the files divide the work.
#include "bitwarden_internal.h"

namespace BitWardenRelay {

// ---- Download-ahead queue (WIRSA_BW_QUEUE) ----
// The sync is a producer (the bwSync task: TLS fetch, JSON scan, decrypt, frame build) and a consumer (the thread that started the sync, the only one touching the Palm line during the stream). Built
// FOLDER/ORG/SYNC_ITEM frames go into a FIFO in PSRAM and the consumer sends each with the usual ack wait. So the network side never waits for the Palm, and a Palm failure cannot trigger a refetch.
// Transient by design: nodes live in PSRAM only (never flash), each is zeroed when acked (or discarded), and all is freed before the sync ends. Without PSRAM (or WIRSA_BW_QUEUE 0) the old synchronous
// path is used unchanged: bwStreamSend/bwEmitFinal then just call bwWriteFrameAcked/bwWriteFrame.
#ifndef WIRSA_BW_QUEUE
#define WIRSA_BW_QUEUE 1
#endif

#if WIRSA_BW_QUEUE
#include <esp_heap_caps.h>

static const size_t kBwQMaxBytes = 2UL * 1024 * 1024;   // producer waits above this


// A frame the Palm did not ack gets this many tries, then waits in a failed list while the rest go out; the failed ones get one more round at the end. A frame bigger than BW_BIG_FRAME waits until
// BW_SETTLE_MS after the previous ack, so the Palm is back in its receive loop before ~1.5 KB arrive (its serial buffer is small, and it saves and housekeeps after each ack).
#ifndef BW_TRIES
#define BW_TRIES 3
#endif
#ifndef BW_BIG_FRAME
#define BW_BIG_FRAME 900
#endif
#ifndef BW_SETTLE_MS
#define BW_SETTLE_MS 300
#endif


BwQueueState gQ;

void bwQFree(BwQNode *n) {
  if (!n) return;
  memset(n, 0, sizeof(BwQNode) + n->len);   // vault data: don't leave it in PSRAM
  heap_caps_free(n);
}

void bwQReset() {
  if (!gQ.mutex) { gQ.mutex = xSemaphoreCreateMutex(); gQ.avail = xSemaphoreCreateCounting(65535, 0); }
  while (xSemaphoreTake(gQ.avail, 0) == pdTRUE) {}
  gQ.started = gQ.cancel = gQ.palmBye = gQ.done = false;
  gQ.head = gQ.tail = nullptr;
  gQ.bytes = 0; gQ.depth = 0; gQ.pushed = 0;
}

bool bwQPush(uint8_t type, const uint8_t *body, size_t len, bool final, const char *label) {
  size_t need = sizeof(BwQNode) + len;
  BwQNode *n = nullptr;
  for (;;) {
    if (gQ.cancel && !final) return false;
    bool roomy = gQ.bytes + need <= kBwQMaxBytes || gQ.depth == 0;
    if (roomy) {
      n = (BwQNode *) heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (n) break;
      if (gQ.depth == 0) return false;      // nothing to wait for: out of memory
    }
    vTaskDelay(2);                          // consumer is draining; wait for room
  }
  n->next = nullptr; n->len = (uint16_t) len; n->type = type; n->final = final;
  n->label[0] = 0;
  if (label) { strncpy(n->label, label, sizeof(n->label) - 1); n->label[sizeof(n->label) - 1] = 0; }
  if (len) memcpy(n->data(), body, len);
  xSemaphoreTake(gQ.mutex, portMAX_DELAY);
  n->seq = ++gQ.pushed;
  if (gQ.tail) gQ.tail->next = n; else gQ.head = n;
  gQ.tail = n;
  gQ.bytes += need; gQ.depth++;
  gQ.started = true;
  xSemaphoreGive(gQ.mutex);
  xSemaphoreGive(gQ.avail);
  return true;
}

BwQNode *bwQPop() {
  xSemaphoreTake(gQ.mutex, portMAX_DELAY);
  BwQNode *n = gQ.head;
  if (n) {
    gQ.head = n->next;
    if (!gQ.head) gQ.tail = nullptr;
    gQ.bytes -= sizeof(BwQNode) + n->len; gQ.depth--;
  }
  xSemaphoreGive(gQ.mutex);
  return n;
}

// Consumer: runs in the thread that started the sync, until the producer is done.
void bwQConsume() {
  BwQNode *fHead = nullptr, *fTail = nullptr;    // not acked in the first pass
  unsigned failedNow = 0;
  uint32_t lastAckAt = 0;
  int consecutive = 0;

  // 1 = acked, 0 = no ack after `tries`, -1 = the Palm quit
  auto sendOne = [&](BwQNode *n, int tries) -> int {
    if (n->len >= BW_BIG_FRAME && lastAckAt) {
      uint32_t since = millis() - lastAckAt;
      if (since < BW_SETTLE_MS) delay(BW_SETTLE_MS - since);
    }
    bool bye = false;
    if (bwWriteFrameAcked(n->type, n->data(), n->len, &bye, tries)) { lastAckAt = millis(); return 1; }
    return bye ? -1 : 0;
  };
  auto dropFailed = [&]() {
    for (BwQNode *f = fHead, *nx; f; f = nx) { nx = f->next; bwQFree(f); }
    fHead = fTail = nullptr; failedNow = 0;
  };

  for (;;) {
    xSemaphoreTake(gQ.avail, portMAX_DELAY);
    BwQNode *n = bwQPop();
    if (!n) { if (gQ.done) break; continue; }     // the producer's "done" token

    if (n->final) {
      if (n->type == BW_SYNC_DONE && !gQ.cancel && fHead) {
        // Second round for what the Palm missed.
        unsigned skippedItems = 0, skipped = 0;
        int bad = 0;
        for (BwQNode *f = fHead, *nx; f; f = nx) {
          nx = f->next;
          int r = gQ.cancel ? -1 : (bad >= 2 ? 0 : sendOne(f, BW_TRIES));
          if (r == 1) {
            bad = 0;
            LOGV("BW: retry OK  #%u '%s' (0x%02X, %uB)\r\n", f->seq, f->label, f->type, f->len);
          } else {
            if (r < 0) { gQ.palmBye = true; gQ.cancel = true; }
            else bad++;
            skipped++;
            if (f->type == BW_SYNC_ITEM) skippedItems++;
            LOGV("BW: SKIPPED   #%u '%s' (0x%02X, %uB) - the Palm never acked it\r\n", f->seq, f->label, f->type, f->len);
          }
          bwQFree(f);
        }
        fHead = fTail = nullptr; failedNow = 0;
        if (skipped) {
          // Report the real item count and say so on the Palm's status line.
          gBwSkippedItems += skippedItems;
          if (skippedItems && n->len >= 2) {
            uint16_t c = (uint16_t) ((n->data()[0] << 8) | n->data()[1]);
            c = c > skippedItems ? (uint16_t) (c - skippedItems) : 0;
            n->data()[0] = (uint8_t) (c >> 8); n->data()[1] = (uint8_t) (c & 0xFF);
          }
          if (!gQ.cancel) {
            char msg[40];
            snprintf(msg, sizeof(msg), "%u item(s) skipped", skipped);
            uint8_t sb[48];
            BwWriter w(sb, sizeof(sb));
            w.str(String(msg));
            bwWriteFrame(BW_SYNC_STATUS, sb, w.len);
          }
        }
      }
      bwWriteFrame(n->type, n->data(), n->len);
      bwQFree(n);
      continue;
    }

    if (gQ.cancel) { bwQFree(n); continue; }       // after a failure the rest is only discarded
    int r = sendOne(n, BW_TRIES);
    if (r == 1) { consecutive = 0; bwQFree(n); continue; }
    if (r < 0) { gQ.palmBye = true; gQ.cancel = true; bwQFree(n); dropFailed(); continue; }
    // No ack: keep the frame, carry on with the next one.
    failedNow++;
    LOGW("BW: item FAILED #%u '%s' (0x%02X, %uB) after %d tries - deferred (%u waiting)\r\n",
                                n->seq, n->label, n->type, n->len, BW_TRIES, failedNow);
    n->next = nullptr;
    if (fTail) fTail->next = n; else fHead = n;
    fTail = n;
    if (++consecutive >= 2) {                       // two in a row: the Palm is gone, not one bad frame
      LOGW("BW: two frames in a row not acked - giving up on the Palm");
      gQ.cancel = true;
      dropFailed();
    }
  }
  dropFailed();
  for (BwQNode *n; (n = bwQPop()) != nullptr; ) bwQFree(n);   // nothing should be left; zero it if so
}

// Producer side, the contract of bwWriteFrameAcked: false = the Palm is gone (palmSaidBye says whether it quit).
bool bwStreamSend(uint8_t type, const uint8_t *body, size_t len, bool *palmSaidBye,
                         const char *label) {
  if (!gQ.active) return bwWriteFrameAcked(type, body, len, palmSaidBye);
  if (palmSaidBye) *palmSaidBye = false;
  if (!bwQPush(type, body, len, false, label)) { if (palmSaidBye) *palmSaidBye = gQ.palmBye; return false; }
  return true;
}

// SYNC_DONE / ERR: queued behind the items once streaming has begun.
void bwEmitFinal(uint8_t type, const uint8_t *body, size_t len) {
  if (gQ.active && gQ.started) bwQPush(type, body, len, true);
  else bwWriteFrame(type, body, len);
}
#else
bool bwStreamSend(uint8_t type, const uint8_t *body, size_t len, bool *palmSaidBye,
                         const char *label) {
  (void) label;
  return bwWriteFrameAcked(type, body, len, palmSaidBye);
}
void bwEmitFinal(uint8_t type, const uint8_t *body, size_t len) { bwWriteFrame(type, body, len); }
#endif

}  // namespace BitWardenRelay
