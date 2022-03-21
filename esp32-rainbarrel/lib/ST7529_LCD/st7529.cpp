#include "st7529.h"
#include <Adafruit_GFX.h>
#include <driver/spi.h> // At the moment this is incompatible with standard SPI

#define st7529_swap(a, b)                                                    \
  (((a) ^= (b)), ((b) ^= (a)), ((a) ^= (b))) ///< No-temp-var swap operation

#define COMMAND0 0
#define COMMAND1 1
#define DATA     2

ST7529_LCD::ST7529_LCD(uint16_t w, uint16_t h, int8_t rst_pin)
    : Adafruit_GFX(w, h), rstPin(rst_pin) {
}

ST7529_LCD::~ST7529_LCD(void) {
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
}

bool ST7529_LCD::begin(void) {
    if (!_init()) {
        return false;
    }
    // set contrast
    // set rotation
    setRotation(2);
    // display splash screen
    return true; // success
}

bool ST7529_LCD::_init(void) {
    // attempt to malloc the bitmap framebuffer
    if ((!buffer) &&
        !(buffer = (uint8_t *)malloc(WIDTH * HEIGHT))) {
        return false;
    }
    // Set up pins
    delay(1); // XXX needed?
    if (rstPin >= 0) {
        digitalWrite(rstPin, 0);
        pinMode(rstPin, OUTPUT);
    }
    spi_init(HSPI); // 4 MHz
    spi_mode(HSPI, 1, 1);
    if (rstPin >= 0) {
        delayMicroseconds(1);
        digitalWrite(rstPin, 1);
        delayMicroseconds(2);
    }
    // Initialize hardware
    ext_mode = 0;
    lcdWrite( COMMAND0, 0x0025 ); // NOP
    lcdWrite( COMMAND0, 0x0025 ); // NOP
    ext_mode = -1; // explicitly set Ext = 0
    lcdWrite( COMMAND0, 0x0094 ); // Exit sleep mode (SLPOUT)
    lcdWrite( COMMAND0, 0x00D1 ); // Internal Oscillator On (OSCON)
    lcdWrite( COMMAND0, 0x0020 ); // Power control set (PWRCTRL)
    lcdWrite( DATA, 0x0008 ); // (Booster on, follower and reference off)
    delay( 5 ); // Booster must be on first before other power enabled
    lcdWrite( COMMAND0, 0x0020 ); // Power control set (PWRCTRL)
    lcdWrite( DATA, 0x000B ); // (booster, follower & reference on)
    delay( 5 ); // Booster must be on first before other power enabled

    // "write contrast"
    lcdWrite( COMMAND0, 0x0081 ); // Program optimum LCD supply voltage (VOLCTRL)
    lcdWrite( DATA, 0x0010 ); // VPR = 0b1 0001 0000 = 0x110 => 14.48V
    lcdWrite( DATA, 0x0004 ); // (Reset state is 0x101 => Vop = 13.88V)

    lcdWrite( COMMAND0, 0x00CA ); // Display control (DISCTRL)
    lcdWrite( DATA, 0x0000 ); // Clock divider = X1
    lcdWrite( DATA, 0x0023 ); // Duty = 144
    lcdWrite( DATA, 0x0000 ); // Frame=1 line cycle; FR Inverse-Set Value = 0
    lcdWrite( COMMAND0, 0x00A6 ); // Normal Display (DISNOR)
    lcdWrite( COMMAND0, 0x00BB ); // Common scan (COMSCN)
    lcdWrite( DATA, 0x0002 );    // 79->0  80->159 (actually 63->0 80->143)
    lcdWrite( COMMAND0, 0x00BC ); // Data scan direction (DATSDR)
    lcdWrite( DATA, 0x0000 ); //Address-scan= column, Column=normal, line=normal
    lcdWrite( DATA, 0x0000 ); // RGB arrangement (not BGR)
    lcdWrite( DATA, 0x0002 ); // 32 greyscale 3Byte 3Pixel mode
    lcdWrite( COMMAND0, 0x0075 ); // Line address set (LASET)
    lcdWrite( DATA, 0x0000 ); // Start Line = 0
    lcdWrite( DATA, 0x0077 ); // End Line = 119  (120 rows - 1)
    lcdWrite( COMMAND0, 0x0015 ); // Column address set (CASET)
    lcdWrite( DATA, 0x0000 ); // Start Column = 0
    lcdWrite( DATA, 0x004F ); // End Column = 79 (240 columns / 3 - 1)
    lcdWrite( COMMAND1, 0x0032 ); // Analog Circuit Set (ANASET)
    lcdWrite( DATA, 0x0000 ); // OSC Frequency = 000 (default, 12.7kHz)
    lcdWrite( DATA, 0x0000 ); // Booster Efficiency = 00 (3kHz)
    lcdWrite( DATA, 0x0002 ); // Bias = 1/12
    lcdWrite( COMMAND1, 0x0034 ); // Software Initial (SWINT)

    // Reset scroll
    lcdWrite( COMMAND0, 0x00AB );
    lcdWrite( DATA, 36 ); // Initial scroll position
    // Invert display
    lcdWrite( COMMAND0, 0x00A7 );
    // Display on
    lcdWrite( COMMAND0, 0x00AF ); // Display On (DISON)

    // Clear framebuffer (but not display memory)
    clearDisplay();
    // set max dirty window
    window_x1 = 0;
    window_y1 = 0;
    window_x2 = WIDTH - 1;
    window_y2 = HEIGHT - 1;

    return true; // Success
}
// DRAWING FUNCTIONS -------------------------------------------------------

/*!
    @brief  Set/clear/invert a single pixel. This is also invoked by the
            Adafruit_GFX library in generating many higher-level graphics
            primitives.
    @param  x
            Column of display -- 0 at left to (screen width - 1) at right.
    @param  y
            Row of display -- 0 at top to (screen height -1) at bottom.
    @param  color
            Pixel color: from ST7529_WHITE (0) to ST7529_BLACK (0xFF)
            (but only top 5 bits are rendered)
    @note   Changes buffer contents only, no immediate effect on display.
            Follow up with a call to display(), or with other graphics
            commands as needed by one's own application.
*/
void ST7529_LCD::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if ((x >= 0) && (x < width()) && (y >= 0) && (y < height())) {
        // Pixel is in-bounds. Rotate coordinates if needed.
        switch (getRotation()) {
        case 1:
            st7529_swap(x, y);
            x = WIDTH - x - 1;
            break;
        case 2:
            x = WIDTH - x - 1;
            y = HEIGHT - y - 1;
            break;
        case 3:
            st7529_swap(x, y);
            y = HEIGHT - y - 1;
            break;
        }

        // adjust dirty window
        window_x1 = min(window_x1, x);
        window_y1 = min(window_y1, y);
        window_x2 = max(window_x2, x);
        window_y2 = max(window_y2, y);

        buffer[x + y*WIDTH] = (color&0xFF);
    }
}

/*!
    @brief  Clear contents of display buffer (set all pixels to off).
    @note   Changes buffer contents only, no immediate effect on display.
            Follow up with a call to display(), or with other graphics
            commands as needed by one's own application.
*/
void ST7529_LCD::clearDisplay(void) {
    memset(buffer, 0, WIDTH * HEIGHT);
    // set max dirty window
    window_x1 = 0;
    window_y1 = 0;
    window_x2 = WIDTH - 1;
    window_y2 = HEIGHT - 1;
}
/*!
    @brief  Return color of a single pixel in display buffer.
    @param  x
            Column of display -- 0 at left to (screen width - 1) at right.
    @param  y
            Row of display -- 0 at top to (screen height -1) at bottom.
    @return true if pixel is set (usually MONOOLED_WHITE, unless display invert
   mode is enabled), false if clear (MONOOLED_BLACK).
    @note   Reads from buffer contents; may not reflect current contents of
            screen if display() has not been called.
*/
uint16_t ST7529_LCD::getPixel(int16_t x, int16_t y) {
    if ((x >= 0) && (x < width()) && (y >= 0) && (y < height())) {
        // Pixel is in-bounds. Rotate coordinates if needed.
        switch (getRotation()) {
        case 1:
            st7529_swap(x, y);
            x = WIDTH - x - 1;
            break;
        case 2:
            x = WIDTH - x - 1;
            y = HEIGHT - y - 1;
            break;
        case 3:
            st7529_swap(x, y);
            y = HEIGHT - y - 1;
            break;
        }
        return buffer[x + y*WIDTH];
    }
    return 0; // Pixel out of bounds
}

/*!
    @brief  Get base address of display buffer for direct reading or writing.
    @return Pointer to an unsigned 8-bit array, column-major, columns padded
            to full byte boundary if needed.
*/
uint8_t *ST7529_LCD::getBuffer(void) { return buffer; }

// OTHER HARDWARE SETTINGS -------------------------------------------------

/*!
    @brief  Enable or disable display invert mode (white-on-black vs
            black-on-white). Handy for testing!
    @param  i
            If true, switch to invert mode (black-on-white), else normal
            mode (white-on-black).
    @note   This has an immediate effect on the display, no need to call the
            display() function -- buffer contents are not changed, rather a
            different pixel mode of the display hardware is used. When
            enabled, drawing MONOOLED_BLACK (value 0) pixels will actually draw
   white, MONOOLED_WHITE (value 1) will draw black.
*/
void ST7529_LCD::invertDisplay(bool i) {
    lcdWrite( COMMAND0, i ? 0xA6 : 0xA7 );
}

/*!
    @brief  Adjust the display contrast.
    @param  level The contrast level from 0 to 0x7F
    @note   This has an immediate effect on the display, no need to call the
            display() function -- buffer contents are not changed.
*/
void ST7529_LCD::setContrast(uint8_t level) {
    // XXX write me
}

/** Push data currently in RAM to display. */
void ST7529_LCD::display(void) {
    // ESP8266 needs a periodic yield() call to avoid watchdog reset.
    yield();
    // Write entire buffer (XXX speed this up!)
    lcdWrite( COMMAND0, 0x0015 ); // Column address set
    lcdWrite( DATA, 0 );
    lcdWrite( DATA, 79 );
    lcdWrite( COMMAND0, 0x0075 ); // Line address set
    lcdWrite( DATA, 0 ); // From line 0
    lcdWrite( DATA, 127 ); // To line 119
    lcdWrite( COMMAND0, 0x05C );
    int i=0;
    for (int y = 0; y < HEIGHT; y++) {
        ESP.wdtFeed();
        for (int x = 0; x < WIDTH; x+=3) {
            lcdWrite( DATA, buffer[i++] );
            lcdWrite( DATA, buffer[i++] );
            lcdWrite( DATA, buffer[i++] );
        }
    }

    // Reset dirty window
    window_x1 = 1024;
    window_y1 = 1024;
    window_x2 = -1;
    window_y2 = -1;
}

void ST7529_LCD::lcdWrite( uint8_t type, uint8_t data) {
    if (type == COMMAND0 && ext_mode != 0) {
        ext_mode = 0;
        lcdWrite( COMMAND0, 0x0030 ); // EXT = 0
    }
    if (type == COMMAND1 && ext_mode != 1) {
        ext_mode = 1;
        lcdWrite( COMMAND1, 0x0031 ); // EXT = 1
    }
    uint32_t data32 = data;
    if (type == DATA ) {
        data32 |= 0x100;
    }
    spi_txd(HSPI, 9, data32);
}
