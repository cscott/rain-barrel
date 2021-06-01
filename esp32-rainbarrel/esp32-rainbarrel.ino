#define RAIN_SERVER
#undef RAIN_CLIENT
#define MOTOR_DRIVER_CONNECTED
#undef TOUCH_INPUTS
#undef DEV_MODE // shorter delays for easier development

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_ThinkInk.h>
#include <ArduinoJson.h>
#include <AsyncDelay.h>
#include "bluehigh-12px.h"
#include "bluebold-14px.h"
#include "icons.h"
#include "coin.h"

#ifdef RAIN_SERVER
# include <Wire.h>
# include <Adafruit_MotorShield.h>
#endif
#ifdef RAIN_CLIENT
# include <HTTPClient.h>
#endif

#define SERVER_MDNS_NAME "rainpump"
#define CLIENT_MDNS_NAME "raingauge"
#define OTA_PASSWD "goaway"
#define WATER_ALARM_LOW 100 /* 10% */
#define KEEPALIVE_INTERVAL_SECS 60
#define LEVEL_SENSOR_INTERVAL_SECS 300 // should be 1 eventually
#define SERVER_PORT 80
#define WATER_READING_ZERO 0
#define WATER_READING_FULL 1024
#define TOUCH_THRESHOLD 105 // percent

const char* ssid = "GriggsCorner";
const char* password = "lottedog";

#define EPD_DC      7 // can be any pin, but required!
#define EPD_CS      8  // can be any pin, but required!
#define EPD_BUSY    -1  // can set to -1 to not use a pin (will wait a fixed delay)
#define SRAM_CS     -1  // can set to -1 to not use a pin (uses a lot of RAM!)
#define EPD_RESET   6  // can set to -1 and share with chip Reset (can't deep sleep)
#define COLOR1 EPD_BLACK
#define COLOR2 EPD_LIGHT
#define COLOR3 EPD_DARK

Adafruit_NeoPixel pixels = Adafruit_NeoPixel(4, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
// 2.9" Grayscale Featherwing or Breakout:
ThinkInk_290_Grayscale4_T5 display(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
AsyncDelay displayMaxRefresh = AsyncDelay(24 * 60 * 60 * 1000, AsyncDelay::MILLIS); // once a day need it or not
AsyncDelay displayMinRefresh = AsyncDelay(5 * 1000, AsyncDelay::MILLIS); // no more than once/five seconds
AsyncDelay lightLevelDelay = AsyncDelay(1000, AsyncDelay::MILLIS); // 1 Hz
int lightLevel = 0;
int lastBrightness = 50;

#ifdef RAIN_SERVER
# define MDNS_NAME SERVER_MDNS_NAME
#else
# define MDNS_NAME CLIENT_MDNS_NAME
#endif

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
#define LEVEL_SENSOR1_PIN A1
#define LEVEL_SENSOR2_PIN 10
AsyncDelay lastLevelReading = AsyncDelay(LEVEL_SENSOR_INTERVAL_SECS * 1000, AsyncDelay::MILLIS);
AsyncDelay lastPing = AsyncDelay(KEEPALIVE_INTERVAL_SECS * 1000, AsyncDelay::MILLIS);
String serverBaseUrl = String("http://" SERVER_MDNS_NAME ".local:80/update");

AsyncDelay touchDelay = AsyncDelay(2000, AsyncDelay::MILLIS);
int touchPins[3] = { T14, T12, T11 };
int touchButtons[3] = { BUTTON_A, BUTTON_B, BUTTON_C };
int touchBase[3];
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
    if (a->water_level[i] != b->water_level[i]) {
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

void normalface() {
  display.setFont(&bluehigh12pt7b);
}

void boldface() {
  display.setFont(&bluebold14pt7b);
}

#ifdef RAIN_SERVER
// Web server
void handleRoot() {
  char temp[400];
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;

  snprintf(temp, 400,

           "<html>\
  <head>\
    <meta http-equiv='refresh' content='5'/>\
    <title>Rainbarrel Pump Manager</title>\
    <style>\
      body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }\
    </style>\
  </head>\
  <body>\
    <h1>Rainbarrel Pump Manager</h1>\
    <p>Uptime: %02d:%02d:%02d</p>\
    <p>Current water source: %s</p>\
    <img src=\"/test.svg\" />\
  </body>\
</html>",

           hr, min % 60, sec % 60,
           state.active_state == STATE_CITY ? "City water" : "Rain barrels"
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
#ifndef TOUCH_INPUTS
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);
  pinMode(BUTTON_D, INPUT_PULLUP);
#endif
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
#ifdef TOUCH_INPUTS
  pinMode(LEVEL_SENSOR1_PIN, INPUT);
  pinMode(LEVEL_SENSOR2_PIN, INPUT);
  for (int i = 0; i < 3; i++) {
    touchBase[i] = touchRead(touchPins[i]);
  }
#endif
#endif

  // for light sensor
  analogReadResolution(12); //12 bits
  analogSetAttenuation(ADC_11db);  //For all pins

  // Neopixel power
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, LOW); // on

  pixels.begin();
  pixels.setBrightness(lastBrightness);
  pixels.fill(0xFF00FF);
  pixels.show(); // Initialize all pixels to 'off'

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
  display.println(ssid);
  display.display();

  Serial.println("Booting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  ArduinoOTA.setHostname(MDNS_NAME);

  // No authentication by default
  //ArduinoOTA.setPassword(OTA_PASSWD);

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
      Serial.printf("Got user_state %d\n", state.user_state);
    }
  }
  http.end();
  lastPing.restart();
  last_xmit_state = state;
}
#endif

void updateState() {
  if (lightLevelDelay.isExpired()) {
    lightLevel = analogRead(LIGHT_SENSOR);
    lightLevelDelay.restart();
    //Serial.print("Light level: ");
    //Serial.println(lightLevel);
  }
#ifdef RAIN_CLIENT
  if (lastLevelReading.isExpired()) {
    // XXX read our level sensors
    state.water_level[0] = random(0, 1000); // analogRead(LEVEL_SENSOR1_PIN);
    state.water_level[1] = random(0, 1000); // analogRead(LEVEL_SENSOR2_PIN);
    lastLevelReading.restart();
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
      cityValve->setSpeed(255);
      valveRunLength.restart();
    }
  } else {
    // move valve to 'rain'
    cityValve->run(BACKWARD);
    if (lastValveState != STATE_RAIN) {
      lastValveState = STATE_RAIN;
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
  // Adjust NeoPixel brightness
  // Measured light levels: 1400-1500 in my office indoors at night
  //                        0 with my finger over it
  int newBrightness = 50;
#ifdef RAIN_CLIENT
  if (lightLevel < 1000 || lightLevel > 2000) { // XXX is this the right relationship?
    newBrightness = 255;
  }
#endif
  if (newBrightness != lastBrightness) {
    lastBrightness = newBrightness;
    pixels.setBrightness(lastBrightness);
    pixels.show();
  }
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

  // make neopixel color match the user state
  if (state.active_state == STATE_CITY) {
    pixels.fill(0x0000FF);
  } else {
    pixels.fill(0x00FF00);
  }
  pixels.show();
}

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

void loop() {
  if (audio_playing) {
    play_audio();
    return;
  }
  ArduinoOTA.handle();
#ifdef RAIN_SERVER
  server.handleClient();
  // Update user state via buttons
  if (! digitalRead(BUTTON_A)) {
    state.user_state = STATE_CITY;
    start_audio();
  } else if (! digitalRead(BUTTON_B)) {
    state.user_state = STATE_AUTO;
    start_audio();
  } else if (! digitalRead(BUTTON_C)) {
    state.user_state = STATE_RAIN;
    start_audio();
  } else if (! digitalRead(BUTTON_D)) {
    // XXX for testing
    state.pipe_water_present = !state.pipe_water_present;
    state.connected_recently = !state.connected_recently;
    start_audio();
  }
  updateState(); // read various sensors
#endif
#ifdef RAIN_CLIENT
  // read buttons, if pressed immediately sent a ?state= request to the server and expire lastPing()
  // Update user state via buttons
  int button_press = 0;
#ifdef TOUCH_INPUTS
  if (touchDelay.isExpired()) {
    touchDelay.restart();
    for (int i = 0; i < 3; i++) {
      int recent = touchRead(touchPins[i]);
      Serial.print(i); Serial.print(": "); Serial.print(touchBase[i]); Serial.print(" "); Serial.println(recent); delay(1);
      if (button_press == 0 && recent > (touchBase[i] * TOUCH_THRESHOLD / 100)) {
        button_press = touchButtons[i];
      }
      touchBase[i] = ((touchBase[i] * 9) + recent) / 10;
    }
  }
#else
  if (! digitalRead(BUTTON_A)) {
    button_press = BUTTON_A;
  } else if (! digitalRead(BUTTON_B)) {
    button_press = BUTTON_B;
  } else if (! digitalRead(BUTTON_C)) {
    button_press = BUTTON_C;
  } else if (! digitalRead(BUTTON_D)) {
    button_press = BUTTON_D;
  }
#endif
  if (button_press == BUTTON_A) {
    sendUpdate(STATE_CITY);
    displayMaxRefresh.expire();
    start_audio();
  } else if (button_press == BUTTON_B) {
    sendUpdate(STATE_AUTO);
    displayMaxRefresh.expire();
    start_audio();
  } else if (button_press == BUTTON_C) {
    sendUpdate(STATE_RAIN);
    displayMaxRefresh.expire();
    start_audio();
  } else if (button_press == BUTTON_D) {
    start_audio();
    lastPing.expire();
  } else {
    updateState();
  }
#endif
  updateDisplay(); // possibly update display if state has changed

#if 0
  if (touchDelay.isExpired()) {
    touchDelay.restart();
    Serial.print("A: ");
    Serial.println(touchRead(T14));
    Serial.print("B: ");
    Serial.println(touchRead(T12));
    Serial.print("C: ");
    Serial.println(touchRead(T11));
  }
#endif
}
