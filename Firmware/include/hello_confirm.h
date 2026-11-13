// hello_confirm.h - the confirmed baud switch of RelayMode::handleHello, as a decision with no hardware in it (host test: test_host/hello_confirm_test.cpp).
//
// Palm -> WiRSa  HELLO  baud u32 [, flags u8]     flags: OFFER on the first HELLO, CONFIRM on the second
// WiRSa -> Palm  HELLO_ACK  version, caps, server, baud     caps CAP_CONFIRM: "I wait for your CONFIRM, the port has not moved"
//
// With OFFER the reply goes out at the idle rate and the port stays there; the Palm reads the reply, sends CONFIRM (still at the idle rate) and moves.
// WiRSa moves when it gets the CONFIRM. A reply the Palm could not read leaves both at the idle rate, and its next HELLO is answered again. Without
// OFFER (an older Palm, or a mode that does not use this) the reply goes out and the port moves at once, as it always did.
#ifndef HELLO_CONFIRM_H
#define HELLO_CONFIRM_H

#include <stdint.h>
#include <stddef.h>

namespace RelayMode {

constexpr uint8_t HELLO_OFFER = 0x01;      // Palm -> WiRSa: I will confirm before I switch
constexpr uint8_t HELLO_CONFIRM = 0x02;    // Palm -> WiRSa: I have the reply, switch now (the same rate again)
constexpr uint8_t CAP_HELLO_CONFIRM = 0x02;   // in the reply's capability byte
constexpr uint32_t HELLO_PENDING_MS = 5000;   // an offer left unconfirmed this long is forgotten

struct HelloPending {
  uint32_t baud = 0;          // the rate offered and answered, 0 = none open
  unsigned long at = 0;       // millis() of the reply
};

enum class HelloAct {
  ReplyAndSwitch,   // answer, then move the port (the way it always was)
  ReplyAndWait,     // answer with CAP_HELLO_CONFIRM, port stays; the offer is open (pending set)
  Switch,           // the Palm's confirm for the open offer: move the port now (pending cleared)
  Ignore,           // a confirm with no open offer: nothing happens (pending cleared)
};

// enabled: the mode asked for the confirmed switch and its reply has the capability byte (Spec::helloConfirm, helloServer).
// body/len: the HELLO as it arrived (the rate is its first four bytes); neg: the rate agreed for it.
inline HelloAct helloDecide(bool enabled, const uint8_t *body, size_t len, uint32_t neg, unsigned long now, HelloPending &p) {
  uint8_t flags = (enabled && len > 4) ? body[4] : 0;
  if (flags & HELLO_CONFIRM) {
    bool open = p.baud && p.baud == neg && (unsigned long) (now - p.at) <= HELLO_PENDING_MS;
    p.baud = 0;
    return open ? HelloAct::Switch : HelloAct::Ignore;
  }
  if (flags & HELLO_OFFER) {
    p.baud = neg;
    p.at = now;
    return HelloAct::ReplyAndWait;
  }
  p.baud = 0;
  return HelloAct::ReplyAndSwitch;
}

}  // namespace RelayMode

#endif
