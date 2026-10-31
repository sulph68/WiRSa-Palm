#ifndef NULL_DISPLAY_H
#define NULL_DISPLAY_H

// Screenless build (WIRSA_SCREENLESS, globals.h): stands in for the
// Adafruit_SSD1306 object with the same calls the firmware makes, all doing
// nothing, so the screen-drawing code compiles unchanged and the display
// libraries drop out of the link. print()/println() come from Print.

#include <Arduino.h>
#include <Adafruit_GFX.h>   // GFXfont, for setFont()

class NullDisplay : public Print {
public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t *, size_t size) override { return size; }
  using Print::write;

  bool begin(uint8_t = 0, uint8_t = 0, bool = true, bool = true) { return true; }
  void display() {}
  void clearDisplay() {}
  void setCursor(int16_t, int16_t) {}
  void setTextColor(uint16_t) {}
  void setTextColor(uint16_t, uint16_t) {}
  void setTextSize(uint8_t) {}
  void setTextWrap(bool) {}
  void setFont(const GFXfont * = NULL) {}
  void setRotation(uint8_t) {}
  void cp437(bool = true) {}
  void drawPixel(int16_t, int16_t, uint16_t) {}
  void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
};

#endif
