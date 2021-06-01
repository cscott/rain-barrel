// includes for Web Server & mDNS
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// includes for OLED display
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// includes for Motor Shield
//#include <Wire.h>
#include <Adafruit_MotorShield.h>

// Wifi configuration
#ifndef STASSID
#define STASSID "GriggsCorner"
#define STAPSK  "lottedog"
#endif
#define MDNS_NAME "rainbarrel"

const char *ssid = STASSID;
const char *password = STAPSK;

ESP8266WebServer server(80);

// Configuration of OLED display
Adafruit_SH110X display = Adafruit_SH110X(64, 128, &Wire);

// OLED FeatherWing buttons map to different pins depending on board:
#if defined(ESP8266)
  #define BUTTON_A  0
  #define BUTTON_B 16
  #define BUTTON_C  2
#else
  #error I removed the other boards...
#endif

// Motorshield configuration
Adafruit_MotorShield AFMS = Adafruit_MotorShield();
Adafruit_DCMotor *cityValve = AFMS.getMotor(1);
Adafruit_DCMotor *ground1 = AFMS.getMotor(2);
Adafruit_DCMotor *ground2 = AFMS.getMotor(3);
Adafruit_DCMotor *pump = AFMS.getMotor(4);

// Other config
#define LED_GPIO 0
#define LED_OFF 1 // active low
#define LED_ON  0 // active low

#define WATER_GPIO 14

#define STATE_CITY 0
#define STATE_AUTO 1
#define STATE_RAIN 2

unsigned char user_state = STATE_AUTO;
unsigned char selected_state = STATE_CITY;
boolean is_water_empty = true;
boolean mdns_success = false;

// Valve logic
void pollWaterEmpty() {
  // XXX in the future we may poll the level sensor via WiFi
  // every 15 seconds or something like that.
  if (digitalRead(WATER_GPIO)) {
    is_water_empty = true;
  } else {
    is_water_empty = false;
  }
}

void updateState() {
  pollWaterEmpty(); // sets the is_water_empty flag
  if (user_state != STATE_AUTO) {
    // force a specific state
    selected_state = user_state;
  } else {
    // in auto
    if ( is_water_empty ) {
      selected_state = STATE_CITY;
    } else {
      selected_state = STATE_RAIN;
    }
  }
  // ok, now adjust settings to match selected state
  if (selected_state == STATE_CITY) {
    // pump off
    pump->setSpeed(0);
    // move value to 'city'
    cityValve->run(FORWARD); cityValve->setSpeed(255);
  } else {
    // move valve to 'rain'
    cityValve->run(BACKWARD); cityValve->setSpeed(255);
    // turn pump on
    pump->setSpeed(255);
  }
}

// Web server
void handleRoot() {
  digitalWrite(LED_GPIO, LED_ON);
  pinMode(LED_GPIO, OUTPUT);
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
           selected_state == STATE_CITY ? "City water" : "Rain barrels"
          );
  server.send(200, "text/html", temp);
  pinMode(LED_GPIO, INPUT_PULLUP);
  digitalWrite(LED_GPIO, LED_OFF);
}

void handleNotFound() {
  digitalWrite(LED_GPIO, LED_ON);
  pinMode(LED_GPIO, OUTPUT);
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
  pinMode(LED_GPIO, INPUT_PULLUP);
  digitalWrite(LED_GPIO, LED_OFF);
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

void normalText() {
  display.setTextColor(SH110X_WHITE, SH110X_BLACK);
}
void invertText() {
  display.setTextColor(SH110X_BLACK, SH110X_WHITE);
}
void maybeInvertText(bool maybe) {
  if (maybe) invertText(); else normalText();
}


// Setup
void setup() {
  Serial.begin(115200);
  Serial.println("Rain barrel pump manager");
  pinMode(WATER_GPIO, INPUT); // do I need a pull-down?

  // Motorshield Setup
  AFMS.begin();
  // This should ground all outputs
  cityValve->setSpeed(0); cityValve->run(FORWARD);
  ground1->setSpeed(0); ground1->run(FORWARD);
  ground2->setSpeed(0); ground2->run(FORWARD);
  pump->setSpeed(0); pump->run(FORWARD);

  // Wifi Setup
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

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
  display.println("Rainbarrel manager");
  normalText();

  display.println("");
  display.print("Connecting to SSID\n" STASSID ": ");
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
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  mdns_success = false;
  if (MDNS.begin(MDNS_NAME)) {
    mdns_success = true;
    Serial.println("MDNS responder started: " MDNS_NAME);
    display.println("mDNS: " MDNS_NAME);
    display.display();
  }

  server.on("/", handleRoot);
  server.on("/test.svg", drawGraph);
  server.on("/status", []() {
    server.send(200, "text/plain", selected_state==STATE_CITY ? "city" : "rain" );
  });
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0,0);
  if (mdns_success) {
    display.print( MDNS_NAME ".local" );
  }
  display.println();
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.println();

  display.print( user_state==STATE_CITY ? "*" : " " );
  display.print( " [A] ");
  maybeInvertText(selected_state==STATE_CITY);
  display.println("CITY");
  normalText();

  display.print( user_state==STATE_AUTO ? "*" : " " );
  display.print( " [B] ");
  display.println("AUTO");

  display.print( user_state==STATE_RAIN ? "*" : " " );
  display.print( " [C] ");
  maybeInvertText(selected_state==STATE_RAIN);
  display.println("RAIN");
  normalText();

  display.println();
  display.print("Barrels are ");
  display.println( is_water_empty ? "EMPTY" : "FULL" );
  display.display();
}

void loop() {
  // main control loop
  if(!digitalRead(BUTTON_A)) user_state = STATE_CITY;
  if(!digitalRead(BUTTON_B)) user_state = STATE_AUTO;
  if(!digitalRead(BUTTON_C)) user_state = STATE_RAIN;
  updateState();
  updateDisplay();
  // also do web stuff
  server.handleClient();
  MDNS.update();
  yield();
}
