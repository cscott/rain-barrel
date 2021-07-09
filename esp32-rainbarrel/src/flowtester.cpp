#ifdef FLOWTESTER
#define SNITCH_TESTER // also used to test the I2C on the snitch!
#include "Arduino.h"
#include <Wire.h>
#include "flowmeter.h"
#ifdef SNITCH_TESTER
# include "smrtysnitch.h"
# include "smrty_decode.h"
#endif

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

// includes for OLED display
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "config.h"
#define MDNS_NAME "flowtester"

// Configuration of OLED display
Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);

// OLED FeatherWing buttons map to different pins depending on board:
#define BUTTON_A  0
#define BUTTON_B 16
#define BUTTON_C  2

// Other config
#define LED_GPIO 0
#define LED_OFF 1 // active low
#define LED_ON  0 // active low

bool mdns_success;

void normalText() {
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
}
void invertText() {
  display.setTextColor(SH110X_BLACK, SH110X_WHITE);
}
void maybeInvertText(bool maybe) {
  if (maybe) invertText(); else normalText();
}

void setup() {
  Wire.begin();
  Serial.begin(115200);

  // Wifi Setup
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // OLED setup
  display.begin(0x3C, true); // Address 0x3C default
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

  // Clear the buffer.
  display.clearDisplay();
  display.setRotation(1);

  display.setTextSize(1);
  display.setCursor(0,0);

  invertText();
  display.println("Flow meter tester");
  normalText();

  display.println("");
  display.print("Connecting to SSID\n" WIFI_SSID ": ");
  display.display();

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.display();

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(WIFI_SSID);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  mdns_success = false;
  if (MDNS.begin(MDNS_NAME)) {
    mdns_success = true;
    Serial.println("MDNS responder started: " MDNS_NAME);
    display.println("mDNS: " MDNS_NAME);
    display.display();
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
}

void printHex(uint8_t num) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", (int)num);
    display.print(buf);
}

void updateDisplay(
#ifdef SNITCH_TESTER
                   uint8_t *buf
#else
                   uint64_t count
#endif
) {
  display.clearDisplay();
  display.setCursor(0,0);
  display.setTextSize(1);
  invertText();
  display.println("Flow meter tester");
  normalText();
  display.print("IP: ");
  display.println(WiFi.localIP());
  if (mdns_success) {
      display.println("mDNS: " MDNS_NAME);
  }
#ifdef SNITCH_TESTER
  display.print("Seq "); printHex(buf[0]); display.println();
  for (int i=0; i<3; i++) {
      for (int j=0; j<8; j++) {
          if (j==1||j==4||j==6||j==7) { display.print(" "); }
          printHex(buf[8*i + j + 1]);
      }
      // verify checksum
      uint8_t checksum=0;
      for (int j=0; j<7; j++) {
          checksum += buf[8*i + j + 1];
      }
      if (checksum != buf[8+i + 7 + 1]) {
          display.print("*");
      }
      display.println();
  }
#else
  display.setTextSize(2);
  display.println(count);
#endif
  display.display();
}

void loop() {
    ArduinoOTA.handle();
    MDNS.update();

#ifdef SNITCH_TESTER
    Wire.requestFrom(SNITCH_I2C_ADDR, 1 + (8*3));
    uint8_t buf[1+(8*3)];
    memset(buf, 0xFF, sizeof(buf));
    for (int i=0; Wire.available(); i++) {
        buf[i] = Wire.read();
    }
    updateDisplay(buf);
#else
    Wire.requestFrom(FLOWMETER_I2C_ADDR, 8);
    uint64_t count = 0;
    for (int i=0; Wire.available(); i++) {
        count |= ((uint64_t)Wire.read()) << (8*i);
    }
    updateDisplay(count);
#endif
}

#endif /* FLOWTESTER */
