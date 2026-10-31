// Arduino.h - the little of the Arduino core that the host tests need: a clock that only moves when the code waits, a String, and a serial port made of two queues.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <deque>
#include <vector>
#include <functional>

extern unsigned long gFakeNow;
inline unsigned long millis() { return gFakeNow; }
inline void delay(unsigned long ms) { gFakeNow += ms; }
inline void yield() { gFakeNow += 1; }

class String {
  std::string s;
public:
  String() {}
  String(const char *p) : s(p) {}
  unsigned length() const { return (unsigned) s.size(); }
  const char *c_str() const { return s.c_str(); }
  void reserve(unsigned n) { s.reserve(n); }
  String &operator+=(char c) { s += c; return *this; }
};

// rx: bytes the firmware will read. tx: bytes it wrote. onFlush runs at each flush(), the moment a frame is complete on the wire: a test answers from there.
class HardwareSerial {
public:
  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
  std::function<void(HardwareSerial &)> onFlush;
  int available() { return (int) rx.size(); }
  int read() { if (rx.empty()) return -1; int b = rx.front(); rx.pop_front(); return b; }
  size_t write(uint8_t b) { tx.push_back(b); return 1; }
  size_t write(const uint8_t *p, size_t n) { tx.insert(tx.end(), p, p + n); return n; }
  void flush() { if (onFlush) onFlush(*this); }
};
