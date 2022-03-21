#ifdef LCDTESTER
#include "Arduino.h"
#include <AsyncDelay.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

#define HARDWARE_SPI

#ifdef HARDWARE_SPI
# include <driver/spi.h> // At the moment this is incompatible with standard SPI
#else
# define LCD_SI 13 /* MOSI */
# define LCD_SCL 14 /* SCK */
#endif

#include "config.h"
#define MDNS_NAME "flowtester" // keep same name as flowtester

#define LED_GPIO 0
#define LED_OFF 1 // active low
#define LED_ON  0 // active low

#define LCD_CS 15 // F3
#define LCD_RST 2 // F0
#define WATER_SENSE 12      // F5 (water present) (also MISO)
#define PUMP_CNTRL 0      // F2 (power tail out)
#define PRESSURE_SW_IN 16 // F1 (pressure switch input)

#define LCD_COLS 240
#define LCD_ROWS 128

uint8_t framebuffer[LCD_COLS * LCD_ROWS] = { 0 };

AsyncDelay updateDelay = AsyncDelay(250, AsyncDelay::MILLIS);
bool blinkWasOn = false;
bool mdns_success;

typedef enum { COMMAND, DATA } write_type_t;

#ifdef HARDWARE_SPI
void lcdWrite( write_type_t type, uint16_t value) {
  uint32_t data = value & 0xFF;
  if (type==DATA) { data |= 0x100; }
  spi_txd(HSPI, 9, data);
}
#else
void lcdWrite( write_type_t type, uint16_t value) {
    digitalWrite(LCD_SCL, 1);
    digitalWrite(LCD_CS, 0);
    digitalWrite(LCD_SI, (type==DATA) ? 1 : 0);
    digitalWrite(LCD_SCL, 0);
    delayMicroseconds(1);
    digitalWrite(LCD_SCL, 1); // Latch at rising edge
    delayMicroseconds(1);
    for (int i=0; i<8; i++) {
        digitalWrite(LCD_SCL, 0);
        digitalWrite(LCD_SI, (value & 0x80) ? 1 : 0); // MSB first
        value = value << 1;
        delayMicroseconds(1);
        digitalWrite(LCD_SCL, 1); // Latch at rising edge
        delayMicroseconds(1);
    }
    digitalWrite(LCD_CS, 1);
}
#endif

void ST7529_ReadEEPROM( void ) {
    lcdWrite( COMMAND, 0x0030 ); // EXT = 0
    lcdWrite( COMMAND, 0x0007 ); // Initial code (1)
    lcdWrite( DATA, 0x0019 );
    lcdWrite( COMMAND, 0x0031 ); // EXT = 1
    lcdWrite( COMMAND, 0x00CD ); // EEPROM ON
    lcdWrite( DATA, 0x0000 ); // Enter "Read Mode"
    delay( 100 ); // Wait for EEPROM operation (100ms)
    lcdWrite( COMMAND, 0x00FD ); // Start EEPROM reading operation
    delay( 100 ); // Wait for EEPROM operation (100ms)
    lcdWrite( COMMAND, 0x00CC ); // Exit EEPROM mode
    lcdWrite( COMMAND, 0x0030 ); // EXT = 0
}

// Initialize 240x120 greyscale display @ 14.5V
void ST7529_Init( void )
{
    digitalWrite(LCD_RST, 0);
    delayMicroseconds(1);
    digitalWrite(LCD_RST, 1);
    delayMicroseconds(2);
    lcdWrite( COMMAND, 0x0025 ); // NOP
    lcdWrite( COMMAND, 0x0025 ); // NOP
    lcdWrite( COMMAND, 0x0030 ); // Set Ext = 0
    lcdWrite( COMMAND, 0x0094 ); // Exit sleep mode (SLPOUT)
    lcdWrite( COMMAND, 0x00D1 ); // Internal Oscillator On (OSCON)
    lcdWrite( COMMAND, 0x0020 ); // Power control set (PWRCTRL)
    lcdWrite( DATA, 0x0008 ); // (Booster on, follower and reference off)
    delay( 5 ); // Booster must be on first before other power enabled
    lcdWrite( COMMAND, 0x0020 ); // Power control set (PWRCTRL)
    lcdWrite( DATA, 0x000B ); // (booster, follower & reference on)
    delay( 5 ); // Booster must be on first before other power enabled

    // "write contrast"
    lcdWrite( COMMAND, 0x0081 ); // Program optimum LCD supply voltage (VOLCTRL)
    lcdWrite( DATA, 0x0010 ); // VPR = 0b1 0001 0000 = 0x110 => 14.48V
    lcdWrite( DATA, 0x0004 ); // (Reset state is 0x101 => Vop = 13.88V)

    lcdWrite( COMMAND, 0x00CA ); // Display control (DISCTRL)
    lcdWrite( DATA, 0x0000 ); // Clock divider = X1
    lcdWrite( DATA, 0x0023 ); // Duty = 144
    lcdWrite( DATA, 0x0000 ); // Frame=1 line cycle; FR Inverse-Set Value = 0
    lcdWrite( COMMAND, 0x00A6 ); // Normal Display (DISNOR)
    lcdWrite( COMMAND, 0x00BB ); // Common scan (COMSCN)
    lcdWrite( DATA, 0x0002 );    // 79->0  80->159 (actually 63->0 80->143)
    lcdWrite( COMMAND, 0x00BC ); // Data scan direction (DATSDR)
    lcdWrite( DATA, 0x0000 ); //Address-scan= column, Column=normal, line=normal
    lcdWrite( DATA, 0x0000 ); // RGB arrangement (not BGR)
    lcdWrite( DATA, 0x0002 ); // 32 greyscale 3Byte 3Pixel mode
    lcdWrite( COMMAND, 0x0075 ); // Line address set (LASET)
    lcdWrite( DATA, 0x0000 ); // Start Line = 0
    lcdWrite( DATA, 0x0077 ); // End Line = 119  (120 rows - 1)
    lcdWrite( COMMAND, 0x0015 ); // Column address set (CASET)
    lcdWrite( DATA, 0x0000 ); // Start Column = 0
    lcdWrite( DATA, 0x004F ); // End Column = 79 (240 columns / 3 - 1)
    lcdWrite( COMMAND, 0x0031 ); // Set Ext = 1
    lcdWrite( COMMAND, 0x0032 ); // Analog Circuit Set (ANASET)
    lcdWrite( DATA, 0x0000 ); // OSC Frequency = 000 (default, 12.7kHz)
    lcdWrite( DATA, 0x0000 ); // Booster Efficiency = 00 (3kHz)
    lcdWrite( DATA, 0x0002 ); // Bias = 1/12
    lcdWrite( COMMAND, 0x0034 ); // Software Initial (SWINT)

    // Reset scroll
    lcdWrite( COMMAND, 0x0030 ); // Set Ext = 0
    lcdWrite( COMMAND, 0x00AB );
    lcdWrite( DATA, 36 ); // Initial scroll position
    // Invert display
    lcdWrite( COMMAND, 0x00A7 );
    // Display on
    lcdWrite( COMMAND, 0x00AF ); // Display On (DISON)
}

void ST7529_Clear() {
    lcdWrite( COMMAND, 0x0030 ); // Set Ext = 0
    lcdWrite( COMMAND, 0x0015 ); // Column address set
    lcdWrite( DATA, 0 );
    lcdWrite( DATA, 79 );
    lcdWrite( COMMAND, 0x0075 ); // Line address set
    lcdWrite( DATA, 0 ); // From line 0
    lcdWrite( DATA, 127 ); // To line 119
    lcdWrite( COMMAND, 0x05C );
    for (size_t i=0; i < sizeof(framebuffer); i++) {
        framebuffer[i] = 0;
    }
    for (int y = 0; y < LCD_ROWS; y++) {
        ESP.wdtFeed();
        for (int x = 0; x < LCD_COLS; x+=3) {
            lcdWrite( DATA, 0 );
            lcdWrite( DATA, 0 );
            lcdWrite( DATA, 0 );
        }
    }
}

void ST7529_SetPixel(uint8_t x, uint8_t y, uint8_t level) {
    level = level << 3;
    lcdWrite( COMMAND, 0x0030 ); // Set Ext = 0
    lcdWrite( COMMAND, 0x0015 ); // Column address set
    lcdWrite( DATA, x / 3 );
    lcdWrite( DATA, 79 );
    lcdWrite( COMMAND, 0x0075 ); // Page address set
    lcdWrite( DATA, y ); // From line 0
    lcdWrite( DATA, 127 ); // To line 127
    framebuffer[x + (y*LCD_COLS)] = level;
    lcdWrite( COMMAND, 0x05C );
    for (int i=0; i<3; i++) {
        uint8_t pixel = framebuffer[(x/3)*3 + (y*LCD_COLS) + i];
        lcdWrite( DATA, pixel );
    }
}

void ST7529_Diagonal() {
    for (int i=0; i<LCD_ROWS && i<LCD_COLS; i++) {
        ST7529_SetPixel(i, i, 0x1F);
    }
    for (int i=0; i<LCD_COLS; i++) {
        ST7529_SetPixel(i, 0, 0x1F);
    }
    for (int i=0; i<LCD_ROWS; i++) {
        ST7529_SetPixel(0, i, 0x1F);
    }
}

bool initializeLCD() {
    // Displaytech 128240D
    // https://www.digikey.com/en/products/detail/displaytech/128240D-FC-BW-3/6650341
    // ST7529 driver chip
    // https://www.crystalfontz.com/controllers/Sitronix/ST7529/
    // IF1=L IF2=L IF3=H (9-bit serial, 3 line)
    delay(1); // wait for power to stabilize
    ST7529_Init();
    // Clear screen
    ST7529_Clear();
    // Display a pattern
    ST7529_Diagonal();
    return true;
}

void setup() {
  Serial.begin(115200);

  // Wifi Setup
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  digitalWrite(LCD_CS, 1);
  pinMode(LCD_CS, OUTPUT);
  digitalWrite(LCD_RST, 0);
  pinMode(LCD_RST, OUTPUT);
  pinMode(WATER_SENSE, INPUT_PULLUP);
  pinMode(PRESSURE_SW_IN, INPUT_PULLUP);
  pinMode(PUMP_CNTRL, OUTPUT);
#ifdef HARDWARE_SPI
  spi_init(HSPI); // 4MHz
  spi_mode(HSPI, 1, 1);
#else
  digitalWrite(LCD_SI, 0);
  pinMode(LCD_SI, OUTPUT);
  digitalWrite(LCD_SCL, 0);
  pinMode(LCD_SCL, OUTPUT);
#endif

  Serial.println("LCD tester");
  Serial.println();
  Serial.print("Connecting to SSID\n" WIFI_SSID ": ");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("connected");

  mdns_success = false;
  if (MDNS.begin(MDNS_NAME)) {
    mdns_success = true;
    Serial.println("MDNS responder started: " MDNS_NAME);
  }

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname(MDNS_NAME);
  ArduinoOTA.setPassword(OTA_PASSWD);
  ArduinoOTA
  .onStart([]() {
    String type;
    digitalWrite(LED_BUILTIN, LOW);
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();

  initializeLCD();
}

void loop() {
    ArduinoOTA.handle();
    MDNS.update();
    if (!updateDelay.isExpired()) {
        return;
    }
    updateDelay.restart();

    blinkWasOn = !blinkWasOn;
    digitalWrite(LED_BUILTIN, blinkWasOn);
}

#endif /* LCDTESTER */
