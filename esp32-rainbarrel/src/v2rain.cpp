#if defined(RAINPUMP_V2) || defined(RAINGAUGE_V2)
#if defined(RAINPUMP_V2) && defined(RAINGAUGE_V2)
# error Can not define both RAINPUMP_V2 and RAINGAUGE_V2, pick one!
#endif

#define USE_SNITCHMOTOR
#define FLOWMETER_CONNECTED
#define ADC_CONNECTED
#define USE_MQTT
#define HAS_ADP1650
#undef DEV_MODE // shorter delays for easier development

#include "Arduino.h"
#include <Wire.h>
#include <SPI.h> // this is done to help pio discover this dependency
#include <AsyncDelay.h>

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>
#ifdef RAINPUMP_V2
# include <WebServer.h>
#endif
#include <ArduinoOTA.h>
#include <Adafruit_GFX.h>

#include <HTTPClient.h>
#ifdef USE_MQTT
# include <ArduinoHA.h>
#endif
#include <ArduinoJson.h>
#include <AsyncDelay.h>
#include "config.h"
#include "bluehigh-10px.h"
#include "bluebold-11px.h"
#include "icons.h"

// V2 hardware features
#include "st7529.h"
#include "cap1298.h"
#include "adp1650.h"
// V2 pin assignments
#if defined(ARDUINO_FEATHER_ESP32)
# define LED_GPIO       13
# define LCD_RST        14      // F0
# define PRESSURE_SW    32      // F1 (pressure switch input)
# define PUMP_CNTRL     15      // F2 (power tail out)
# define LCD_CS         33      // F3
# define WATER_SENSE    27      // F4 (water present)
// Be careful, F5 has a pull-down and must be left alone at boot.
# define LCD_SI         18 // MOSI
# define LCD_SCL         5 // SCK
#else
# error Unknown pin assignments
#endif

#ifdef RAINPUMP_V2
# include "flowmeter.h"
# include "smrtysnitch.h"
# include "smrty_decode.h"
bool setSnitchGPIO(uint8_t which, uint8_t level);
#endif
#ifdef RAINGAUGE_V2
# ifdef ADC_CONNECTED
#  include <Adafruit_ADS1X15.h>
# endif
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
#define FLOW_TO_GALLONS(oldFlow, newFlow) \
  ((newFlow - oldFlow) / (double)TICKS_PER_GALLON)

#define NUM_FLOWMETERS 3
#define FOREACH_FLOWMETER(x) x x x
#define FOREACH_FLOWMETER_ARG(x) x(0) x(1) x(2)
#define FOREACH_FLOWMETER_ARG2(x) x(0,1) x(1,2) x(2,3)

#define COMMA ,

enum PumpCntrl { PUMP_OFF = 0, PUMP_ON = 1 };

#define COLOR1 0x00 // was: EPD_WHITE
#define COLOR2 0x7F // was: EPD_LIGHT
#define COLOR3 0xBF // was: EPD_DARK
#define COLOR4 0xFF // was: EPD_BLACK

// General note: Most (or all?) of the intervals below have small offsets added
// to make the interval a prime # of milliseconds.  This ensures that intervals
// will tend to space themselves out over time and not all pile up at the same
// millisecond boundary.
#define SECONDS_MS (1000)
#define MINUTES_MS (60 * SECONDS_MS)
#define HOURS_MS (60 * MINUTES_MS)

#define LCD_WIDTH 240
#define LCD_HEIGHT 128
ST7529_LCD display = ST7529_LCD(LCD_WIDTH, LCD_HEIGHT, LCD_RST, LCD_CS, LCD_SCL, LCD_SI);
bool adp1650_found = false;

AsyncDelay displayMaxRefresh = AsyncDelay(4 * HOURS_MS + 11, AsyncDelay::MILLIS); // 6x a day need it or not
AsyncDelay displayMinRefresh = AsyncDelay(13, AsyncDelay::MILLIS); // no more than once/10ms seconds
AsyncDelay backlightDelay = AsyncDelay(1 * MINUTES_MS, AsyncDelay::MILLIS); // backlight on for a minute if triggered

enum DisplayState { DISPLAY_MAIN, DISPLAY_CONFIG } displayState = DISPLAY_MAIN;
AsyncDelay configDisplayTimeout = AsyncDelay(30 * SECONDS_MS, AsyncDelay::MILLIS);
AsyncDelay configDisplayKeyRepeat = AsyncDelay(1500, AsyncDelay::MILLIS);

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
AsyncDelay capReadInterval = AsyncDelay(50 + 3, AsyncDelay::MILLIS); // read @ 20Hz

WiFiClient client; // change to WiFiClientSecure for TLS support
#ifdef USE_MQTT
HADevice ha_device;
HAMqtt ha_mqtt(client, ha_device, 10/*maximum entities*/);
# define MQTT_HOST IPAddress(192,168,198,32)
# define MQTT_USER "mqtt103"
# define MQTT_PASS "mqtt103"
#endif

/****************************** Feeds ***************************************/

#ifdef USE_MQTT

#ifdef RAINPUMP_V2
// Update valve state every ~5 minutes; this is the MQTT keepalive.
AsyncDelay valveStateFeedMaxDelay = AsyncDelay(5*MINUTES_MS - 1019, AsyncDelay::MILLIS);
AsyncDelay valveStateFeedMinDelay = AsyncDelay(1*SECONDS_MS + 13, AsyncDelay::MILLIS); // not more than 1/second
HABinarySensor ha_valve_state_sensor("valve_state");
HASelect ha_valve_state_select("valve_select");
HABinarySensor ha_pipe_water_sensor("pipe_water_present");

AsyncDelay flowMeterInterval = AsyncDelay(1*MINUTES_MS - 1, AsyncDelay::MILLIS);
AsyncDelay flowMeterIntervalMax = AsyncDelay(1 * HOURS_MS - 7, AsyncDelay::MILLIS);
#define FLOWMETER_SENSOR(x,y)                                    \
  HASensorNumber("flowmeter" #y, HASensorNumber::PrecisionP3),
HASensorNumber ha_flow_meter[NUM_FLOWMETERS] = {
  FOREACH_FLOWMETER_ARG2(FLOWMETER_SENSOR)
};

HATagScanner ha_smrty_raw("smrty_raw");
HASensorNumber ha_smrty_moisture("smrty_moisture", HASensorNumber::PrecisionP1);
HASensorNumber ha_smrty_temperature("smrty_temp", HASensorNumber::PrecisionP1);

HABinarySensor ha_pressure_sensor("pressure_sensor");

#endif /* RAINPUMP_V2 */

#ifdef RAINGAUGE_V2
// 11s so they don't hit at the same time as the SMRT-Y poll, which is 9s
AsyncDelay waterLevelFeedDelay = AsyncDelay(11*SECONDS_MS + 3, AsyncDelay::MILLIS); // every ~10s
HASensorNumber ha_water_level1("waterlevel1", HASensorNumber::PrecisionP1);
HASensorNumber ha_water_level1_raw("waterlevel1_raw", HASensorNumber::PrecisionP0);
HASensorNumber ha_water_level2("waterlevel2", HASensorNumber::PrecisionP1);
HASensorNumber ha_water_level2_raw("waterlevel2_raw", HASensorNumber::PrecisionP0);
#endif /* RAINGAUGE_V2 */
#endif /* USE_MQTT */

#ifdef RAINPUMP_V2
# define MDNS_NAME SERVER_MDNS_NAME
#endif
#ifdef RAINGAUGE_V2
# define MDNS_NAME CLIENT_MDNS_NAME
#endif

AsyncDelay connectionWatchdog = AsyncDelay(5*MINUTES_MS + 7, AsyncDelay::MILLIS);

#ifdef RAINPUMP_V2
// Motorshield configuration
#ifdef USE_SNITCHMOTOR
# define CITY_SNITCH_GPIO 1
# define RAIN_SNITCH_GPIO 0
#endif

WebServer server(SERVER_PORT);
AsyncDelay lastPing = AsyncDelay(3 * KEEPALIVE_INTERVAL_SECS * SECONDS_MS  + 1, AsyncDelay::MILLIS);
// The valve seems to get hot if you keep it activated -- though it should not!  Anyway, only
// run the valve for a minute when it changes state.
int lastValveState = -1;
AsyncDelay valveRunLength = AsyncDelay(1 * MINUTES_MS + 13, AsyncDelay::MILLIS);

// This interval is offset just a smidge because we ideally want to bin the
// flow per minute. But being .002% too high shouldn't matter.
// But do try to generate at least 1 data point per day
uint64_t lastFlowMeterReading[] = { FOREACH_FLOWMETER(0 COMMA) };
bool lastFlowMeterReadingValid[] = { FOREACH_FLOWMETER(false COMMA) };

// This is polling delay; it is also minimum MQTT publish interval so we don't
// get throttled.
// 9s so they don't hit at the same time as the water level poll, which is 11s
AsyncDelay smrtyInterval = AsyncDelay(9 * SECONDS_MS + 1, AsyncDelay::MILLIS);
uint8_t smrtyLastSeqno = 0xFF;
#endif

#ifdef RAINGAUGE_V2
#ifdef ADC_CONNECTED
Adafruit_ADS1115 ads1115;
#endif
AsyncDelay lastLevelReading = AsyncDelay(100, AsyncDelay::MILLIS);
AsyncDelay lastPing = AsyncDelay(KEEPALIVE_INTERVAL_SECS * SECONDS_MS - 1, AsyncDelay::MILLIS);
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

#if defined(USE_MQTT) && defined(RAINPUMP_V2)
void ha_set_valve_state_sensor(bool initial = false) {
  bool value = (state.active_state != STATE_CITY);
  /* Can't dynamically update icon, alas. */
#if 0
  if (value) {
    ha_valve_state_sensor.setIcon("mdi:weather-rainy");
  } else {
    ha_valve_state_sensor.setIcon("mdi:city");
  }
#endif
  if (initial) {
    ha_valve_state_sensor.setCurrentState(value);
  } else {
    ha_valve_state_sensor.setState(value);
  }
}

void ha_set_valve_state_select(bool initial = false) {
  int8_t value;
  switch (state.user_state) {
  case STATE_CITY:
    value = 0;
    break;
  default:
  case STATE_AUTO:
    value = 1;
    break;
  case STATE_RAIN:
    value = 2;
    break;
  }
  if (initial) {
    ha_valve_state_select.setCurrentState(value);
  } else {
    ha_valve_state_select.setState(value);
  }
}

void ha_valve_state_select_command(int8_t index, HASelect *sender) {
  switch(index) {
  case 0:
    state.user_state = STATE_CITY;
    break;
  default:
  case 1:
    state.user_state = STATE_AUTO;
    break;
  case 2:
    state.user_state = STATE_RAIN;
    break;
  }
  ha_set_valve_state_select(); // report back to HA
  updateState(); // adjust various valves
}
#endif

void smallface() {
  display.setFont();
  display.setTextSize(1);
}

void normalface() {
  display.setFont(&bluehigh10pt7b);
  display.setTextSize(1);
}

void boldface() {
  display.setFont(&bluebold11pt7b);
  display.setTextSize(1);
}

#ifdef RAINPUMP_V2
// Web server
void handleRoot() {
  char temp[850];
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;

  // Sum flows!
  uint64_t total_flow = 0;
  bool saw_good_reading = false;
  for (int i=0; i<NUM_FLOWMETERS; i++) {
    if (lastFlowMeterReadingValid[i]) {
      total_flow += lastFlowMeterReading[i];
      saw_good_reading = true;
    }
  }

  snprintf(temp, 850,

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
"    <p>Total flow: %.1lf gallons (raw count: %ld)</p>\n"
"    <p>Pressure Switch: %s</p>\n"
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
           state.connected_recently ? "Yes" : "No",
           (double) (FLOW_TO_GALLONS(0, total_flow)),
           (long) (saw_good_reading ? total_flow : -1),
           digitalRead(PRESSURE_SW) ? "OPEN" : "CLOSED"
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

bool readSmrtySnitch(uint8_t which, uint8_t *seqno, struct smrty_msg *msg, uint8_t *good_checksum);
bool pollSmrtySnitch();

void handleTest() {
  char temp[800];
  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "level1") {
    }
  }
  uint8_t seqno;
  struct smrty_msg msg;
  uint8_t good_checksum;
  bool st = 1 ? pollSmrtySnitch() :
      readSmrtySnitch(0xC0, &seqno, &msg, &good_checksum);
  const char *mqtt_status = "N/A";
  snprintf(temp, 800, "<html><body><pre>\n"
           "Return value: %s\n"
           "[%02X] %02X %02X %02X %02X | %02X %02X %02X | %02X%s\n"
           "</pre>"
           "<p>MQTT: %s</p>"
           "</body></html>",
           st ? "true" : "false",
           (int)seqno,
           msg.addr, msg.cmd, msg.tx_data1, msg.tx_data2,
           msg.rx_data1, msg.rx_data2, msg.status,
           msg.checksum, good_checksum ? "":"*",
           mqtt_status
           );
  // Output
  server.send(200, "text/html", temp);
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

bool readFlowMeter(uint64_t *result, uint8_t which) {
    uint64_t count = 0;
#ifdef FLOWMETER_CONNECTED
    uint8_t nBytes = Wire.requestFrom(FLOWMETER_I2C_ADDR_BASE + which, 8);
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

void setPumpCntrl(enum PumpCntrl is_on) {
  // if we turn pump ON and then PRESSURE_SW turns it off (ie is HIGH)
  // less than XX seconds later, then leave pump off for YY seconds before
  // trying to turn the pump on again. (exponential backoff on YY?)
    digitalWrite(PUMP_CNTRL, !is_on); // active low
}

void setValve(enum PumpState state) {
    switch (state) {
    case STATE_CITY:
#ifdef USE_SNITCHMOTOR
        setSnitchGPIO(RAIN_SNITCH_GPIO, 0);
        setSnitchGPIO(CITY_SNITCH_GPIO, 1);
#endif
        break;
    case STATE_RAIN:
#ifdef USE_SNITCHMOTOR
        setSnitchGPIO(CITY_SNITCH_GPIO, 0);
        setSnitchGPIO(RAIN_SNITCH_GPIO, 1);
#endif
        break;
    }
}

void idleValve() {
#ifdef USE_SNITCHMOTOR
    setSnitchGPIO(CITY_SNITCH_GPIO, 0);
    setSnitchGPIO(RAIN_SNITCH_GPIO, 0);
#endif
}

bool readSmrtySnitch(uint8_t which, uint8_t *seqno, struct smrty_msg *msg, uint8_t *good_checksum) {
    Wire.beginTransmission(SNITCH_I2C_ADDR);
    Wire.write(which);
    uint8_t status = Wire.endTransmission();
    uint8_t tries = 0;
    if (status != 0) {
        return false; // no response from client, must be disconnected
    }
    uint8_t reg, nBytes;
#define FLUSH(readSoFar)                                \
    do {                                                \
        for (nBytes-=readSoFar; nBytes != 0; nBytes--) {  \
            Wire.read();                                \
        }                                               \
    } while (false)
    while (true) {
        nBytes = Wire.requestFrom(SNITCH_I2C_ADDR, 10);
        if (nBytes == 0) {
            return false; /* unexpected! */
        }
        reg = Wire.read();
        if (reg == which) {
            break; // reg write is done!
        }
        FLUSH(1);
        // busy loop until reg. write is done, should be quick
        if ((++tries) > 3) {
            return false; // but don't get stuck here forever
        }
    }
    if (nBytes < 10) {
         /* incomplete read for some reason */
        FLUSH(1);
        return false;
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
    FLUSH(10);
    return true;
}

bool setSnitchGPIO(uint8_t which, uint8_t level) {
    uint8_t seqno;
    struct smrty_msg msg;
    uint8_t good_checksum;
    uint8_t cmd = (0xC0) | ((which & 0x1F) << 1) | (level & 1);
    return readSmrtySnitch(cmd, &seqno, &msg, &good_checksum);
}

void publishSmrty(uint8_t seqno, struct smrty_msg *msg, bool good_checksum) {
#ifdef USE_MQTT
    const char *fmt = "[%02X] %02X %02X %02X %02X | %02X %02X %02X | %02X%s";
    char buf[strlen(fmt)+1]; // string will be shorter than this
    snprintf(buf, strlen(fmt)+1, fmt, seqno,
             msg->addr, msg->cmd, msg->tx_data1, msg->tx_data2,
             msg->rx_data1, msg->rx_data2, msg->status,
             msg->checksum, good_checksum ? "":"*");
    ha_smrty_raw.tagScanned(buf);
    if (!good_checksum) { return; }
    // Do our best to decode these values.
    uint16_t tx_data = ((uint16_t)msg->tx_data2)<<8 | msg->tx_data1;
    uint16_t rx_data = ((uint16_t)msg->rx_data2)<<8 | msg->rx_data1;
    switch (tx_data) {
    case 0x000B:
      ha_smrty_moisture.setValue(static_cast<float>(
        ((float)rx_data)/100.0
      ));
      break;
    case 0x0005:
      // This could use more points to do a proper line fit
      // (This one done at https://mycurvefit.com/)
      ha_smrty_temperature.setValue(static_cast<float>(
        33.1877 + (rx_data*.1089572)
      ));
      break;
    case 0x000E:
      // This is electrical conductivity, but it's always 0.0, so ignore it.
      break;
    default:
      break;
    }
#endif /* USE_MQTT */
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
#if 0
        return true; // This ensures that we don't get throttled by Adafruit IO
#endif
    }
    return true;
}

#endif

bool adp1650_write(uint8_t regno, uint8_t val) {
    Wire.beginTransmission(ADP1650_I2C_ADDR);
    Wire.write(regno);
    Wire.write(val);
    uint8_t status = Wire.endTransmission();
    if (status != 0) {
        return false;
    }
    return true;
}

bool adp1650_read(uint8_t regno, uint8_t *val) {
    Wire.beginTransmission(ADP1650_I2C_ADDR);
    Wire.write(regno);
    uint8_t status = Wire.endTransmission();
    if (status != 0) {
        return false;
    }
    uint8_t nBytes = Wire.requestFrom(ADP1650_I2C_ADDR, 1);
    if (nBytes == 0) {
        return false;
    }
    *val = Wire.read();
    return true;
}

bool adp1650_set_backlight(uint8_t level) {
    if (!adp1650_found) {
        return false;
    }
    // clamp level
    uint8_t current =
        (level > 3) ? ADP1650_I_TOR_125MA :
        (level > 2) ? ADP1650_I_TOR_100MA :
        ADP1650_I_TOR_25MA;
    adp1650_write( ADP1650_REG_CURRENT_SET, current );
    uint8_t output_mode = ADP1650_FREQ_FB;
    if (level > 0) {
        output_mode |= ADP1650_OUTPUT_EN | ADP1650_LED_MOD_ASSIST;
    } else {
        output_mode |= ADP1650_LED_MOD_STANDBY;
    }
    adp1650_write( ADP1650_REG_OUTPUT_MODE, output_mode );
    return true;
}

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
    pinMode(PRESSURE_SW, INPUT);
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
    WiFi.setSleep(false);

    Serial.println("cap1298 setup");
    cap1298_setup();

#ifdef RAINPUMP_V2
    idleValve(); // this will handle the SNITCHMOTOR case too
    setPumpCntrl(PUMP_OFF);
#endif

#ifdef RAINGAUGE_V2
#ifdef ADC_CONNECTED
    Serial.println("ADC setup");
    ads1115.begin(0x48); // At default address
    ads1115.setGain(GAIN_TWO); // +/-2.048V
#endif
#endif

    adp1650_found = false;
#ifdef HAS_ADP1650
    uint8_t val;
    if (adp1650_read(ADP1650_REG_DESIGN_INFO, &val) && val == 0x22) {
        adp1650_found = true;
    }
#endif
    Serial.println("Display setup");
    // XXX be extra safe and don't try to dynamically detect the adp1650
    // because the consequences of turning on the internal booster when
    // we've got an external supply set up are sufficiently dire.
    display.begin(false /* !adp1650_found*/);
#ifdef RAINGAUGE_V2
    display.setContrast(83); // different default contrast for outdoors
#endif
    Serial.println(ESP.getFreeHeap(),DEC);
    display.clearDisplay();
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
    adp1650_set_backlight(2); // turn on backlight while booting

    Serial.println("Booting");

    // Wait for connection
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        Serial.println("Connection Failed! Rebooting...");
        delay(5000);
        ESP.restart();
    }
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

#ifdef USE_MQTT
    byte mac[6];
    WiFi.macAddress(mac);
    ha_device.setUniqueId(mac, sizeof(mac));
    ha_device.setSoftwareVersion("1.0.0");
    ha_device.setManufacturer("C. Scott Ananian");
    ha_device.enableSharedAvailability();
    ha_device.enableLastWill();
#ifdef RAINPUMP_V2
    ha_device.setName("Rain Pump");
    ha_device.setModel("Rain Pump");
    //ha_device.setIcon("mdi:water-pump");

    ha_valve_state_sensor.setName("Using Rain Water");
    ha_valve_state_sensor.setIcon("mdi:weather-rainy");
    ha_set_valve_state_sensor(true);

    ha_valve_state_select.setName("Water Source");
    ha_valve_state_select.setOptions("City;Auto;Rain");
    ha_valve_state_select.setRetain(true);
    ha_valve_state_select.onCommand(ha_valve_state_select_command);
    ha_set_valve_state_select(true);

    ha_pipe_water_sensor.setName("Pipe Water Present");
    ha_pipe_water_sensor.setDeviceClass("moisture");

#define FLOWMETER_NAME(i,j)                              \
    ha_flow_meter[i].setName("Irrigation Flow " #j);
    FOREACH_FLOWMETER_ARG2(FLOWMETER_NAME);

    for(int i=0; i<NUM_FLOWMETERS; i++) {
      ha_flow_meter[i].setDeviceClass("water");
      ha_flow_meter[i].setUnitOfMeasurement("gal");
      ha_flow_meter[i].setStateClass("total_increasing");
    }

    ha_smrty_raw.setName("SMRTY Raw Reads");

    ha_smrty_moisture.setName("Soil Moisture");
    ha_smrty_moisture.setUnitOfMeasurement("%");
    ha_smrty_moisture.setDeviceClass("moisture");
    ha_smrty_moisture.setStateClass("measurement");

    ha_smrty_temperature.setName("Soil Temperature");
    ha_smrty_temperature.setUnitOfMeasurement("°F");
    ha_smrty_temperature.setDeviceClass("temperature");
    ha_smrty_temperature.setStateClass("measurement");

    ha_pressure_sensor.setName("Pump Running");
    ha_pressure_sensor.setDeviceClass("running");
#endif

#ifdef RAINGAUGE_V2
    ha_device.setName("Rain Gauge");
    ha_device.setModel("Rain Gauge");
    //ha_device.setIcon("mdi:gauge");
    ha_water_level1.setName("Water Level Barrel 1");
    ha_water_level2.setName("Water Level Barrel 2");
    ha_water_level1.setDeviceClass("volume_storage");
    ha_water_level2.setDeviceClass("volume_storage");
    ha_water_level1.setUnitOfMeasurement("%");
    ha_water_level2.setUnitOfMeasurement("%");
    ha_water_level1.setIcon("mdi:gauge");
    ha_water_level2.setIcon("mdi:gauge");
    ha_water_level1.setStateClass("measurement");
    ha_water_level2.setStateClass("measurement");
    ha_water_level1_raw.setName("Water Level Barrel 1 (raw)");
    ha_water_level2_raw.setName("Water Level Barrel 2 (raw)");
    ha_water_level1_raw.setDeviceClass("volume_storage");
    ha_water_level2_raw.setDeviceClass("volume_storage");
    //ha_water_level1_raw.setUnitOfMeasurement("counts");
    //ha_water_level2_raw.setUnitOfMeasurement("counts");
    ha_water_level1_raw.setIcon("mdi:gauge");
    ha_water_level2_raw.setIcon("mdi:gauge");
    ha_water_level1_raw.setStateClass("measurement");
    ha_water_level2_raw.setStateClass("measurement");
#endif

    ha_mqtt.begin(MQTT_HOST, MQTT_USER, MQTT_PASS);
#endif

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
    server.on("/test", handleTest);
    server.onNotFound(handleNotFound);
    server.begin();
    MDNS.addService("http", "tcp", SERVER_PORT);
    Serial.println("HTTP server started");
    valveRunLength.restart(); // run the valve to move it
    for (int i=0; i<NUM_FLOWMETERS; i++) {
      lastFlowMeterReadingValid[i] = readFlowMeter(&(lastFlowMeterReading[i]), i);
    }
    flowMeterInterval.restart(); // next read the flow meter in a minute
    flowMeterIntervalMax.expire(); // report the first reading regardless
#endif

    Serial.println("Ready");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    displayMaxRefresh.expire();
    displayMinRefresh.expire();
    backlightDelay.expire();
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
    // periodically send levels to Home Assistant
    if (waterLevelFeedDelay.isExpired()) {
      // percentages
      float percent;
      percent = state.water_level[0] / (float)10;
      ha_water_level1.setValue(static_cast<float>(percent));
      percent = state.water_level[1] / (float)10;
      ha_water_level2.setValue(static_cast<float>(percent));
      // raw levels, for calibration purposes
      ha_water_level1_raw.setValue(raw_level[0]);
      ha_water_level2_raw.setValue(raw_level[1]);
      // wait for next time
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
    bool force_update = false;
    if (flowMeterIntervalMax.isExpired()) {
      force_update = true;
      flowMeterIntervalMax.repeat();
    }
    for (int i=0; i<NUM_FLOWMETERS; i++) {
      uint64_t newFlow;
      bool valid = readFlowMeter(&newFlow, i);
      bool should_update = force_update;
      if (!valid) {
        lastFlowMeterReadingValid[i] = false;
        should_update = false;
      } else if (!lastFlowMeterReadingValid[i]) {
        lastFlowMeterReading[i] = newFlow;
        lastFlowMeterReadingValid[i] = true;
        should_update = true;
      } else if (newFlow != lastFlowMeterReading[i]) {
        // don't fill the log with a lot of unnecessary zeroes
        lastFlowMeterReading[i] = newFlow;
        should_update = true;
      }
#ifdef USE_MQTT
      if (should_update) {
        double gallons = FLOW_TO_GALLONS(0, newFlow);
        ha_flow_meter[i].setValue((float)gallons);
      }
#endif
    }
  }
  // read the SMRTY snitch at interval
#ifdef USE_MQTT
  if (smrtyInterval.isExpired()) {
      smrtyInterval.restart();
      pollSmrtySnitch();
  }
#endif
  // read our pipe water sensor
  state.pipe_water_present = !digitalRead(WATER_SENSE);
#ifdef USE_MQTT
  ha_pipe_water_sensor.setState(state.pipe_water_present);
#endif

  // implement "auto" mode
  if (state.user_state != STATE_AUTO) {
    // force a specific state
    state.active_state = state.user_state;
  } else {
    boolean is_rain_empty = !state.pipe_water_present;
    // This can get stuck when the pipe_water_present is dry but the
    // rain barrels have filled up again.  So we should ignore
    // pipe_water_present if the rain barrel level is above like 33%
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

  // ok, now adjust settings to match selected state
  static bool not_idled = true;
  if (lastValveState != state.active_state) {
    lastValveState = state.active_state;
#ifdef USE_MQTT
    valveStateFeedMaxDelay.expire();
#endif /* USE_MQTT */
    valveRunLength.restart();
    setPumpCntrl((state.active_state == STATE_CITY) ? PUMP_OFF : PUMP_ON);
    setValve((enum PumpState)state.active_state);
    not_idled = true;
  } else if (not_idled && valveRunLength.isExpired()) {
    idleValve();
    not_idled = false;
  }

#ifdef USE_MQTT
  if (valveStateFeedMaxDelay.isExpired() && valveStateFeedMinDelay.isExpired()) {
    ha_set_valve_state_sensor();
    // don't reset valveStateFeedDelay, we don't need to periodically send this.
    valveStateFeedMinDelay.restart();
    valveStateFeedMaxDelay.restart();
  }
  ha_set_valve_state_select(); // update if necessary
#endif /* USE_MQTT */
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

static uint8_t config_display_menu_selected = 0;

void switchToConfigDisplay() {
    displayState = DISPLAY_CONFIG;
    displayMaxRefresh.expire();
    configDisplayTimeout.restart();
    configDisplayKeyRepeat.restart();
    config_display_menu_selected = 0;
}

void maybeSwitchToMainDisplay() {
  if (displayState == DISPLAY_CONFIG && configDisplayTimeout.isExpired()) {
    displayState = DISPLAY_MAIN;
    displayMaxRefresh.expire();
  }
}

void updateConfigDisplay() {
#define CONFIG_COLUMN 25
#define CONFIG_TOP 38
#define CONFIG_LINEHEIGHT 14
  const char *menuItems[] = { "Increase Contrast", "Decrease Contrast", "Exit" };
  const uint8_t num_items = (sizeof(menuItems)/sizeof(*menuItems));
  char buf[10];

  if (config_display_menu_selected >= num_items) {
    config_display_menu_selected = 0;
  }

  normalface();

  for (int i=0; i < num_items; i++) {
    bool selected = (config_display_menu_selected == i);
    display.setCursor(CONFIG_COLUMN, CONFIG_TOP + i*CONFIG_LINEHEIGHT);
    display.setTextColor(selected ? COLOR3 : COLOR2);
    display.println(menuItems[i]);
    display.setCursor(CONFIG_COLUMN, CONFIG_TOP + i*CONFIG_LINEHEIGHT);
    snprintf(buf, 10, "%d. ", (i+1));
    rightjustify(buf);
  }
  display.setCursor(2 /* tweak */,
                    2 /* tweak */ + CONFIG_TOP + config_display_menu_selected * CONFIG_LINEHEIGHT);
  boldface();
  display.setTextColor(COLOR3);
  display.print("*");

  smallface();
  display.setCursor(0, CONFIG_TOP + (num_items)*CONFIG_LINEHEIGHT + 4);
  display.setTextColor(COLOR2);
  display.print("Cntrst:");
  display.print(display.getContrast());
#ifdef RAINGAUGE_V2
  for (int i=0; i<2; i++) {
      display.print(" Level "); display.print(i); display.print(": ");
      display.print(ads1115.readADC_SingleEnded(i));
  }
#endif
#ifdef RAINPUMP_V2
  display.print(" Flow:");
  uint64_t total_flow = 0;
  bool saw_good_reading = false;
  for (int i=0; i<NUM_FLOWMETERS; i++) {
    lastFlowMeterReadingValid[i] = readFlowMeter(&(lastFlowMeterReading[i]), i);
    if (lastFlowMeterReadingValid[i]) {
      total_flow += lastFlowMeterReading[i];
      saw_good_reading = true;
    }
  }
  if (saw_good_reading) {
    display.print(total_flow);
  } else {
    display.print("-");
  }
  display.print(" Press:");
  display.print(digitalRead(PRESSURE_SW)?"OP":"CL");
  display.print(" SMRTY:");
  snprintf(buf, 10, "%02X", smrtyLastSeqno);
  display.print(buf);
#endif
}

void handleConfigDisplayButtons(ButtonPress button_press) {
  if (!button_press.a && !button_press.b && !button_press.c && !button_press.d){
    return;
  }
  configDisplayTimeout.restart();
  configDisplayKeyRepeat.start(250, AsyncDelay::MILLIS); // quick repeat
  displayMaxRefresh.expire();
  if (button_press.d) {
    displayState = DISPLAY_MAIN;
  } else if (button_press.a) {
    if (config_display_menu_selected > 0) {
      config_display_menu_selected--;
    }
  } else if (button_press.b) {
    config_display_menu_selected++;
  } else if (button_press.c) {
    switch (config_display_menu_selected) {
    case 0:
      display.setContrast(display.getContrast() + 1);
      break;
    case 1:
      display.setContrast(display.getContrast() - 1);
      break;
    case 2:
      displayState = DISPLAY_MAIN;
      break;
    default:
      break;
    }
  }
}

void updateDisplay() {
  // transflective display update
  if (displayState == DISPLAY_MAIN &&
      state_equal(&state, &last_displayed_state) &&
      !displayMaxRefresh.isExpired()) {
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
  display.print(WiFi.localIP());
  display.setCursor(display.width() - 4, 17);
  rightjustify(MDNS_NAME ".local");

#define BAR_TOP 26
#define BAR_HEIGHT (53-26)
#define BAR_WIDTH (LCD_WIDTH-38)
#define BAR_SPACE (63-26)
#define BUTTON_HEIGHT 28

  if (displayState == DISPLAY_CONFIG) {
    // Configuration screen!
    updateConfigDisplay();
    // Draw button guide
    display.drawFastHLine(0, display.height() - BUTTON_HEIGHT, display.width(), COLOR3);
    for (int i = 0; i < 3; i++) {
      display.drawFastVLine((i + 1)*display.width() / 4, display.height() - BUTTON_HEIGHT, BUTTON_HEIGHT, COLOR3);
    }
    for (int i = 0; i < 4; i++) {
      display.setCursor((2 * i + 1)*display.width() / 8, display.height() - BUTTON_HEIGHT / 2);
      display.setTextColor(COLOR4);
      normalface();
      centerjustify(i == 0 ? "UP" : i == 1 ? "DOWN" : i == 2 ? "OK" : "BACK" );
    }
    display.display(); // or partial update?
    return;
  }

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
    delay(5*SECONDS_MS);
    ESP.restart();
  }
#ifdef USE_MQTT
#ifdef RAINPUMP_V2
  // Hack in ha_pressure_sensor reading, which doesn't have a
  // proper polling loop
  ha_pressure_sensor.setState(!digitalRead(PRESSURE_SW));
#endif
  ha_mqtt.loop();
#endif /* USE_MQTT */
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
  if (buttons_changed) {
    // the first repeat delay is longer than subsequent.
    configDisplayKeyRepeat.start(2*SECONDS_MS, AsyncDelay::MILLIS);
  }
  if (button_press.prox) {
      backlightDelay.restart();
  }
  // someday we could use an ambient light sensor here?
  adp1650_set_backlight(backlightDelay.isExpired() ? 1 : 3);

#ifdef RAINPUMP_V2
  server.handleClient();
  if (WiFi.isConnected()) {
    connectionWatchdog.restart();
  }
  // Update user state via buttons
  if (displayState == DISPLAY_CONFIG) {
    if (buttons_changed || configDisplayKeyRepeat.isExpired()) {
      handleConfigDisplayButtons(button_press);
    }
    maybeSwitchToMainDisplay();
  } else if (button_press.a) {
    state.user_state = STATE_CITY;
  } else if (button_press.b) {
    state.user_state = STATE_AUTO;
  } else if (button_press.c) {
    state.user_state = STATE_RAIN;
  } else if (button_press.d) {
    if (buttons_changed) {
      switchToConfigDisplay(); // Switch to configuration mode
    }
  }
  updateState(); // read various sensors
#endif
#ifdef RAINGAUGE_V2
  // read buttons, if pressed immediately sent a ?state= request to the server and expire lastPing()
  if (displayState == DISPLAY_CONFIG) {
    if (buttons_changed || configDisplayKeyRepeat.isExpired()) {
      handleConfigDisplayButtons(button_press);
    } else {
      updateState();
    }
    maybeSwitchToMainDisplay();
  } else if (!buttons_changed) {
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
    // Force-send an update to the server
    lastPing.expire();
    switchToConfigDisplay(); // Switch to configuration mode
  } else {
    updateState();
  }
#endif
  updateDisplay(); // possibly update display if state has changed
}

#endif /* RAINPUMP_V2 || RAINGAUGE_V2 */
