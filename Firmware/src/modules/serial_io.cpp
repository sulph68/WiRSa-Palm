#include "serial_io.h"
#include "diagnostics.h"
#include <HardwareSerial.h>

extern HardwareSerial PhysicalSerial;

// Binary mode: when true, text output on PhysicalSerial is suppressed (SLIP and SASD carry binary protocol data on the RS232 port).
bool binaryModeActive = false;

void setBinaryMode(bool active) {
  binaryModeActive = active;
}

static inline bool physicalSerialReady() {
  return !binaryModeActive;
}

// (A "telnet console" was a third output here: consoleClient/consoleConnected, the tcpServer it would have come from, and ConsoleAvailable/ConsoleRead/ConsolePrint/ConsolePrintLn. Removed as dead:
// consoleConnected was only ever set false at its declaration, consoleClient never got a real connection (nothing called tcpServer.accept()/available()/hasClient(), see network.cpp), and the Console*()
// functions had no callers, so every consoleReady()-gated branch was unconditionally dead, confirmed not assumed.)

void SerialPrintLn(String s) {
  Serial.println(s);
  if (physicalSerialReady()) PhysicalSerial.println(s);
}

void SerialPrintLn(char c, int format) {
  Serial.println(c, format);
  if (physicalSerialReady()) PhysicalSerial.println(c, format);
}

void SerialPrintLn(char c) {
  Serial.println(c);
  if (physicalSerialReady()) PhysicalSerial.println(c);
}

void SerialPrint(String s) {
  Serial.print(s);
  if (physicalSerialReady()) PhysicalSerial.print(s);
}

void SerialPrint(char c) {
  Serial.print(c);
  if (physicalSerialReady()) PhysicalSerial.print(c);
}

void SerialPrint(char c, int format) {
  Serial.print(c, format);
  if (physicalSerialReady()) PhysicalSerial.print(c, format);
}

void SerialPrintLn() {
  Serial.println();
  if (physicalSerialReady()) PhysicalSerial.println();
}

void SerialPrintLn(unsigned char n, int base) {
  Serial.println(n, base);
  if (physicalSerialReady()) PhysicalSerial.println(n, base);
}

void SerialPrintLn(int n) {
  Serial.println(n);
  if (physicalSerialReady()) PhysicalSerial.println(n);
}

void SerialPrintLn(int n, int base) {
  Serial.println(n, base);
  if (physicalSerialReady()) PhysicalSerial.println(n, base);
}

void SerialPrintLn(unsigned int n) {
  Serial.println(n);
  if (physicalSerialReady()) PhysicalSerial.println(n);
}

void SerialPrintLn(unsigned int n, int base) {
  Serial.println(n, base);
  if (physicalSerialReady()) PhysicalSerial.println(n, base);
}

void SerialPrintLn(long n) {
  Serial.println(n);
  if (physicalSerialReady()) PhysicalSerial.println(n);
}

void SerialPrintLn(long n, int base) {
  Serial.println(n, base);
  if (physicalSerialReady()) PhysicalSerial.println(n, base);
}

void SerialPrintLn(unsigned long n) {
  Serial.println(n);
  if (physicalSerialReady()) PhysicalSerial.println(n);
}

void SerialPrintLn(unsigned long n, int base) {
  Serial.println(n, base);
  if (physicalSerialReady()) PhysicalSerial.println(n, base);
}

void SerialPrint(unsigned char n, int base) {
  Serial.print(n, base);
  if (physicalSerialReady()) PhysicalSerial.print(n, base);
}

void SerialPrint(int n) {
  Serial.print(n);
  if (physicalSerialReady()) PhysicalSerial.print(n);
}

void SerialPrint(int n, int base) {
  Serial.print(n, base);
  if (physicalSerialReady()) PhysicalSerial.print(n, base);
}

void SerialPrint(unsigned int n) {
  Serial.print(n);
  if (physicalSerialReady()) PhysicalSerial.print(n);
}

void SerialPrint(unsigned int n, int base) {
  Serial.print(n, base);
  if (physicalSerialReady()) PhysicalSerial.print(n, base);
}

void SerialPrint(long n) {
  Serial.print(n);
  if (physicalSerialReady()) PhysicalSerial.print(n);
}

void SerialPrint(long n, int base) {
  Serial.print(n, base);
  if (physicalSerialReady()) PhysicalSerial.print(n, base);
}

void SerialPrint(unsigned long n) {
  Serial.print(n);
  if (physicalSerialReady()) PhysicalSerial.print(n);
}

void SerialPrint(unsigned long n, int base) {
  Serial.print(n, base);
  if (physicalSerialReady()) PhysicalSerial.print(n, base);
}

void SerialFlush() {
  Serial.flush();
  if (physicalSerialReady()) PhysicalSerial.flush();
}

int SerialAvailable() {
  int c = Serial.available();
  if (c == 0)
    c = PhysicalSerial.available();
  return c;
}

int SerialRead() {
  // priority: USB Serial, then Physical Serial
  int c = Serial.available();
  if (c > 0) {
    int byte = Serial.read();
    hexDumpByte('R', (uint8_t)byte);
    return byte;
  }
  c = PhysicalSerial.available();
  if (c > 0) {
    int byte = PhysicalSerial.read();
    hexDumpByte('R', (uint8_t)byte);
    return byte;
  }
  return -1;
}

void SerialWrite(int c) {
  hexDumpByte('T', (uint8_t)c);
  Serial.write(c);
  if (physicalSerialReady()) PhysicalSerial.write(c);
}

String getTimestamp() {
  unsigned long ms = millis();
  unsigned long sec = ms / 1000;
  unsigned long min = sec / 60;
  unsigned long hrs = min / 60;

  sec = sec % 60;
  min = min % 60;
  ms = ms % 1000;

  char timestamp[16];
  sprintf(timestamp, "[%02lu:%02lu:%02lu.%03lu] ", hrs, min, sec, ms);
  return String(timestamp);
}

// USB debug print with a timestamp (USB Serial only)
void UsbDebugPrint(String s) {
  Serial.print(getTimestamp());
  Serial.print(s);
}

// USB debug println with a timestamp (USB Serial only)
void UsbDebugPrintLn(String s) {
  Serial.print(getTimestamp());
  Serial.println(s);
}
