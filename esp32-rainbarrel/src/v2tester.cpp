#if defined(V2TESTER) || defined(V2TESTER32)
#include "Arduino.h"
#include <Wire.h>
#include <AsyncDelay.h>

#ifdef V2TESTER
# include <ESP8266WiFi.h>
# include <ESP8266mDNS.h>
#endif
#ifdef V2TESTER32
# include <WiFi.h>
# include <ESPmDNS.h>
#endif
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <Adafruit_GFX.h>

// If you use HAS_OLED, you need to adjust the library dependencies in
// platformio.ini to (re)include 'Adafruit SH110X'
//#define HAS_OLED
#define HAS_ST7529
#define HAS_ADP1650

// includes for OLED display
#ifdef HAS_OLED
#include <Adafruit_SH110X.h> // I2C 0x3C
#endif
#ifdef HAS_ST7529
#include "st7529.h"
#endif

#include "config.h"
#define MDNS_NAME "flowtester" // keep same name as flowtester

#include "flowmeter.h" // I2C 0x08 (not connected)
#include "smrtysnitch.h" // I2C 0x24
#include "cap1298.h"   // I2C 0x28
#include "adp1650.h"   // I2C 0x30 (not found :( )
//#include <Adafruit_MotorShield.h> // I2C 0x60

#ifdef HAS_OLED
// Configuration of OLED display
Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);
#define WHITE SH110X_WHITE
#define BLACK SH110X_BLACK

// OLED FeatherWing buttons map to different pins depending on board:
#define BUTTON_A  0
#define BUTTON_B 16
#define BUTTON_C  2
#endif

// Other config
#ifdef V2TESTER
# define LED_GPIO        0
# define LCD_RST         2      // F0
# define PRESSURE_SW_IN 16      // F1 (pressure switch input)
# define PUMP_CNTRL      0      // F2 (power tail out)
# define LCD_CS         15      // F3
# define WATER_SENSE    12      // F5 (water present) (also MISO)
# define LCD_SI         13 // MOSI
# define LCD_SCL        14 // SCK
#endif
#ifdef V2TESTER32
# define LED_GPIO       13
# define LCD_RST        14      // F0
# define PRESSURE_SW_IN 32      // F1 (pressure switch input)
# define PUMP_CNTRL     15      // F2 (power tail out)
# define LCD_CS         33      // F3
# define WATER_SENSE    12      // F5 (water present, has a pull-down, boot!)
# define LCD_SI         18 // MOSI
# define LCD_SCL         5 // SCK
#endif

#define LED_OFF 1 // active low
#define LED_ON  0 // active low

#ifdef HAS_ST7529
ST7529_LCD display = ST7529_LCD(240, 128, LCD_RST, LCD_CS, LCD_SCL, LCD_SI);
#define WHITE ST7529_BLACK
#define BLACK ST7529_WHITE
#endif

AsyncDelay updateDelay = AsyncDelay(100, AsyncDelay::MILLIS);
bool mdns_success;
bool adp1650_found = false;
AsyncDelay blinkDelay = AsyncDelay(1000, AsyncDelay::MILLIS);
bool blinkWasOn = false;

#if defined(HAS_OLED) || defined(HAS_ST7529)
void normalText() {
  display.setTextColor(WHITE, BLACK);
}
void invertText() {
  display.setTextColor(BLACK, WHITE);
}
void maybeInvertText(bool maybe) {
  if (maybe) invertText(); else normalText();
}

void printHex(uint8_t num) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", (int)num);
    display.print(buf);
}
#endif

bool writeRegister(uint8_t i2c_addr, uint8_t regno, uint8_t val) {
  Wire.beginTransmission(i2c_addr);
  Wire.write(regno);
  Wire.write(val);
  uint8_t status = Wire.endTransmission();
  if (status != 0) {
      return false;
  }
  return true;
}

bool readRegister(uint8_t i2c_addr, uint8_t regno, uint8_t *val) {
  Wire.beginTransmission(i2c_addr);
  Wire.write(regno);
  uint8_t status = Wire.endTransmission();
  if (status != 0) {
      return false;
  }
  uint8_t nBytes = Wire.requestFrom(i2c_addr, (uint8_t)1);
  if (nBytes == 0) {
      return false;
  }
  *val = Wire.read();
  return true;
}

void setup() {
  Wire.begin();
  Serial.begin(115200);

  // Wifi Setup
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  digitalWrite(LED_BUILTIN, LED_ON);
  pinMode(LED_BUILTIN, OUTPUT);

  digitalWrite(LCD_CS, 1);
  pinMode(LCD_CS, OUTPUT);
  digitalWrite(LCD_RST, 0);
  pinMode(LCD_RST, OUTPUT);

  adp1650_found = false;
  uint8_t val;
  if (readRegister(ADP1650_I2C_ADDR, 0x00, &val) && val == 0x22) {
      adp1650_found = true;
  }
#ifdef HAS_OLED
  // OLED setup
  display.begin(0x3C, true); // Address 0x3C default
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);
#endif
#ifdef HAS_ST7529
  // assume that if adp1650 was found, then external booster is being used
  // XXX be safe, since we know we've wired up an external power supply,
  // don't take the chance of having them fight.
  bool st = display.begin(false /*!adp1650_found*/);
  Serial.print("Display begin: ");
  Serial.println(st);
#endif

  pinMode(WATER_SENSE, INPUT_PULLUP);
  pinMode(PRESSURE_SW_IN, INPUT_PULLUP);
  pinMode(PUMP_CNTRL, OUTPUT);

  // CAP1298 setup
  // main control => combo mode, active gain=1, standby gain=8
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_MAIN_CONTROL, 0x0E);
  // No multitouch limiting
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_MULTI_TOUCH_CONFIG, 0x00);
  // No repeating interrupts
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_REPEAT_ENABLE, 0x00);
  // No signal guard (default value). CS5 was supposed to be guard for CS6,
  // but as it turns out CS6 has too large a capacitance to be used.
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_SIGNAL_GUARD_ENABLE, 0x00);
  // Enable CS1-CS4 in "active" mode.
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_SENSOR_INPUT_ENABLE, 0x0F);
  // Default 32x sensitivity for active mode. (bump to 1F or 0F for more sens.)
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_SENSITIVITY_CONTROL, 0x2F);
  // Default averaging and sensing cycle time for active channels
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_SAMPLING_CONFIG, 0x39);
  // Enable CS5 in "standby" mode (CS6 is unusable)
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_STANDBY_CHANNEL, 0x10);
  // Default averaging and sensing cycle time for standby channels
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_STANDBY_CONFIG, 0x39);
  // Maximum 128x sensitivity for standby channels.
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_STANDBY_SENSITIVITY, 0x00);
  // Gain adj: CS1=1, CS2=1, CS3=2, CS4=2
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_CALIBRATION_SENSITIVITY1, 0x50);
  // Gain adj: CS5=1 CS6=1 CS7=1 CS8=1
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_CALIBRATION_SENSITIVITY2, 0x00);
  // Recalibrate all channels now.
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_CALIBRATION, 0x1F);

#if defined(HAS_OLED) || defined(HAS_ST7529)
  // Clear the buffer.
  display.clearDisplay();
#ifdef HAS_OLED
  display.setRotation(1);
#endif

  display.setTextSize(1);
  display.setCursor(0,0);

  invertText();
  display.println("Rainbarrel V2 tester");
  Serial.println("Rainbarrel V2 tester");
  normalText();

  display.println("");
  Serial.println();
  display.print("Connecting to SSID\n" WIFI_SSID ": ");
  Serial.print("Connecting to SSID\n" WIFI_SSID ": ");
  display.display();
#endif

  // Wait for connection
#ifdef V2TESTER
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
#endif
#ifdef V2TESTER32
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
#endif
  Serial.println();

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
#if defined(HAS_OLED) || defined(HAS_ST7529)
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.display();
#endif

  mdns_success = false;
  if (MDNS.begin(MDNS_NAME)) {
    mdns_success = true;
    Serial.println("MDNS responder started: " MDNS_NAME);
#if defined(HAS_OLED) || defined(HAS_ST7529)
    display.println("mDNS: " MDNS_NAME);
    display.display();
#endif
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
  Serial.println("Startup complete.");
}

void updateDisplay(bool blink) {
#if defined(HAS_OLED) || defined(HAS_ST7529)
    uint8_t nBytes, status, val;

  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextSize(1);
  invertText();
  display.print("V2 tester");
  display.println(blink ? "!" : " ");
  normalText();
  display.print("IP: ");
  display.println(WiFi.localIP());
  if (mdns_success) {
      display.println("mDNS: " MDNS_NAME);
  }

  // Test snitch
  display.print("Snitch: ");
  Wire.beginTransmission(SNITCH_I2C_ADDR);
  Wire.write(0);
  status = Wire.endTransmission();
  if (status != 0) {
      display.print("NOT FOUND");
  } else {
      do {
          nBytes = Wire.requestFrom(SNITCH_I2C_ADDR, 10);
          if (nBytes == 0) {
              display.print("BAD COMM");
              break;
          }
      } while (Wire.read() != 0);
      for (int j=0; Wire.available() && j<9; j++) {
          val = Wire.read();
          if (j < 6) { printHex(val); }
      }
  }
  display.println();

  // Test cap sense
  display.print("CAP1298: ");
#if 0
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_PRODUCT_ID, &val)) {
      printHex(val); display.print(" ");
  } else {
      display.print("XX ");
  }
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_MANUFACTURER_ID, &val)) {
      printHex(val); display.print(" ");
  } else {
      display.print("XX ");
  }
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_REVISION, &val)) {
      printHex(val); display.print(" ");
  } else {
      display.print("XX ");
  }
  display.print("STA:");
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_GENERAL_STATUS, &val)) {
      printHex(val); display.print(" ");
  } else {
      display.print("XX ");
  }
  display.print("CAL:");
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_CALIBRATION, &val)) {
      printHex(val); display.print(" ");
  } else {
      display.print("XX ");
  }
  display.print("NOI:");
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_NOISE_FLAG_STATUS, &val)) {
      printHex(val); display.print(" ");
  } else {
      display.print("XX ");
  }
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_BASE_COUNT_OOL, &val)) {
      printHex(val); display.print(" ");
  } else {
      display.print("XX ");
  }
#endif
  bool bright = false;
  display.print("SEN:");
  if (readRegister(CAP1298_I2C_ADDR, CAP1298_REG_SENSOR_STATUS, &val)) {
      printHex(val);
      bright = (val&0x10);
  } else {
      display.print("XX");
  }
  display.println();
#if 0
  for (int i=0; i<8; i++) {
      val = 0xCA;
      //readRegister(CAP1298_I2C_ADDR, CAP1298_REG_SENSOR_DELTA_COUNT(i), &val);
      readRegister(CAP1298_I2C_ADDR, CAP1298_REG_SENSOR_INPUT_CALIBRATION(i), &val);
      printHex(val); display.print(" ");
  }
  display.println();
#endif
  // clear interrupt
  readRegister(CAP1298_I2C_ADDR, CAP1298_REG_MAIN_CONTROL, &val);
  writeRegister(CAP1298_I2C_ADDR, CAP1298_REG_MAIN_CONTROL, val&0xFE);

#ifdef HAS_ADP1650
  // Try to connect to ADP1650
  adp1650_found = false;
  display.print("ADP1650: ");
  if (readRegister(ADP1650_I2C_ADDR, 0x00, &val)) {
      printHex(val);
      if (val == 0x22) {
          adp1650_found = true;
      }
  } else {
      display.print("XX");
  }
  display.println();
  if (adp1650_found) {
      // 0 = 25mA.  Nominal=120mA; 4 = 125mA.  So "3" is probably the right value.
      // but "max current" on the datasheet is 180mA
      writeRegister(ADP1650_I2C_ADDR, 0x03 /* Current Set Register */, bright ? 4 : 0 /*25mA*/);
      writeRegister(ADP1650_I2C_ADDR, 0x04 /* Output Mode Register */, 0x10 | ((blink||bright) ? 0xA : 0x0));
  }
#endif

#if 1
  for (uint8_t address = 1; address < 127; address++) {
      Wire.beginTransmission(address);
      int error = Wire.endTransmission();
      if (error == 0) {
          printHex(address); display.print(" ");
      } else if (error == 4) {
          printHex(address); display.print("! ");
      }
  }
#endif
#if 0
  // test VLCD power supply by displaying a checkerboard pattern
  for (int i=0; i<display.width()+display.height(); i+=2) {
      display.drawLine(0,i,i,0,WHITE);
  }
#endif

  display.display();
#endif
}

void loop() {
    ArduinoOTA.handle();
#ifdef V2TESTER
    MDNS.update();
#endif
    if (blinkDelay.isExpired()) {
        blinkWasOn = !blinkWasOn;
        blinkDelay.restart();
    }
    if (!updateDelay.isExpired()) {
        return;
    }
    updateDelay.restart();

    updateDisplay(blinkWasOn);
    digitalWrite(LED_BUILTIN, blinkWasOn ? LED_ON : LED_OFF);
}

#endif /* V2TESTER || V2TESTER32 */
