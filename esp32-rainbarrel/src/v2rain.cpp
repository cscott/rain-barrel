#if defined(RAINPUMP_V2) || defined(RAINGAUGE_V2)
#if defined(RAINPUMP_V2) && defined(RAINGAUGE_V2)
# error Can not define both RAINPUMP_V2 and RAINGAUGE_V2, pick one!
#endif

#define MOTOR_DRIVER_CONNECTED
#define FLOWMETER_CONNECTED
#define ADC_CONNECTED
#define USE_MQTT
#undef DEV_MODE // shorter delays for easier development

#include "Arduino.h"
#include <Wire.h>
#include <SPI.h> // this is done to help pio discover this dependency
#include <AsyncDelay.h>

#ifdef ESP32
# include <WiFi.h>
# include <ESPmDNS.h>
#else
# include <ESP8266WiFi.h>
# include <ESP8266mDNS.h>
#endif
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <Adafruit_GFX.h>

#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>
#include <ArduinoJson.h>
#include <AsyncDelay.h>
#include "config.h"
#include "bluehigh-10px.h"
#include "bluebold-11px.h"
#include "icons.h"

// V2 hardware features
#include "st7529.h"
#include "cap1298.h"
// also adp1650.h, but that's not working yet
// V2 pin assignments
#if defined(ARDUINO_FEATHER_ESP32)
# define LED_GPIO       13
# define LCD_RST        14      // F0
# define PRESSURE_SW    32      // F1 (pressure switch input)
# define PUMP_CNTRL     15      // F2 (power tail out)
# define LCD_CS         33      // F3
# define WATER_SENSE    12      // F5 (water present, has a pull-down, boot!)
# define LCD_SI         18 // MOSI
# define LCD_SCL         5 // SCK
#elif defined(ARDUINO_ESP8266_ADAFRUIT_HUZZAH) // FEATHER_8266
# define LED_GPIO       -1 // steps on GPIO 0, suppress
# define LCD_RST         2 // F0 / GPIO  2 => LCD_RST (out)
# define PRESSURE_SW    16 // F1 / GPIO 16 => Pressure switch (in)
# define PUMP_CNTRL      0 // F2 / GPIO  0 => Pump (out)
# define LCD_CS         15 // F3 / GPIO 15 => LCD_CS (out)
# define LCD_SI         13 // F4 / GPIO 13 => LCD_MOSI (out)
# define WATER_SENSE    10 // F5 / GPIO 12 => Water sensor (in)
# define LCD_SCL        14 // F6 / GPIO 14 => LCD_SCK
#else
# error Unknown pin assignments
#endif

#ifdef RAIN_SERVER_v2
# include <Adafruit_MotorShield.h>
# include "flowmeter.h"
# include "smrtysnitch.h"
# include "smrty_decode.h"
#endif
#ifdef RAINGAUGE_V2
# ifdef ESP32
#  include <HTTPClient.h>
# else
#  include <ESP8266HTTPClient.h>
# endif
#ifdef ADC_CONNECTED
# include <Adafruit_ADS1X15.h>
#endif
#endif

#define SERVER_MDNS_NAME "rainpump"
#define CLIENT_MDNS_NAME "raingauge"
#define WATER_ALARM_LOW 100 /* 10% */
#define KEEPALIVE_INTERVAL_SECS 60
#define LEVEL_SENSOR_INTERVAL_SECS 1
#define SERVER_PORT 80
#define WATER_READING_ZERO 0
// (old) rain barrel 1 max observed 17194 rain barrel 2 got up to 17414
// (new adc) around 32000 seems to be full, measured up to 32751 in testing
#define WATER_READING_FULL 32000
// From datasheet: "Frequency(Hz) = (8.1Q) -3 +/- 10% where Q is L/min"
// So if G(gallons/min) = Q/3.78541
// 1 gal/s = 60 gal/min = 227.1246 L/min => 1839.7-3 ~= 1837Hz
// We count both transitions, so 3673.4 ticks per gallon.
#define TICKS_PER_GALLON 3673.4

#define COLOR1 0x00 // was: EPD_WHITE
#define COLOR2 0x7F // was: EPD_LIGHT
#define COLOR3 0xBF // was: EPD_DARK
#define COLOR4 0xFF // was: EPD_BLACK

// General note: Most (or all?) of the intervals below have small offsets added
// to make the interval a prime # of milliseconds.  This ensures that intervals
// will tend to space themselves out over time and not all pile up at the same
// millisecond boundary.

#define LCD_WIDTH 240
#define LCD_HEIGHT 128
ST7529_LCD display = ST7529_LCD(LCD_WIDTH, LCD_HEIGHT, LCD_RST, LCD_CS, LCD_SCL, LCD_SI);

AsyncDelay displayMaxRefresh = AsyncDelay(4 * 60 * 60 * 1000 + 11, AsyncDelay::MILLIS); // 6x a day need it or not
AsyncDelay displayMinRefresh = AsyncDelay(13, AsyncDelay::MILLIS); // no more than once/10ms seconds

struct ButtonPress {
  ButtonPress(): a(false), b(false), c(false), d(false), prox(false) {}
  boolean a : 1;
  boolean b : 1;
  boolean c : 1;
  boolean d : 1;
  boolean prox : 1;
  bool operator==(const ButtonPress& rhs) const {
    return a == rhs.a && b == rhs.b && c == rhs.c && d == rhs.d && prox == rhs.prox;
  }
  boolean pressed() {
      return a || b || c || d;
  }
  void dump() {
      Serial.print(a ? "A" : " ");
      Serial.print(b ? "B" : " ");
      Serial.print(c ? "C" : " ");
      Serial.print(d ? "D" : " ");
      Serial.print(prox ? "X" : " ");
      Serial.println();
  }
};
ButtonPress lastButtonPress;

AsyncDelay debounceDelay = AsyncDelay(250 + 1, AsyncDelay::MILLIS); // switch debounce
#ifdef RAINGAUGE_V2
AsyncDelay capReadInterval = AsyncDelay(50 + 3, AsyncDelay::MILLIS); // read @ 20Hz
#endif

// WiFiClientSecure for SSL/TLS support (but this is broken!)
WiFiClientSecure client;
// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);
AsyncDelay mqttDelay = AsyncDelay(15000 + 3, AsyncDelay::MILLIS); // retry every 15 seconds if not connected

#ifdef ESP32
// io.adafruit.com root CA
const char* adafruitio_root_ca = \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh\n" \
    "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n" \
    "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD\n" \
    "QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT\n" \
    "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n" \
    "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG\n" \
    "9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB\n" \
    "CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97\n" \
    "nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt\n" \
    "43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P\n" \
    "T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4\n" \
    "gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO\n" \
    "BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR\n" \
    "TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw\n" \
    "DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr\n" \
    "hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg\n" \
    "06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF\n" \
    "PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls\n" \
    "YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk\n" \
    "CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=\n" \
    "-----END CERTIFICATE-----\n";
#else
// io.adafruit.com SHA1 fingerprint
static const char *adafruit_fingerprint PROGMEM = "59 3C 48 0A B1 8B 39 4E 0D 58 50 47 9A 13 55 60 CC A0 1D AF";
//X509List cert(adafruitio_root_ca);
#endif

/****************************** Feeds ***************************************/

// Setup a feed called 'test' for publishing.
// Notice MQTT paths for AIO follow the form: <username>/feeds/<feedname>
#define FEED_PREFIX AIO_USERNAME "/feeds/rain-barrels."
#ifdef RAINPUMP_V2
AsyncDelay valveStateFeedMaxDelay = AsyncDelay(60*60*1000 + 1, AsyncDelay::MILLIS); // at least 1/hr
AsyncDelay valveStateFeedMinDelay = AsyncDelay(1000 + 13, AsyncDelay::MILLIS); // not more than 1/second
Adafruit_MQTT_Publish valveStateFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "water-source");
#endif
#ifdef RAINGAUGE_V2
// 11s so they don't hit at the same time as the SMRT-Y poll, which is 9s
AsyncDelay waterLevelFeedDelay = AsyncDelay(11*1000 + 3, AsyncDelay::MILLIS); // every ~10s
Adafruit_MQTT_Publish waterLevel1Feed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "waterlevel1");
Adafruit_MQTT_Publish waterLevel2Feed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "waterlevel2");
#endif

#ifdef RAINPUMP_V2
# define MDNS_NAME SERVER_MDNS_NAME
#endif
#ifdef RAINGAUGE_V2
# define MDNS_NAME CLIENT_MDNS_NAME
#endif

AsyncDelay connectionWatchdog = AsyncDelay(5*60*1000 + 7, AsyncDelay::MILLIS);

#ifdef RAINPUMP_V2
// Motorshield configuration
Adafruit_MotorShield AFMS = Adafruit_MotorShield();
Adafruit_DCMotor *cityValve = AFMS.getMotor(1);
Adafruit_DCMotor *ground1 = AFMS.getMotor(2);
Adafruit_DCMotor *ground2 = AFMS.getMotor(3);
Adafruit_DCMotor *pump = AFMS.getMotor(4);

WebServer server(SERVER_PORT);
AsyncDelay lastPing = AsyncDelay(3 * KEEPALIVE_INTERVAL_SECS * 1000  + 1, AsyncDelay::MILLIS);
// The valve seems to get hot if you keep it activated -- though it should not!  Anyway, only
// run the valve for a minute when it changes state.
int lastValveState = -1;
AsyncDelay valveRunLength = AsyncDelay(60 * 1000 + 13, AsyncDelay::MILLIS);

// This interval is offset just a smidge because we ideally want to bin the
// flow per minute. But being .002% too high shouldn't matter.
// But do try to generate at least 1 data point per day
uint64_t lastFlowMeterReading = 0;
bool lastFlowMeterReadingValid = false;
AsyncDelay flowMeterInterval = AsyncDelay(60 * 1000 - 1, AsyncDelay::MILLIS);
AsyncDelay flowMeterIntervalMax = AsyncDelay(24 * 60 * 60 * 1000 - 7, AsyncDelay::MILLIS);
Adafruit_MQTT_Publish flowMeterFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "irrigation-flow");

Adafruit_MQTT_Publish smrtyRawFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "smrty-raw");
Adafruit_MQTT_Publish soilMoistureFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "smrty-moisture");
Adafruit_MQTT_Publish soilTemperatureFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "smrty-temperature");
// This is polling delay; it is also minimum MQTT publish interval so we don't
// get throttled.
// 9s so they don't hit at the same time as the water level poll, which is 11s
AsyncDelay smrtyInterval = AsyncDelay(9 * 1000 + 1, AsyncDelay::MILLIS);
uint8_t smrtyLastSeqno = 0xFF;
#endif

#ifdef RAINGAUGE_V2
#ifdef ADC_CONNECTED
Adafruit_ADS1115 ads1115;
#endif
AsyncDelay lastLevelReading = AsyncDelay(100, AsyncDelay::MILLIS);
AsyncDelay lastPing = AsyncDelay(KEEPALIVE_INTERVAL_SECS * 1000 - 1, AsyncDelay::MILLIS);
String serverBaseUrl = String("http://" SERVER_MDNS_NAME ".local:80/update");

// It would really be nice if we could do this filtering on the analog
// side instead of the digital side... (we tried!)
// Actually - one of our sensors is much noisier than the other! HW issue?
#define LEVEL_SAMPLE_FILTER 4
int32_t level_accum[2] = { 0, 0 };
#endif

enum PumpState {
  STATE_CITY = 0,
  STATE_AUTO = 1,
  STATE_RAIN = 2,
};

typedef struct SystemState {
  int user_state; // State selected via the UI
  int active_state; // Running state (ie, not 'auto')
  boolean connected_recently;
  boolean pipe_water_present;
  int water_level[2];
} SystemState;

bool state_equal(SystemState *a, SystemState *b) {
  if (a->user_state != b->user_state) {
    return false;
  }
  if (a->active_state != b->active_state) {
    return false;
  }
  if (a->connected_recently != b->connected_recently) {
    return false;
  }
  if (a->pipe_water_present != b->pipe_water_present) {
    return false;
  }
  for (int i = 0; i < 2; i++) {
    // we'll call them equal if they are "close enough" (aka 10 counts)
    int diff = a->water_level[i] - b->water_level[i];
    if (diff < -10 || diff > +10) {
      return false;
    }
  }
  return true;
}

SystemState state = {
  STATE_AUTO,
  STATE_CITY,
  false,
  false,
  { 560, 630 },
};
SystemState last_displayed_state = state;
#ifdef RAINGAUGE_V2
SystemState last_xmit_state = state;
#endif

void updateState();

void normalface() {
  display.setFont(&bluehigh10pt7b);
}

void boldface() {
  display.setFont(&bluebold11pt7b);
}

#ifdef RAINPUMP_V2
// Web server
void handleRoot() {
  char temp[800];
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;

  snprintf(temp, 800,

           "<html>\n"
"  <head>\n"
"    <meta http-equiv='refresh' content='10'/>\n"
"    <title>Rainbarrel Pump Manager</title>\n"
"    <style>\n"
"      body { background-color: #fff; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }\n"
"    </style>\n"
"  </head>\n"
"  <body>\n"
"    <h1>Rainbarrel Pump Manager</h1>\n"
"    <p><a href='https://io.adafruit.com/cscott/dashboards/griggs-corner-rain-barrels?kiosk=true'>Dashboard</a></p>\n"
"    <p>Uptime: %02d:%02d:%02d</p>\n"
"    <p>User mode: %s %s %s</p>\n"
"    <p>Active water source: %s</p>\n"
"    <pre>%3d.%d%% %3d.%d%%</pre>\n"
"    <p>Contacted recently by rain gauge: %s</p>\n"
"    <img src=\"/test.svg\" />\n"
"  </body>\n"
"</html>",

           hr, min % 60, sec % 60,
           state.user_state == STATE_CITY ? "<b>CITY</b>" : "<a href='/update?state=0'>city</a>",
           state.user_state == STATE_AUTO ? "<b>AUTO</b>" : "<a href='/update?state=1'>auto</a>",
           state.user_state == STATE_RAIN ? "<b>RAIN</b>" : "<a href='/update?state=2'>rain</a>",
           state.active_state == STATE_CITY ? "City water" : "Rain barrels",
           state.water_level[0]/10, state.water_level[0]%10,
           state.water_level[1]/10, state.water_level[1]%10,
           state.connected_recently ? "Yes" : "No"
          );
  server.send(200, "text/html", temp);
}

void handleUpdate() {
  // update level sensors
  boolean changedState = false;
  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "level1") {
      state.water_level[0] = server.arg(i).toInt();
      lastPing.restart();
    }
    if (server.argName(i) == "level2") {
      state.water_level[1] = server.arg(i).toInt();
      lastPing.restart();
    }
    if (server.argName(i) == "state") {
      state.user_state = server.arg(i).toInt();
      if (state.user_state < STATE_CITY || state.user_state > STATE_RAIN) {
        state.user_state = STATE_AUTO;
      }
      changedState = true;
    }
  }
  if (changedState) {
    updateState(); // update active state to match new user state
  }
  // output our current state
  StaticJsonDocument<96> doc;
  doc["user_state"] = state.user_state;
  doc["active_state"] = state.active_state;
  doc["pipe_water_present"] = state.pipe_water_present;
  JsonArray water_level = doc.createNestedArray("water_level");
  water_level.add(state.water_level[0]);
  water_level.add(state.water_level[1]);
  String out = "";
  serializeJson(doc, out);
  server.send(200, "text/json", out);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }

  server.send(404, "text/plain", message);
}

void drawGraph() {
  String out;
  out.reserve(2600);
  char temp[70];
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"400\" height=\"150\">\n";
  out += "<rect width=\"400\" height=\"150\" fill=\"rgb(250, 230, 210)\" stroke-width=\"1\" stroke=\"rgb(0, 0, 0)\" />\n";
  out += "<g stroke=\"black\">\n";
  int y = rand() % 130;
  for (int x = 10; x < 390; x += 10) {
    int y2 = rand() % 130;
    sprintf(temp, "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke-width=\"1\" />\n", x, 140 - y, x + 10, 140 - y2);
    out += temp;
    y = y2;
  }
  out += "</g>\n</svg>\n";

  server.send(200, "image/svg+xml", out);
}

bool readFlowMeter(uint64_t *result) {
    uint64_t count = 0;
#ifdef FLOWMETER_CONNECTED
    uint8_t nBytes = Wire.requestFrom(FLOWMETER_I2C_ADDR, 8);
    for (int i=0; Wire.available() > 0 && i<8; i++) {
        count |= ((uint64_t)Wire.read()) << (8*i);
    }
    while (Wire.available() > 0) {
        Wire.read();
    }
    *result = count;
    return (nBytes >= 8);
#endif
    return false;
}

bool readSmrtySnitch(uint8_t which, uint8_t *seqno, struct smrty_msg *msg, uint8_t *good_checksum) {
    Wire.beginTransmission(SNITCH_I2C_ADDR);
    Wire.write(which);
    uint8_t status = Wire.endTransmission();
    if (status != 0) {
        return false; // no response from client, must be disconnected
    }
    uint8_t reg, nBytes;
    do {
        nBytes = Wire.requestFrom(SNITCH_I2C_ADDR, 10);
        if (nBytes == 0) {
            return false; /* unexpected! */
        }
        reg = Wire.read();
    } while (reg != which);  // busy loop until reg. write is done
    if (nBytes < 10) {
        return false; /* incomplete read for some reason */
    }
    *seqno = Wire.read();
    uint8_t checksum = 0;
    uint8_t *p = (uint8_t*)msg;
    for (int i=0; i<8; i++, p++) {
        *p = Wire.read();
        if (i<7) {
            checksum += *p;
        }
    }
    *good_checksum = (checksum == msg->checksum) ? 1 : 0;
    return true;
}

void publishSmrty(uint8_t seqno, struct smrty_msg *msg, bool good_checksum) {
    const char *fmt = "[%02X] %02X %02X %02X %02X | %02X %02X %02X | %02X%s";
    char buf[strlen(fmt)+1]; // string will be shorter than this
    snprintf(buf, strlen(fmt)+1, fmt, seqno,
             msg->addr, msg->cmd, msg->tx_data1, msg->tx_data2,
             msg->rx_data1, msg->rx_data2, msg->status,
             msg->checksum, good_checksum ? "":"*");
    smrtyRawFeed.publish(buf);
    if (!good_checksum) { return; }
    // Do our best to decode these values.
    uint16_t tx_data = ((uint16_t)msg->tx_data2)<<8 | msg->tx_data1;
    uint16_t rx_data = ((uint16_t)msg->rx_data2)<<8 | msg->rx_data1;
    switch (tx_data) {
    case 0x000B:
        soilMoistureFeed.publish(((float)rx_data)/100.0);
        break;
    case 0x0005:
        // This could use more points to do a proper line fit
        // (This one done at https://mycurvefit.com/)
        soilTemperatureFeed.publish(33.1877 + (rx_data*.1089572));
        break;
    case 0x000E:
        // This is electrical conductivity, but it's always 0.0, so ignore it.
        break;
    default:
        break;
    }
}

bool pollSmrtySnitch(void) {
    struct smrty_msg read_msgs[SNITCH_BUFFER_SIZE];
    uint8_t read_seqno[SNITCH_BUFFER_SIZE];
    uint8_t read_good_checksum[SNITCH_BUFFER_SIZE];
    bool status;
    int i;
    for (i=0; i<SNITCH_BUFFER_SIZE; i++) {
        status = readSmrtySnitch(i, &read_seqno[i], &read_msgs[i], &read_good_checksum[i]);
        if (!status) {
            return false; // communications error, bail.
        }
        if (read_seqno[i] == 0xFF /* uninitialized */ ||
            read_seqno[i] == smrtyLastSeqno) {
            // we found one we already transmitted, don't need to fetch more
            break;
        }
    }
    // ok, transmit up to item i.
    for (--i; i>=0; i--) {
        publishSmrty(read_seqno[i], &read_msgs[i], read_good_checksum[i]);
        smrtyLastSeqno = read_seqno[i];
        return true; // This ensures that we don't get throttled by Adafruit IO
    }
    return true;
}

#endif

bool cap1298_write(uint8_t regno, uint8_t val) {
    Wire.beginTransmission(CAP1298_I2C_ADDR);
    Wire.write(regno);
    Wire.write(val);
    uint8_t status = Wire.endTransmission();
    if (status != 0) {
        return false;
    }
    return true;
}
bool cap1298_read(uint8_t regno, uint8_t *val) {
    Wire.beginTransmission(CAP1298_I2C_ADDR);
    Wire.write(regno);
    uint8_t status = Wire.endTransmission();
    if (status != 0) {
        return false;
    }
    uint8_t nBytes = Wire.requestFrom(CAP1298_I2C_ADDR, 1);
    if (nBytes == 0) {
        return false;
    }
    *val = Wire.read();
    return true;
}

void cap1298_setup() {
    // CAP1298 setup
    // main control => combo mode, active gain=1, standby gain=8
    cap1298_write(CAP1298_REG_MAIN_CONTROL, 0x0E);
    // No multitouch limiting
    cap1298_write(CAP1298_REG_MULTI_TOUCH_CONFIG, 0x00);
    // No repeating interrupts
    cap1298_write(CAP1298_REG_REPEAT_ENABLE, 0x00);
    // No signal guard (default value). CS5 was supposed to be guard for CS6,
    // but as it turns out CS6 has too large a capacitance to be used.
    cap1298_write(CAP1298_REG_SIGNAL_GUARD_ENABLE, 0x00);
    // Enable CS1-CS4 in "active" mode.
    cap1298_write(CAP1298_REG_SENSOR_INPUT_ENABLE, 0x0F);
    // Default 32x sensitivity for active mode. (bump to 1F or 0F for more sens.)
    cap1298_write(CAP1298_REG_SENSITIVITY_CONTROL, 0x2F);
    // Default averaging and sensing cycle time for active channels
    cap1298_write(CAP1298_REG_SAMPLING_CONFIG, 0x39);
    // Enable CS5 in "standby" mode (CS6 is unusable)
    cap1298_write(CAP1298_REG_STANDBY_CHANNEL, 0x10);
    // Default averaging and sensing cycle time for standby channels
    cap1298_write(CAP1298_REG_STANDBY_CONFIG, 0x39);
    // Maximum 128x sensitivity for standby channels.
    cap1298_write(CAP1298_REG_STANDBY_SENSITIVITY, 0x00);
    // Gain adj: CS1=1, CS2=1, CS3=2, CS4=2
    cap1298_write(CAP1298_REG_CALIBRATION_SENSITIVITY1, 0x50);
    // Gain adj: CS5=1 CS6=1 CS7=1 CS8=1
    cap1298_write(CAP1298_REG_CALIBRATION_SENSITIVITY2, 0x00);
    // Recalibrate all channels now.
    cap1298_write(CAP1298_REG_CALIBRATION, 0x1F);
}

void cap1298_read_buttons(ButtonPress *bp) {
    uint8_t val;
    if (!cap1298_read(CAP1298_REG_SENSOR_STATUS, &val)) {
        return;
    }
    bp->a = (val & 0x01) != 0;
    bp->b = (val & 0x02) != 0;
    bp->c = (val & 0x04) != 0;
    bp->d = (val & 0x08) != 0;
    bp->prox = (val & 0x10) != 0;
    // Clear interrupt
    cap1298_read(CAP1298_REG_MAIN_CONTROL, &val);
    cap1298_write(CAP1298_REG_MAIN_CONTROL, val&0xFE);
}

void setup() {
    Wire.begin();
    Serial.begin(115200);
    Serial.println("Setup!");
    // initialize built in LED pin as an output.
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

#ifdef RAINPUMP_V2
    pinMode(WATER_SENSE, INPUT_PULLUP);
    pinMode(PRESSURE_SW, INPUT_PULLUP);
    pinMode(PUMP_CNTRL, OUTPUT);
#endif

    digitalWrite(LCD_CS, 1);
    pinMode(LCD_CS, OUTPUT);
    digitalWrite(LCD_RST, 0);
    pinMode(LCD_RST, OUTPUT);

    Serial.println("WiFi setup");
    // Wifi Setup
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.println("cap1298 setup");
    cap1298_setup();

#ifdef RAINPUMP_V2
#ifdef MOTOR_DRIVER_CONNECTED
    // Motorshield Setup
    Serial.println("Motorshield setup");
    AFMS.begin();
    // This should ground all outputs
    cityValve->setSpeed(0); cityValve->run(FORWARD);
    ground1->setSpeed(0); ground1->run(FORWARD);
    ground2->setSpeed(0); ground2->run(FORWARD);
    pump->setSpeed(0); pump->run(FORWARD);
#endif
#endif

#ifdef RAINGAUGE_V2
#ifdef ADC_CONNECTED
    Serial.println("ADC setup");
    ads1115.begin(0x48); // At default address
    ads1115.setGain(GAIN_TWO); // +/-2.048V
#endif
#endif

    Serial.println("Display setup");
    display.begin();
    Serial.println(ESP.getFreeHeap(),DEC);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(COLOR4);
    boldface();
    display.setCursor(0, 20);
    display.println(MDNS_NAME);
    normalface();
    display.setTextWrap(true);
    display.print("Connecting to SSID: ");
    display.println(WIFI_SSID);
    Serial.println("About to display");
    display.display();

    Serial.println("Booting");

    // Wait for connection
#ifdef ESP32
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        Serial.println("Connection Failed! Rebooting...");
        delay(5000);
        ESP.restart();
    }
#else
    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        timeout++;
        if (timeout > 60) {
            Serial.println("Connection Failed! Rebooting...");
            ESP.restart();
        }
    }
#endif
    display.print("IP: ");
    display.println(WiFi.localIP());
    display.display();

    bool mdns_success = false;
    if (MDNS.begin(MDNS_NAME)) {
        mdns_success = true;
        Serial.println("MDNS responder started: " MDNS_NAME);
        display.println("mDNS: " MDNS_NAME);
        display.display();
    }

    // Set Adafruit IO's root CA
#ifdef ESP32
    client.setCACert(adafruitio_root_ca);
#else
    client.setFingerprint(adafruit_fingerprint);
    //client.setTrustAnchors(&cert);
#endif
    mqtt.connect();

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

#ifdef RAINGAUGE_V2
    // Find the pump server
    int n = MDNS.queryService("http", "tcp");
    for (int i = 0; i < n; i++) {
        Serial.print("  ");
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(MDNS.hostname(i));
        Serial.print(" (");
        Serial.print(MDNS.IP(i));
        Serial.print(":");
        Serial.print(MDNS.port(i));
        Serial.println(")");
        if (MDNS.hostname(i) == "rainpump" || MDNS.hostname(i) == "rainpump.local" ) {
            serverBaseUrl.remove(0);
            serverBaseUrl += "http://";
            serverBaseUrl += MDNS.IP(i).toString();
            serverBaseUrl += ":";
            serverBaseUrl += MDNS.port(i);
            serverBaseUrl += "/update";
            Serial.print("rainpump found: ");
            Serial.println(serverBaseUrl);
        }
    }
    for (int i=0; i<2; i++) {
        level_accum[i] = 0;
#ifdef ADC_CONNECTED
        for (int j=0; j<LEVEL_SAMPLE_FILTER; j++) {
            level_accum[i] += ads1115.readADC_SingleEnded(i);
        }
#endif
    }
#endif

#ifdef RAINPUMP_V2
    server.on("/", handleRoot);
    server.on("/test.svg", drawGraph);
    server.on("/inline", []() {
        server.send(200, "text/plain", "this works as well");
    });
    server.on("/update", handleUpdate);
    server.onNotFound(handleNotFound);
    server.begin();
    MDNS.addService("http", "tcp", SERVER_PORT);
    Serial.println("HTTP server started");
    valveRunLength.restart(); // run the valve to move it
    lastFlowMeterReadingValid = readFlowMeter(&lastFlowMeterReading);
    flowMeterInterval.restart(); // next read the flow meter in a minute
    flowMeterIntervalMax.expire(); // report the first reading regardless
#endif

    Serial.println("Ready");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    displayMaxRefresh.expire();
    displayMinRefresh.expire();
    lastPing.expire();
    connectionWatchdog.restart();
}

#ifdef RAINGAUGE_V2
void sendUpdate(int new_user_state = -1) {
  state.connected_recently = false;
  // test client
  HTTPClient http;
  WiFiClient client2; // not secure
  String url2 = serverBaseUrl;
  url2 += "?level1=";
  url2 += state.water_level[0];
  url2 += "&level2=";
  url2 += state.water_level[1];
  if (new_user_state >= STATE_CITY && new_user_state <= STATE_RAIN) {
    url2 += "&state=";
    url2 += new_user_state;
  }
  Serial.println(url2);
  http.begin(client2, url2);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    StaticJsonDocument<192> root;
    DeserializationError error = deserializeJson(root, http.getStream());
    if (error) {
      Serial.print(F("deserializeJson failed: "));
      Serial.println(error.f_str());
    } else {
      state.user_state = root["user_state"];
      state.active_state = root["active_state"];
      state.pipe_water_present = root["pipe_water_present"];
      state.connected_recently = true;
      connectionWatchdog.restart();
      Serial.printf("Got user_state %d\n", state.user_state);
    }
  } else {
      Serial.print("Bad HTTP response: ");
      Serial.println(httpCode);
  }
  http.end();
  lastPing.restart();
  last_xmit_state = state;
}
#endif

void updateState() {
#ifdef RAINGAUGE_V2
  if (lastLevelReading.isExpired()) {
    // use .repeat() to ensure our filter bandwidth is relatively constant
    // even if there's a (slow) display refresh between samples
    lastLevelReading.repeat();
    int32_t raw_level[2];
    for (int i=0; i<2; i++) {
      level_accum[i] =
        ((int64_t)level_accum[i] * (LEVEL_SAMPLE_FILTER-1) / LEVEL_SAMPLE_FILTER)
#ifdef ADC_CONNECTED
        + ads1115.readADC_SingleEnded(i)
#endif
          ;
      raw_level[i] = level_accum[i] / LEVEL_SAMPLE_FILTER;
      state.water_level[i] = (raw_level[i] - WATER_READING_ZERO) * 1000
        / (WATER_READING_FULL - WATER_READING_ZERO);
      if (state.water_level[i] < 0) {
        state.water_level[i] = 0;
      }
      if (state.water_level[i] > 1000) {
        state.water_level[i] = 1000;
      }
#ifdef DEV_MODE
      // XXX avoid cycling the valves in auto mode while we're testing
      if (state.water_level[i] <= WATER_ALARM_LOW) {
        state.water_level[i] = WATER_ALARM_LOW+1;
      }
#endif
    }
#ifdef USE_MQTT
    // periodically send levels to AIO
    if (waterLevelFeedDelay.isExpired() && mqtt.connected()) {
      // publishing raw levels for now, for calibration purpoes
      waterLevel1Feed.publish(raw_level[0]/*state.water_level[0]*/);
      waterLevel2Feed.publish(raw_level[1]/*state.water_level[1]*/);
      waterLevelFeedDelay.restart();
    }
#endif
  }
  if (lastPing.isExpired() || !state_equal(&state, &last_xmit_state)) {
    // send our levels to the server via a GET request and update our copy of the server state
    sendUpdate();
  }
#endif
#ifdef RAINPUMP_V2
  // read flow meter at intervals
  if (flowMeterInterval.isExpired()) {
    flowMeterInterval.repeat();
    if (mqtt.connected()) {
        uint64_t newFlow;
        bool valid = readFlowMeter(&newFlow);
        if (!valid) {
            lastFlowMeterReadingValid = false;
        } else if (!lastFlowMeterReadingValid) {
            lastFlowMeterReading = newFlow;
            lastFlowMeterReadingValid = true;
        } else {
            double gallons = (newFlow - lastFlowMeterReading) / (double)TICKS_PER_GALLON;
            // don't fill the log with a lot of unnecessary zeroes
            if (newFlow != lastFlowMeterReading ||
                flowMeterIntervalMax.isExpired()) {
                if (flowMeterFeed.publish(gallons)) {
                    lastFlowMeterReading = newFlow;
                    flowMeterIntervalMax.repeat();
                }
            }
        }
    }
  }
  // read the SMRTY snitch at interval
  if (smrtyInterval.isExpired()) {
      smrtyInterval.restart();
      if (mqtt.connected()) {
          pollSmrtySnitch();
      }
  }
  // read our pipe water sensor
  state.pipe_water_present = !digitalRead(WATER_SENSE);

  // implement "auto" mode
  if (state.user_state != STATE_AUTO) {
    // force a specific state
    state.active_state = state.user_state;
  } else {
    boolean is_rain_empty = !state.pipe_water_present;
    if (state.connected_recently && !is_rain_empty) { // use rain barrel levels
      is_rain_empty = (state.water_level[0] <= WATER_ALARM_LOW || state.water_level[1] <= WATER_ALARM_LOW);
    }
    // in auto
    if ( is_rain_empty ) {
      state.active_state = STATE_CITY;
    } else {
      state.active_state = STATE_RAIN;
    }
  }
  state.connected_recently = !lastPing.isExpired();
#ifdef MOTOR_DRIVER_CONNECTED
  // ok, now adjust settings to match selected state
  if (state.active_state == STATE_CITY) {
    // pump off
    pump->setSpeed(0);
    // move value to 'city'
    cityValve->run(FORWARD);
    if (lastValveState != STATE_CITY) {
      lastValveState = STATE_CITY;
      valveStateFeedMaxDelay.expire();

      cityValve->setSpeed(255);
      valveRunLength.restart();
    }
  } else {
    // move valve to 'rain'
    cityValve->run(BACKWARD);
    if (lastValveState != STATE_RAIN) {
      lastValveState = STATE_RAIN;
      valveStateFeedMaxDelay.expire();

      cityValve->setSpeed(255);
      valveRunLength.restart();
    }
    // turn pump on
    pump->setSpeed(255);
  }
  if (valveRunLength.isExpired()) {
    cityValve->setSpeed(0);
  }
#endif
  if (valveStateFeedMaxDelay.isExpired() && valveStateFeedMinDelay.isExpired() && mqtt.connected()) {
    valveStateFeed.publish( state.active_state == STATE_CITY ? "building-o" : "w:raindrops" );
    // don't reset valveStateFeedDelay, we don't need to periodically send this.
    valveStateFeedMinDelay.restart();
    valveStateFeedMaxDelay.restart();
  }
#endif
}

// centered in both x and y
void centerjustify(const char *str) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(display.getCursorX() - (w / 2), display.getCursorY() + (h / 2));
  display.print(str);
}

void rightjustify(const char *str) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(display.getCursorX() - w - 1, display.getCursorY());
  display.print(str);
}

void leftjustify(const char *str) {
    display.print(str);
}

void updateDisplay() {
  // transflective display update
  if (state_equal(&state, &last_displayed_state) && !displayMaxRefresh.isExpired()) {
    return; // don't update if nothing has changed
  }

  if (!displayMinRefresh.isExpired()) {
    return; // don't update too frequently
  }
  displayMinRefresh.restart();
  displayMaxRefresh.restart();
  last_displayed_state = state;

  boolean client_connected_recently = true;
#ifdef RAINGAUGE_V2
  client_connected_recently = state.connected_recently;
#endif

  display.clearDisplay();
  normalface();
  display.setTextColor(COLOR3);
  display.setCursor(4, 17);
  display.setTextSize(1);
  display.print(WiFi.localIP());
  display.setCursor(display.width() - 4, 17);
  rightjustify(MDNS_NAME ".local");

#define BAR_TOP 26
#define BAR_HEIGHT (53-26)
#define BAR_WIDTH (LCD_WIDTH-38)
#define BAR_SPACE (63-26)
  for (int i = 0; i < 2; i++) {
    int x = display.width() / 2 - (BAR_WIDTH / 2);
    int y = BAR_TOP + i * BAR_SPACE;
    int amount = state.water_level[i] * (BAR_WIDTH - 2) / 1000;
    display.setCursor(x - 4, y + (BAR_HEIGHT / 2) + 6);
    display.setTextColor(COLOR3);
    rightjustify(i == 0 ? "1:" : "2:");
    display.fillRect(x, y, BAR_WIDTH, BAR_HEIGHT, COLOR4);
    display.fillRect(x + 1, y + 1, BAR_WIDTH - 2, BAR_HEIGHT - 2, COLOR1);
#ifdef RAINPUMP_V2
    if (!state.connected_recently) {
      // no recent water level data
      continue;
    }
#endif
    display.fillRect(x + 1, y + 1, amount, BAR_HEIGHT - 2, COLOR2); // the actual graph amount
    display.setTextColor(COLOR4);
    display.setCursor(x + (BAR_WIDTH / 2), y + (BAR_HEIGHT / 2));
    boldface();
    char buf[20];
    snprintf(buf, 20, "%.0f%%", state.water_level[i] / 10.);
    centerjustify(buf);
    normalface();
  }

#define BUTTON_HEIGHT 28
  if (client_connected_recently) {
    display.fillRect(state.user_state * display.width() / 4, display.height() - BUTTON_HEIGHT,
                     display.width() / 4, BUTTON_HEIGHT, COLOR2);
  }
  display.drawFastHLine(0, display.height() - BUTTON_HEIGHT, display.width(), COLOR3);
  for (int i = 0; i < 3; i++) {
    display.drawFastVLine((i + 1)*display.width() / 4, display.height() - BUTTON_HEIGHT, BUTTON_HEIGHT, COLOR3);
  }
  for (int i = 0; i < 3; i++) {
    display.setCursor((2 * i + 1)*display.width() / 8, display.height() - BUTTON_HEIGHT / 2);
    if (!client_connected_recently) {
      display.setTextColor(COLOR2);
      normalface();
    } else if (i == state.user_state) {
      display.setTextColor(COLOR4);
      boldface();
    } else {
      if (i == state.active_state) {
        display.setTextColor(COLOR4);
      } else {
        display.setTextColor(COLOR3);
      }
      normalface();
    }
    centerjustify(i == 0 ? "CITY" : i == 1 ? "AUTO" : "RAIN" );
  }
#define BITMAP4(x, y, w, h, num) BITMAP4_(x,y,w,h,num) // need to expand the _NUM macro before pasting
#define BITMAP4_(x, y, w, h, num) do {\
    display.drawBitmap(x, y, LightBitmap ## num, w, h, COLOR2); \
    display.drawBitmap(x, y, DarkBitmap ## num, w, h, COLOR3); \
    display.drawBitmap(x, y, BlackBitmap ## num, w, h, COLOR4); \
  } while(false)

  if (state.user_state == STATE_AUTO && state.active_state == STATE_CITY && client_connected_recently) {
    BITMAP4(display.width() / 4 - LEFT_ARROW_WIDTH / 2,
            display.height() - BUTTON_HEIGHT / 2 - LEFT_ARROW_HEIGHT / 2,
            LEFT_ARROW_WIDTH,
            LEFT_ARROW_HEIGHT,
            LEFT_ARROW_NUM);
  }
  if (state.user_state == STATE_AUTO && state.active_state == STATE_RAIN && client_connected_recently) {
    BITMAP4(2 * display.width() / 4 - RIGHT_ARROW_WIDTH / 2,
            display.height() - BUTTON_HEIGHT / 2 - RIGHT_ARROW_HEIGHT / 2,
            RIGHT_ARROW_WIDTH,
            RIGHT_ARROW_HEIGHT,
            RIGHT_ARROW_NUM);
  }
  // pipe sensor status
#define PIPE_ICON_X (LCD_WIDTH - 47) // was 54
  if (client_connected_recently) {
    if (state.pipe_water_present) {
      BITMAP4(PIPE_ICON_X - DROP_ON_WIDTH / 2,
              display.height() - BUTTON_HEIGHT / 2 - DROP_ON_HEIGHT / 2,
              DROP_ON_WIDTH, DROP_ON_HEIGHT,
              DROP_ON_NUM);
    } else {
      BITMAP4(PIPE_ICON_X - DROP_OFF_WIDTH / 2,
              display.height() - BUTTON_HEIGHT / 2 - DROP_OFF_HEIGHT / 2,
              DROP_OFF_WIDTH, DROP_OFF_HEIGHT,
              DROP_OFF_NUM);
    }
  }
  // connection status
#define CONN_ICON_X (LCD_WIDTH - 18) // was 22
  if (state.connected_recently) {
    BITMAP4(CONN_ICON_X - CONN_ON_WIDTH / 2,
            display.height() - BUTTON_HEIGHT / 2 - CONN_ON_HEIGHT / 2,
            CONN_ON_WIDTH, CONN_ON_HEIGHT,
            CONN_ON_NUM);
  } else {
    BITMAP4(CONN_ICON_X - CONN_OFF_WIDTH / 2,
            display.height() - BUTTON_HEIGHT / 2 - CONN_OFF_HEIGHT / 2,
            CONN_OFF_WIDTH, CONN_OFF_HEIGHT,
            CONN_OFF_NUM);
  }

  display.display(); // or partial update?
}

void loop() {
  ArduinoOTA.handle();
  if (connectionWatchdog.isExpired()) {
    delay(5000);
    ESP.restart();
  }
#ifdef USE_MQTT
  if ((!mqtt.connected()) && mqttDelay.isExpired()) {
    int ret = mqtt.connect();
    if (ret != 0) {
        Serial.print("MQTT: ");
        Serial.println(mqtt.connectErrorString(ret));
    }
    mqtt.disconnect();
    mqttDelay.restart();
  }
#endif
  // Update user state via buttons
  ButtonPress button_press = lastButtonPress;
  bool buttons_changed = false;
  if (capReadInterval.isExpired()) { // don't clog up the I2C
      capReadInterval.restart();
#ifndef DISABLE_BUTTONS
      cap1298_read_buttons(&button_press);
      buttons_changed = !(button_press == lastButtonPress);
#if 0 // debugging
      if (buttons_changed) {
          Serial.println("Buttons pressed");
          button_press.dump();
          lastButtonPress.dump();
      }
#endif
      lastButtonPress = button_press;
#endif
  }

#ifdef RAINPUMP_V2
  server.handleClient();
  if (WiFi.isConnected()) {
    connectionWatchdog.restart();
  }
  // Update user state via buttons
  if (button_press.a) {
    state.user_state = STATE_CITY;
  } else if (button_press.b) {
    state.user_state = STATE_AUTO;
  } else if (button_press.c) {
    state.user_state = STATE_RAIN;
  } else if (button_press.d) {
    /* no function in server */
  }
  updateState(); // read various sensors
#endif
#ifdef RAINGAUGE_V2
  // read buttons, if pressed immediately sent a ?state= request to the server and expire lastPing()
  if (!buttons_changed) {
      updateState();
  } else if (button_press.a) {
    sendUpdate(STATE_CITY);
    displayMaxRefresh.expire();
  } else if (button_press.b) {
    sendUpdate(STATE_AUTO);
    displayMaxRefresh.expire();
  } else if (button_press.c) {
    sendUpdate(STATE_RAIN);
    displayMaxRefresh.expire();
  } else if (button_press.d) {
    lastPing.expire();
    displayMaxRefresh.expire(); // force refresh
  } else {
    updateState();
  }
#endif
  updateDisplay(); // possibly update display if state has changed
}

#endif /* RAINPUMP_V2 || RAINGAUGE_V2 */
