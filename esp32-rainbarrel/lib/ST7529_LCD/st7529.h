#ifndef ST7529_H
#define ST7529_H

#include <Adafruit_GFX.h>

#define ST7529_BLACK 0xFF
#define ST7529_WHITE 0x00

class ST7529_LCD : public Adafruit_GFX {
public:
    ST7529_LCD(uint16_t w = 240, uint16_t h = 128, int8_t rst_pin = 2);
    ~ST7529_LCD(void);

    bool begin(void); // must call this to init
    void display(void);
    void clearDisplay(void);
    void invertDisplay(bool i); // override
    void setContrast(uint8_t contrastlevel);
    void drawPixel(int16_t x, int16_t y, uint16_t color); // override
    uint16_t getPixel(int16_t x, int16_t y);
    uint8_t *getBuffer(void);
 protected:
    bool _init(void);
    void lcdWrite( uint8_t type, uint8_t data );
    uint8_t *buffer = NULL; // Internal 1:1 framebuffer of display mem
  int16_t window_x1, ///< Dirty tracking window minimum x
      window_y1,     ///< Dirty tracking window minimum y
      window_x2,     ///< Dirty tracking window maximum x
      window_y2;     ///< Dirty tracking window maximum y
public:
  int rstPin; // The arduino pin connected to reset (-1 if unused)
  int ext_mode = -1; // 0 if EXT = 0, 1 if EXT = 1, -1 if unknown (initially)
};

#endif /* !ST7529_H */
