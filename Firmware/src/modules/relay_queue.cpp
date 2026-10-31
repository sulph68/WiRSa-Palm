// relay_queue.cpp - see relay_queue.h.
#include "log.h"
#include "relay_queue.h"
#include "relay_proto.h"
#include <esp_heap_caps.h>

extern HardwareSerial PhysicalSerial;
extern bool usbDebug;

namespace RelayQueue {
namespace {

const size_t kMaxBytes = 4UL * 1024 * 1024;   // producer waits above this

struct Node {
  Node *next;
  uint16_t len;
  uint16_t seq;
  uint8_t type;
  bool final;
  char label[32];                  // log only
  uint8_t *data() { return (uint8_t *) (this + 1); }
};

struct {
  Config cfg;
  bool active = false;
  volatile bool started = false;   // first frame queued: the consumer owns the Palm line from here
  volatile bool cancel = false;    // consumer lost the Palm: the producer stops
  volatile bool done = false;
  uint32_t acked = 0, lost = 0;     // consumer's tally for stats()
  Node *head = nullptr, *tail = nullptr;
  size_t bytes = 0;
  unsigned depth = 0;
  uint16_t pushed = 0;
  SemaphoreHandle_t mutex = nullptr;
  SemaphoreHandle_t avail = nullptr;   // one count per queued node, plus one when the producer is done
} Q;

void freeNode(Node *n) {
  if (!n) return;
  memset(n, 0, sizeof(Node) + n->len);   // don't leave synced data in PSRAM
  heap_caps_free(n);
}

bool push(uint8_t type, const uint8_t *body, size_t len, bool final, const char *label = nullptr) {
  size_t need = sizeof(Node) + len;
  Node *n = nullptr;
  for (;;) {
    if (Q.cancel && !final) return false;
    if (Q.bytes + need <= kMaxBytes || Q.depth == 0) {
      n = (Node *) heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (n) break;
      if (Q.depth == 0) return false;       // nothing to wait for: out of memory
    }
    vTaskDelay(2);                          // the consumer is draining: wait for room
  }
  n->next = nullptr; n->len = (uint16_t) len; n->type = type; n->final = final;
  n->label[0] = 0;
  if (label) { strncpy(n->label, label, sizeof(n->label) - 1); n->label[sizeof(n->label) - 1] = 0; }
  if (len) memcpy(n->data(), body, len);
  xSemaphoreTake(Q.mutex, portMAX_DELAY);
  n->seq = ++Q.pushed;
  if (Q.tail) Q.tail->next = n; else Q.head = n;
  Q.tail = n;
  Q.bytes += need; Q.depth++;
  Q.started = true;
  xSemaphoreGive(Q.mutex);
  xSemaphoreGive(Q.avail);
  return true;
}

Node *pop() {
  xSemaphoreTake(Q.mutex, portMAX_DELAY);
  Node *n = Q.head;
  if (n) {
    Q.head = n->next;
    if (!Q.head) Q.tail = nullptr;
    Q.bytes -= sizeof(Node) + n->len; Q.depth--;
  }
  xSemaphoreGive(Q.mutex);
  return n;
}

}  // namespace

bool begin(const Config &cfg) {
  Q.cfg = cfg;
  Q.active = psramFound();
  if (!Q.active) return false;
  if (!Q.mutex) { Q.mutex = xSemaphoreCreateMutex(); Q.avail = xSemaphoreCreateCounting(65535, 0); }
  while (xSemaphoreTake(Q.avail, 0) == pdTRUE) {}
  Q.started = Q.cancel = Q.done = false;
  Q.acked = Q.lost = 0;
  Q.head = Q.tail = nullptr;
  Q.bytes = 0; Q.depth = 0; Q.pushed = 0;
  return true;
}

void disable() { Q.active = false; }
bool active() { return Q.active; }
bool cancelled() { return Q.active && Q.cancel; }
Stats stats() { return Stats{Q.acked, Q.lost}; }

void end() {
  if (!Q.active) return;
  Q.done = true;
  xSemaphoreGive(Q.avail);
}

void consume() {
  if (!Q.active) return;
  Node *fHead = nullptr, *fTail = nullptr;      // not acked in the first pass
  unsigned failedNow = 0;
  int consecutive = 0;

  // 1 = acked, 0 = no ack after the configured tries, -1 = the Palm sent BYE
  auto sendOne = [&](Node *n) -> int {
    bool bye = false;
    if (RelayProto::writeFrameAcked(PhysicalSerial, n->type, n->data(), n->len,
                                    Q.cfg.ackType, Q.cfg.byeType, Q.cfg.ackScratch, Q.cfg.ackCap,
                                    Q.cfg.retries, Q.cfg.retryDelayMs, Q.cfg.ackTimeoutMs,
                                    Q.cfg.byteTimeoutMs, &bye)) return 1;
    return bye ? -1 : 0;
  };
  auto dropFailed = [&]() {
    for (Node *f = fHead, *nx; f; f = nx) { nx = f->next; Q.lost++; freeNode(f); }
    fHead = fTail = nullptr; failedNow = 0;
  };

  for (;;) {
    xSemaphoreTake(Q.avail, portMAX_DELAY);
    Node *n = pop();
    if (!n) { if (Q.done) break; continue; }     // the producer's "done" token

    if (n->final) {
      if (n->type == Q.cfg.retryRoundBefore && !Q.cancel && fHead) {
        int bad = 0;                              // second round for what the Palm missed
        for (Node *f = fHead, *nx; f; f = nx) {
          nx = f->next;
          int r = Q.cancel ? -1 : (bad >= 2 ? 0 : sendOne(f));
          if (r == 1) {
            bad = 0;
            Q.acked++;
            LOGV("RelayQueue: retry OK  #%u '%s' (0x%02X, %uB)\r\n", f->seq, f->label, f->type, f->len);
          } else {
            if (r < 0) Q.cancel = true; else bad++;
            Q.lost++;
            LOGV("RelayQueue: SKIPPED   #%u '%s' (0x%02X, %uB) - the Palm never acked it\r\n", f->seq, f->label, f->type, f->len);
          }
          freeNode(f);
        }
        fHead = fTail = nullptr; failedNow = 0;
      }
      RelayProto::writeFrame(PhysicalSerial, n->type, n->data(), n->len);
      freeNode(n);
      continue;
    }

    if (Q.cancel) { Q.lost++; freeNode(n); continue; }      // after a failure the rest is only discarded
    int r = sendOne(n);
    if (r == 1) { consecutive = 0; Q.acked++; freeNode(n); continue; }
    if (r < 0) {
      Q.cancel = true;
      LOGW("RelayQueue: the Palm sent BYE at #%u '%s' - discarding the rest\r\n", n->seq, n->label);
      Q.lost++; freeNode(n); dropFailed(); continue;
    }
    // No ack: keep the frame, carry on with the next one.
    failedNow++;
    LOGW("RelayQueue: FAILED #%u '%s' (0x%02X, %uB) after %d tries - deferred (%u waiting)\r\n",
                                n->seq, n->label, n->type, n->len, Q.cfg.retries, failedNow);
    n->next = nullptr;
    if (fTail) fTail->next = n; else fHead = n;
    fTail = n;
    if (++consecutive >= 2) {                      // two in a row: the Palm is gone, not one bad frame
      LOGW("RelayQueue: two frames in a row not acked - giving up on the Palm");
      Q.cancel = true;
      dropFailed();
    }
  }
  dropFailed();
  for (Node *n; (n = pop()) != nullptr; ) { if (!n->final) Q.lost++; freeNode(n); }   // nothing should be left; zero it if so
}

bool send(uint8_t type, const uint8_t *body, size_t len, const char *label) {
  if (!Q.active) {
    bool bye = false;
    return RelayProto::writeFrameAcked(PhysicalSerial, type, body, len,
                                       Q.cfg.ackType, Q.cfg.byeType, Q.cfg.ackScratch, Q.cfg.ackCap,
                                       Q.cfg.retries, Q.cfg.retryDelayMs, Q.cfg.ackTimeoutMs,
                                       Q.cfg.byteTimeoutMs, &bye);
  }
  return push(type, body, len, false, label);
}

void emitFinal(uint8_t type, const uint8_t *body, size_t len) {
  if (Q.active && Q.started) push(type, body, len, true);
  else RelayProto::writeFrame(PhysicalSerial, type, body, len);
}

}  // namespace RelayQueue
