#if defined(RAIN_SERVER) || defined(RAIN_CLIENT)
#if defined(RAIN_SERVER) && defined(RAIN_CLIENT)
# error Can not define both RAIN_SERVER and RAIN_CLIENT, pick one!
#endif

#define MOTOR_DRIVER_CONNECTED
#undef DEV_MODE // shorter delays for easier development

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ThinkInk.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>
#include <ArduinoJson.h>
#include <AsyncDelay.h>
#include "config.h"
#include "bluehigh-12px.h"
#include "bluebold-14px.h"
#include "icons.h"

#ifdef RAIN_SERVER
# include <Adafruit_MotorShield.h>
# define ENABLE_AUDIO
#endif
#ifdef RAIN_CLIENT
# include <HTTPClient.h>
# include "MPR121.h"
# include <Adafruit_ADS1X15.h>
#endif

#ifdef ENABLE_AUDIO
#include "coin.h"
#endif

#define SERVER_MDNS_NAME "rainpump"
#define CLIENT_MDNS_NAME "raingauge"
#define WATER_ALARM_LOW 100 /* 10% */
#define KEEPALIVE_INTERVAL_SECS 60
#define LEVEL_SENSOR_INTERVAL_SECS 1
#define SERVER_PORT 80
#define WATER_READING_ZERO 0
// rain barrel 1 max observed 17194 rain barrel 2 got up to 17414
#define WATER_READING_FULL 17200

#define EPD_DC      7 // can be any pin, but required!
#define EPD_CS      8  // can be any pin, but required!
#define EPD_BUSY    -1  // can set to -1 to not use a pin (will wait a fixed delay)
#define SRAM_CS     -1  // can set to -1 to not use a pin (uses a lot of RAM!)
#define EPD_RESET   6  // can set to -1 and share with chip Reset (can't deep sleep)
#define COLOR1 EPD_BLACK
#define COLOR2 EPD_LIGHT
#define COLOR3 EPD_DARK

Adafruit_NeoPixel pixels = Adafruit_NeoPixel(4, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
// 2.9in Grayscale Featherwing or Breakout:
ThinkInk_290_Grayscale4_T5 display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
AsyncDelay displayMaxRefresh = AsyncDelay(4 * 60 * 60 * 1000, AsyncDelay::MILLIS); // 6x a day need it or not
AsyncDelay displayMinRefresh = AsyncDelay(5 * 1000, AsyncDelay::MILLIS); // no more than once/five seconds

AsyncDelay lightLevelDelay = AsyncDelay(1*1000, AsyncDelay::MILLIS); // 1 Hz
int lightLevel = 0;

struct ButtonPress {
  ButtonPress(): a(false), b(false), c(false), d(false) {}
  boolean a : 1;
  boolean b : 1;
  boolean c : 1;
  boolean d : 1;
  bool operator==(const ButtonPress& rhs) const {
    return a == rhs.a && b == rhs.b && c == rhs.c && d == rhs.d;
  }
};
ButtonPress lastButtonPress;

AsyncDelay debounceDelay = AsyncDelay(250, AsyncDelay::MILLIS); // switch debounce
#ifdef RAIN_CLIENT
AsyncDelay capReadInterval = AsyncDelay(50, AsyncDelay::MILLIS); // read @ 20Hz
#endif

// WiFiClientSecure for SSL/TLS support
WiFiClientSecure client;
// Setup the MQTT client class by passing in the WiFi client and MQTT server and login details
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);
AsyncDelay mqttDelay = AsyncDelay(5000, AsyncDelay::MILLIS); // retry every 5 seconds if not connected

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
/****************************** Feeds ***************************************/

// Setup a feed called 'test' for publishing.
// Notice MQTT paths for AIO follow the form: <username>/feeds/<feedname>
#define FEED_PREFIX AIO_USERNAME "/feeds/rain-barrels."
AsyncDelay lightLevelFeedDelay = AsyncDelay(60*1000, AsyncDelay::MILLIS); // 1/minute
#ifdef RAIN_SERVER
Adafruit_MQTT_Publish lightLevelFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "ambient-light-server");

AsyncDelay valveStateFeedMaxDelay = AsyncDelay(60*60*1000, AsyncDelay::MILLIS); // at least 1/hr
AsyncDelay valveStateFeedMinDelay = AsyncDelay(1000, AsyncDelay::MILLIS); // not more than 1/second
Adafruit_MQTT_Publish valveStateFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "water-source");
#endif
#ifdef RAIN_CLIENT
Adafruit_MQTT_Publish lightLevelFeed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "ambient-light-client");

AsyncDelay waterLevelFeedDelay = AsyncDelay(10*1000, AsyncDelay::MILLIS); // every 10s
Adafruit_MQTT_Publish waterLevel1Feed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "waterlevel1");
Adafruit_MQTT_Publish waterLevel2Feed = Adafruit_MQTT_Publish(&mqtt, FEED_PREFIX "waterlevel2");
#endif

#ifdef RAIN_SERVER
# define MDNS_NAME SERVER_MDNS_NAME
#endif
#ifdef RAIN_CLIENT
# define MDNS_NAME CLIENT_MDNS_NAME
#endif

AsyncDelay connectionWatchdog = AsyncDelay(5*60*1000, AsyncDelay::MILLIS);

#ifdef RAIN_SERVER
#define WATER_GPIO 10
// Motorshield configuration
Adafruit_MotorShield AFMS = Adafruit_MotorShield();
Adafruit_DCMotor *cityValve = AFMS.getMotor(1);
Adafruit_DCMotor *ground1 = AFMS.getMotor(2);
Adafruit_DCMotor *ground2 = AFMS.getMotor(3);
Adafruit_DCMotor *pump = AFMS.getMotor(4);

WebServer server(SERVER_PORT);
AsyncDelay lastPing = AsyncDelay(3 * KEEPALIVE_INTERVAL_SECS * 1000, AsyncDelay::MILLIS);
// The valve seems to get hot if you keep it activated -- though it should not!  Anyway, only
// run the valve for a minute when it changes state.
int lastValveState = -1;
AsyncDelay valveRunLength = AsyncDelay(60 * 1000, AsyncDelay::MILLIS);
#endif

#ifdef RAIN_CLIENT
Adafruit_ADS1115 ads1115;
AsyncDelay lastLevelReading = AsyncDelay(30, AsyncDelay::MILLIS);
AsyncDelay lastPing = AsyncDelay(KEEPALIVE_INTERVAL_SECS * 1000, AsyncDelay::MILLIS);
String serverBaseUrl = String("http://" SERVER_MDNS_NAME ".local:80/update");

// It would really be nice if we could do this filtering on the analog
// side instead of the digital side... (we tried!)
#define LEVEL_SAMPLE_FILTER 16
int32_t level_accum[2] = { 0, 0 };

MPR121 capTouch;
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
#ifdef RAIN_CLIENT
SystemState last_xmit_state = state;
#endif

void updateState();

void normalface() {
  display.setFont(&bluehigh12pt7b);
}

void boldface() {
  display.setFont(&bluebold14pt7b);
}

#ifdef RAIN_SERVER
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
#endif

void setup() {
  // initialize built in LED pin as an output.
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);
  pinMode(BUTTON_D, INPUT_PULLUP);

  // set speaker enable pin to output
  pinMode(SPEAKER_SHUTDOWN, OUTPUT);
  // and immediately disable it to save power
  digitalWrite(SPEAKER_SHUTDOWN, LOW);

#ifdef RAIN_SERVER
  pinMode(WATER_GPIO, INPUT_PULLDOWN);
#ifdef MOTOR_DRIVER_CONNECTED
  // Motorshield Setup
  AFMS.begin();
  // This should ground all outputs
  cityValve->setSpeed(0); cityValve->run(FORWARD);
  ground1->setSpeed(0); ground1->run(FORWARD);
  ground2->setSpeed(0); ground2->run(FORWARD);
  pump->setSpeed(0); pump->run(FORWARD);
#endif
#endif

#ifdef RAIN_CLIENT
  pinMode(A1, INPUT);
  pinMode(2, INPUT);
  pinMode(10, INPUT);
  delay(100); // cap touch needs a moment!
  Wire.begin();
  capTouch = MPR121(-1, false, 0x5A, false, true);
  // Adafruit defaults TOUCH_THRESHOLD_DEFAULT 12 RELEASE_THRESHOLD_DEFAULT 6
  capTouch.setThresholds(8, 2);
  capTouch.setThreshold(0, 20, 2); // button A has an itchy trigger
  ads1115.begin(0x48); // At default address
  ads1115.setGain(GAIN_TWO); // +/-2.048V
#endif

  // for light sensor
  analogReadResolution(12); //12 bits
  analogSetAttenuation(ADC_11db);  //For all pins

  // Neopixel power
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, LOW); // on

  pixels.begin();
  pixels.setBrightness(50);
  pixels.fill(0xFF00FF);
  pixels.show(); // Show startup color

  display.begin(THINKINK_GRAYSCALE4);
  display.clearBuffer();
  display.setTextSize(1);
  display.setTextColor(EPD_BLACK);
  boldface();
  display.setCursor(0, 20);
  display.println(MDNS_NAME);
  normalface();
  display.setTextWrap(true);
  display.print("Connecting to SSID: ");
  display.println(WIFI_SSID);
  display.display();

  Serial.println("Booting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  // Set Adafruit IO's root CA
  client.setCACert(adafruitio_root_ca);

  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname(MDNS_NAME);

  // No authentication by default
#ifndef DEV_MODE
  ArduinoOTA.setPassword(OTA_PASSWD);
#endif

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

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
  })
  .onEnd([]() {
    Serial.println("\nEnd");
  })
  .onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  })
  .onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();

#ifdef RAIN_CLIENT
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
    if (MDNS.hostname(i) == "rainpump") {
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
      for (int j=0; j<LEVEL_SAMPLE_FILTER; j++) {
          level_accum[i] += ads1115.readADC_SingleEnded(i);
      }
  }
#endif

#ifdef RAIN_SERVER
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
#endif

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  displayMaxRefresh.expire();
  displayMinRefresh.expire();
  lastPing.expire();
  connectionWatchdog.restart();
}

#ifdef RAIN_CLIENT
void sendUpdate(int new_user_state = -1) {
  state.connected_recently = false;
  // test client
  HTTPClient http;
  String url2 = serverBaseUrl;
  url2 += "?level1=";
  url2 += state.water_level[0];
  url2 += "&level2=";
  url2 += state.water_level[1];
  if (new_user_state >= STATE_CITY && new_user_state <= STATE_RAIN) {
    url2 += "&state=";
    url2 += new_user_state;
  }
  //Serial.println(url2);
  http.begin(url2);
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
  }
  http.end();
  lastPing.restart();
  last_xmit_state = state;
}
#endif

void updatePixels() {
  // Measured light levels:
  // 8191 cloudy day outside (this is saturated)
  // 5800 in my office during the daytime with the lights on (cloudy day)
  // 4200 in my office during the daytime with the lights off (cloudy day)
  // 1400-1500 in my office indoors at night
  // 0 with my finger over it
  pixels.setBrightness(50);
  if (lightLevel < 2000) {
    // white backlight for readability at night
    pixels.fill(pixels.Color(32, 32, 32));
  } else if (state.active_state == STATE_CITY) {
    pixels.fill(pixels.Color(0, 0, 64));
  } else {
    pixels.fill(pixels.Color(0, 64, 0));
  }
  if (lastButtonPress.a) {
    pixels.setPixelColor(3, 255, 0, 0);
  }
  if (lastButtonPress.b) {
    pixels.setPixelColor(2, 0, 255, 0);
  }
  if (lastButtonPress.c) {
    pixels.setPixelColor(1, 0, 0, 255);
  }
  if (lastButtonPress.d) {
    pixels.setPixelColor(0, 255,255,255);
  }
  pixels.show();
}


void updateState() {
  if (lightLevelDelay.isExpired()) {
    // turn off the backlight for a true reading of ambient
    pixels.setBrightness(0);
    pixels.fill(0);
    pixels.show();

    lightLevel = analogRead(LIGHT_SENSOR);

    // turn the pixels back on (at appropriate brightness!)
    updatePixels();

    if (lightLevelFeedDelay.isExpired() && mqtt.connected()) {
      lightLevelFeed.publish(lightLevel);
      lightLevelFeedDelay.restart();
    }
    lightLevelDelay.restart();
  }
#ifdef RAIN_CLIENT
  if (lastLevelReading.isExpired()) {
    // use .repeat() to ensure our filter bandwidth is relatively constant
    // even if there's a (slow) display refresh between samples
    lastLevelReading.repeat();
    int32_t raw_level[2];
    for (int i=0; i<2; i++) {
      level_accum[i] =
        ((int64_t)level_accum[i] * (LEVEL_SAMPLE_FILTER-1) / LEVEL_SAMPLE_FILTER)
        + ads1115.readADC_SingleEnded(i);
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
    // periodically send levels to AIO
    if (waterLevelFeedDelay.isExpired() && mqtt.connected()) {
      // publishing raw levels for now, for calibration purpoes
      waterLevel1Feed.publish(raw_level[0]/*state.water_level[0]*/);
      waterLevel2Feed.publish(raw_level[1]/*state.water_level[1]*/);
      waterLevelFeedDelay.restart();
    }
  }
  if (lastPing.isExpired() || !state_equal(&state, &last_xmit_state)) {
    // send our levels to the server via a GET request and update our copy of the server state
    sendUpdate();
  }
#endif
#ifdef RAIN_SERVER
  // read our pipe water sensor
  state.pipe_water_present = !digitalRead(WATER_GPIO);

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

void updateDisplay() {
  // eInk display update
  if (state_equal(&state, &last_displayed_state) && !displayMaxRefresh.isExpired()) {
    return; // don't update if nothing has changed
  }

  if (!displayMinRefresh.isExpired()) {
    return; // don't update too frequently
  }
  displayMinRefresh.restart();
  displayMaxRefresh.restart();
  last_displayed_state = state;

  // make neopixel color match the user state
  updatePixels();

  boolean client_connected_recently = true;
#ifdef RAIN_CLIENT
  client_connected_recently = state.connected_recently;
#endif

  display.clearBuffer();
  normalface();
  display.setTextColor(EPD_DARK);
  display.setCursor(4, 17);
  display.setTextSize(1);
  display.print(WiFi.localIP());
  display.setCursor(display.width() - 4, 17);
  rightjustify(MDNS_NAME ".local");

#define BAR_TOP 26
#define BAR_HEIGHT (53-26)
#define BAR_WIDTH (256-38)
#define BAR_SPACE (63-26)
  for (int i = 0; i < 2; i++) {
    int x = display.width() / 2 - (BAR_WIDTH / 2);
    int y = BAR_TOP + i * BAR_SPACE;
    int amount = state.water_level[i] * (BAR_WIDTH - 2) / 1000;
    display.setCursor(x - 16, y + (BAR_HEIGHT / 2) + 6);
    display.setTextColor(EPD_DARK);
    rightjustify(i == 0 ? "1:" : "2:");
    display.fillRect(x, y, BAR_WIDTH, BAR_HEIGHT, EPD_BLACK);
    display.fillRect(x + 1, y + 1, BAR_WIDTH - 2, BAR_HEIGHT - 2, EPD_WHITE);
#ifdef RAIN_SERVER
    if (!state.connected_recently) {
      // no recent water level data
      continue;
    }
#endif
    display.fillRect(x + 1, y + 1, amount, BAR_HEIGHT - 2, EPD_LIGHT); // the actual graph amount
    display.setTextColor(EPD_BLACK);
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
                     display.width() / 4, BUTTON_HEIGHT, EPD_LIGHT);
  }
  display.drawFastHLine(0, display.height() - BUTTON_HEIGHT, display.width(), EPD_DARK);
  for (int i = 0; i < 3; i++) {
    display.drawFastVLine((i + 1)*display.width() / 4, display.height() - BUTTON_HEIGHT, BUTTON_HEIGHT, EPD_DARK);
  }
  for (int i = 0; i < 3; i++) {
    display.setCursor((2 * i + 1)*display.width() / 8, display.height() - BUTTON_HEIGHT / 2);
    if (!client_connected_recently) {
      display.setTextColor(EPD_LIGHT);
      normalface();
    } else if (i == state.user_state) {
      display.setTextColor(EPD_BLACK);
      boldface();
    } else {
      if (i == state.active_state) {
        display.setTextColor(EPD_BLACK);
      } else {
        display.setTextColor(EPD_DARK);
      }
      normalface();
    }
    centerjustify(i == 0 ? "CITY" : i == 1 ? "AUTO" : "RAIN" );
  }
#define BITMAP4(x, y, w, h, num) BITMAP4_(x,y,w,h,num) // need to expand the _NUM macro before pasting
#define BITMAP4_(x, y, w, h, num) do {\
    display.drawBitmap(x, y, LightBitmap ## num, w, h, EPD_LIGHT); \
    display.drawBitmap(x, y, DarkBitmap ## num, w, h, EPD_DARK); \
    display.drawBitmap(x, y, BlackBitmap ## num, w, h, EPD_BLACK); \
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
  if (client_connected_recently) {
    if (state.pipe_water_present) {
      BITMAP4(242 - DROP_ON_WIDTH / 2,
              display.height() - BUTTON_HEIGHT / 2 - DROP_ON_HEIGHT / 2,
              DROP_ON_WIDTH, DROP_ON_HEIGHT,
              DROP_ON_NUM);
    } else {
      BITMAP4(242 - DROP_OFF_WIDTH / 2,
              display.height() - BUTTON_HEIGHT / 2 - DROP_OFF_HEIGHT / 2,
              DROP_OFF_WIDTH, DROP_OFF_HEIGHT,
              DROP_OFF_NUM);
    }
  }
  // connection status
  if (state.connected_recently) {
    BITMAP4(274 - CONN_ON_WIDTH / 2,
            display.height() - BUTTON_HEIGHT / 2 - CONN_ON_HEIGHT / 2,
            CONN_ON_WIDTH, CONN_ON_HEIGHT,
            CONN_ON_NUM);
  } else {
    BITMAP4(274 - CONN_OFF_WIDTH / 2,
            display.height() - BUTTON_HEIGHT / 2 - CONN_OFF_HEIGHT / 2,
            CONN_OFF_WIDTH, CONN_OFF_HEIGHT,
            CONN_OFF_NUM);
  }

  display.display(); // or partial update?
}

#ifdef ENABLE_AUDIO
uint32_t audio_pointer = 0;
bool audio_playing = false;
AsyncDelay audioDelay = AsyncDelay(1000000L / SAMPLE_RATE, AsyncDelay::MICROS);
void start_audio() {
  audio_pointer = 0;
  audio_playing = true;
  audioDelay.restart();
  digitalWrite(SPEAKER_SHUTDOWN, HIGH);
}
void play_audio() {
  if (audio_pointer >= sizeof(audio)) {
    audio_playing = false;
    digitalWrite(SPEAKER_SHUTDOWN, LOW);
  } else if (audioDelay.isExpired()) {
    if (audio_pointer == 0) {
      audioDelay.restart();
    } else {
      audioDelay.repeat();
    }
    dacWrite(A0, audio[audio_pointer++]);
  }
}
#endif /* ENABLE_AUDIO */

void loop() {
#ifdef ENABLE_AUDIO
  if (audio_playing) {
    play_audio();
    return;
  }
#endif
  ArduinoOTA.handle();
  if (connectionWatchdog.isExpired()) {
    delay(5000);
    ESP.restart();
  }
  if ((!mqtt.connected()) && mqttDelay.isExpired()) {
    int ret = mqtt.connect();
    if (ret != 0) {
      Serial.println(mqtt.connectErrorString(ret));
    }
    mqttDelay.restart();
  }
  // Update user state via buttons
  ButtonPress button_press;
  if (!debounceDelay.isExpired()) {
    // don't process additional button presses while debouncing
  } else {
    if (! digitalRead(BUTTON_A)) {
      button_press.a = true;
    }
    if (! digitalRead(BUTTON_B)) {
      button_press.b = true;
    }
    if (! digitalRead(BUTTON_C)) {
      button_press.c = true;
    }
    if (! digitalRead(BUTTON_D)) {
      button_press.d = true;
    }
#ifdef RAIN_CLIENT
    if (capReadInterval.isExpired()) { // don't clog up the I2C
      capReadInterval.restart();
      capTouch.readTouchInputs();
      if (capTouch.touched(0)) {
        button_press.a = true;
      }
      if (capTouch.touched(1)) {
        button_press.b = true;
      }
      if (capTouch.touched(2)) {
        button_press.c = true;
      }
      if (capTouch.touched(3)) {
        button_press.d = true;
      }
    } else {
      button_press = lastButtonPress;
    }
#endif
    if (!(lastButtonPress == button_press)) {
      updatePixels(); // show button press!
    }
    lastButtonPress = button_press;
    if (button_press.a || button_press.b || button_press.c || button_press.d) {
      debounceDelay.restart();
#ifdef ENABLE_AUDIO
      start_audio();
#endif
    }
  }
#ifdef RAIN_SERVER
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
#ifdef RAIN_CLIENT
  // read buttons, if pressed immediately sent a ?state= request to the server and expire lastPing()
#if 1 // def DEV_MODE
  button_press = ButtonPress(); // XXX getting sensitivity right
#endif
  if (button_press.a) {
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

#endif /* RAIN_SERVER || RAIN_CLIENT */
