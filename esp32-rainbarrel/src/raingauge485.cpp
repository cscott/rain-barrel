#ifdef RAINGAUGE485

#include "Arduino.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <AsyncDelay.h>
#include "config.h"

#define SERVER_MDNS_NAME "rainpump"
#define MDNS_NAME        "raingauge485"

#define RS485_TX_PIN     17
#define RS485_RX_PIN     18
#define RS485_EN_PIN     21
// Optoisolator U9 assumed non-inverting (must be, or UART polarity would break).
// If RS485 direction is reversed, swap these two values.
#define RS485_TX_ENABLE  HIGH
#define RS485_RX_ENABLE  LOW

#define MODBUS_BAUD      9600
#define NUM_SENSORS      4
#define MODBUS_TIMEOUT_MS 100

#define SECONDS_MS (1000)
#define MINUTES_MS (60 * SECONDS_MS)

HardwareSerial Serial485(1);
WebServer server(80);

String serverBaseUrl = String("http://" SERVER_MDNS_NAME ".local:80/update");

int16_t sensorValue[NUM_SENSORS];
bool    sensorPresent[NUM_SENSORS];
uint8_t sensorUnit[NUM_SENSORS];
uint8_t sensorDecimals[NUM_SENSORS];
bool    sensorConfigValid[NUM_SENSORS];

AsyncDelay pollInterval = AsyncDelay(5 * SECONDS_MS + 3, AsyncDelay::MILLIS);

// ── MODBUS CRC16 (polynomial 0xA001) ─────────────────────────────────────────

static uint16_t crc16(const uint8_t *buf, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else              { crc >>= 1; }
        }
    }
    return crc;
}

// ── RS485 half-duplex send/receive ────────────────────────────────────────────

static void rs485_send(const uint8_t *buf, size_t len) {
    digitalWrite(RS485_EN_PIN, RS485_TX_ENABLE);
    Serial485.write(buf, len);
    Serial485.flush(); // wait for TX FIFO to drain into shift register
    delay(2);          // 2ms > 1.5 char-times at 9600 baud; ensures last stop bit clears
    digitalWrite(RS485_EN_PIN, RS485_RX_ENABLE);
    while (Serial485.available()) Serial485.read(); // discard any bus noise
}

static int rs485_recv(uint8_t *buf, int expected) {
    unsigned long start = millis();
    int idx = 0;
    while (idx < expected) {
        if (millis() - start > (unsigned long)MODBUS_TIMEOUT_MS) break;
        if (Serial485.available()) buf[idx++] = (uint8_t)Serial485.read();
    }
    return idx;
}

// ── MODBUS FC=0x03 read holding register ─────────────────────────────────────

bool modbus_read_reg(uint8_t addr, uint16_t reg, uint16_t *result) {
    uint8_t req[8];
    req[0] = addr;
    req[1] = 0x03;
    req[2] = (uint8_t)(reg >> 8);
    req[3] = (uint8_t)(reg & 0xFF);
    req[4] = 0x00;
    req[5] = 0x01; // read 1 register
    uint16_t crc = crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    rs485_send(req, sizeof(req));

    // Response: addr FC DL data_H data_L CRC_L CRC_H  (7 bytes)
    uint8_t resp[7];
    if (rs485_recv(resp, 7) < 7) return false;
    if (resp[0] != addr || resp[1] != 0x03 || resp[2] != 0x02) return false;
    uint16_t resp_crc = crc16(resp, 5);
    if (resp[5] != (uint8_t)(resp_crc & 0xFF) ||
        resp[6] != (uint8_t)(resp_crc >> 8)) return false;
    *result = ((uint16_t)resp[3] << 8) | resp[4];
    return true;
}

// ── MODBUS FC=0x06 write single register ─────────────────────────────────────

bool modbus_write_reg(uint8_t addr, uint16_t reg, uint16_t value) {
    uint8_t req[8];
    req[0] = addr;
    req[1] = 0x06;
    req[2] = (uint8_t)(reg >> 8);
    req[3] = (uint8_t)(reg & 0xFF);
    req[4] = (uint8_t)(value >> 8);
    req[5] = (uint8_t)(value & 0xFF);
    uint16_t crc = crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    rs485_send(req, sizeof(req));

    // Response is an echo of the request
    uint8_t resp[8];
    if (rs485_recv(resp, 8) < 8) return false;
    return memcmp(req, resp, 8) == 0;
}

// ── Address assignment ────────────────────────────────────────────────────────

// Change the sensor at currentAddr to newAddr, then save the new address to
// EEPROM.  Per the datasheet, the sensor switches to newAddr immediately after
// echoing the write-address response, so the save command is sent at newAddr.
bool setDeviceAddress(uint8_t currentAddr, uint8_t newAddr) {
    if (!modbus_write_reg(currentAddr, 0x0000, newAddr)) return false;
    delay(50); // sensor switches to newAddr after echoing response at currentAddr
    return modbus_write_reg(newAddr, 0x000F, 0x0000); // save to EEPROM
}

// ── sendUpdate stub ───────────────────────────────────────────────────────────

// Stub: will eventually send calibrated water levels to rainpump32 via HTTP GET
// to serverBaseUrl, as raingauge32 does in v2rain.cpp.  Not called until
// hardware is tested and level min/max are calibrated.
void sendUpdate() {
    // TODO: build URL from serverBaseUrl, append ?level1=...&level2=...&level3=...
    // and send HTTP GET as raingauge32 does in v2rain.cpp sendUpdate().
    (void)serverBaseUrl;
}

// ── Depth formatting helpers ──────────────────────────────────────────────────

static const char *unitName(uint8_t code) {
    switch (code) {
        case  1: return "KPa";
        case  2: return "MPa";
        case  3: return "Pa";
        case  4: return "bar";
        case  5: return "mbar";
        case  6: return "kgf/cm2";
        case  7: return "psi";
        case 16: return "m";
        case 17: return "cm";
        case 18: return "mm";
        default: return "?";
    }
}

// Format raw reading with decimal point applied, e.g. raw=2500, decimals=1 -> "250.0 cm"
static void formatDepth(char *buf, size_t buflen, int16_t raw, uint8_t decimals, uint8_t unit) {
    if (decimals == 0) {
        snprintf(buf, buflen, "%d %s", (int)raw, unitName(unit));
        return;
    }
    // Build divisor = 10^decimals
    int32_t divisor = 1;
    for (uint8_t d = 0; d < decimals; d++) divisor *= 10;
    int32_t whole = raw / divisor;
    int32_t frac  = raw % divisor;
    if (frac < 0) frac = -frac;
    // print fractional part zero-padded to 'decimals' digits
    snprintf(buf, buflen, "%d.%0*d %s", (int)whole, (int)decimals, (int)frac, unitName(unit));
}

// Read unit (reg 0x0002) and decimal point (reg 0x0003) for sensor at 1-based index i+1
static bool readSensorConfig(int i) {
    uint16_t u, d;
    if (!modbus_read_reg(i + 1, 0x0002, &u)) return false;
    if (!modbus_read_reg(i + 1, 0x0003, &d)) return false;
    sensorUnit[i]        = (uint8_t)u;
    sensorDecimals[i]    = (uint8_t)(d & 0x0F); // 0–4
    sensorConfigValid[i] = true;
    return true;
}

// ── Sensor polling ────────────────────────────────────────────────────────────

void updateSensors() {
    for (int i = 0; i < NUM_SENSORS; i++) {
        uint16_t raw;
        if (modbus_read_reg(i + 1, 0x0004, &raw)) {
            sensorValue[i]   = (int16_t)raw;
            if (!sensorPresent[i] || !sensorConfigValid[i])
                readSensorConfig(i);
            sensorPresent[i] = true;
        } else {
            sensorPresent[i] = false;
        }
    }
}

// ── Web server handlers ───────────────────────────────────────────────────────

void handleRoot() {
    char rows[512] = "";
    for (int i = 0; i < NUM_SENSORS; i++) {
        char row[128];
        if (sensorPresent[i]) {
            char depth[32] = "-";
            if (sensorConfigValid[i])
                formatDepth(depth, sizeof(depth), sensorValue[i], sensorDecimals[i], sensorUnit[i]);
            snprintf(row, sizeof(row),
                     "      <tr><td>0x%02X</td><td>%d</td><td>%s</td></tr>\n",
                     i + 1, (int)sensorValue[i], depth);
        } else {
            snprintf(row, sizeof(row),
                     "      <tr><td>0x%02X</td><td>-</td><td>-</td></tr>\n", i + 1);
        }
        strncat(rows, row, sizeof(rows) - strlen(rows) - 1);
    }

    int sec = millis() / 1000;
    int min = sec / 60;
    int hr  = min / 60;

    char buf[1500];
    snprintf(buf, sizeof(buf),
        "<html>\n"
        "  <head>\n"
        "    <meta http-equiv='refresh' content='10'/>\n"
        "    <title>RS485 Rain Gauge</title>\n"
        "    <style>body{font-family:Arial,sans-serif;}td,th{padding:3px 12px;}</style>\n"
        "  </head>\n"
        "  <body>\n"
        "    <h1>RS485 Rain Gauge</h1>\n"
        "    <p><a href='http://homeassistant.local:8123/rain-barrels'>Home Assistant</a></p>\n"
        "    <p>Uptime: %02d:%02d:%02d &nbsp; IP: %s</p>\n"
        "    <p>Pump server: %s</p>\n"
        "    <h2>Sensor Readings (raw)</h2>\n"
        "    <table border='1'>\n"
        "      <tr><th>Address</th><th>Raw Value</th><th>Depth</th></tr>\n"
        "%s"
        "    </table>\n"
        "    <h2>Assign Address</h2>\n"
        "    <p>Reassign a sensor to a new address:</p>\n"
        "    <form action='/setaddr'>\n"
        "      Old address: <select name='old'>\n"
        "        <option value='1'>1</option>\n"
        "        <option value='2'>2</option>\n"
        "        <option value='3'>3</option>\n"
        "        <option value='4'>4</option>\n"
        "      </select>\n"
        "      New address: <select name='new'>\n"
        "        <option value='1'>1</option>\n"
        "        <option value='2'>2</option>\n"
        "        <option value='3'>3</option>\n"
        "        <option value='4'>4</option>\n"
        "      </select>\n"
        "      <input type='submit' value='Set Address'>\n"
        "    </form>\n"
        "  </body>\n"
        "</html>",
        hr, min % 60, sec % 60,
        WiFi.localIP().toString().c_str(),
        serverBaseUrl.c_str(),
        rows);
    server.send(200, "text/html", buf);
}

void handleSetAddr() {
    if (!server.hasArg("old")) {
        server.send(400, "text/plain", "Missing 'old' parameter");
        return;
    }
    if (!server.hasArg("new")) {
        server.send(400, "text/plain", "Missing 'new' parameter");
        return;
    }
    int oldAddr = server.arg("old").toInt();
    if (oldAddr < 1 || oldAddr > 4) {
        server.send(400, "text/plain", "Old address must be 1-4");
        return;
    }
    int newAddr = server.arg("new").toInt();
    if (newAddr < 1 || newAddr > 4) {
        server.send(400, "text/plain", "New address must be 1-4");
        return;
    }
    bool ok = setDeviceAddress((uint8_t)oldAddr, (uint8_t)newAddr);
    char msg[256];
    snprintf(msg, sizeof(msg),
        "<html><body>"
        "<p>Set address from 0x%02X to 0x%02X: <b>%s</b></p>"
        "<p><a href='/'>Back</a></p>"
        "</body></html>",
        oldAddr, newAddr, ok ? "OK" : "FAILED (no sensor at old addr?)");
    server.send(ok ? 200 : 500, "text/html", msg);
    pollInterval.expire();
}

void handleNotFound() {
    server.send(404, "text/plain", "Not found: " + server.uri());
}

// ── setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("Setup: " MDNS_NAME);

    // RS485 UART on UART1; start in receive mode
    Serial485.begin(MODBUS_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, RS485_RX_ENABLE);

    for (int i = 0; i < NUM_SENSORS; i++) {
        sensorPresent[i]     = false;
        sensorValue[i]       = 0;
        sensorUnit[i]        = 0;
        sensorDecimals[i]    = 0;
        sensorConfigValid[i] = false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setSleep(false);
    Serial.println("Connecting to WiFi...");
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        Serial.println("Connection failed, rebooting...");
        delay(5000);
        ESP.restart();
    }
    Serial.print("IP: "); Serial.println(WiFi.localIP());

    if (MDNS.begin(MDNS_NAME)) {
        Serial.println("mDNS started: " MDNS_NAME);
    }

    // Discover the rainpump server via mDNS (same pattern as raingauge32 in v2rain.cpp)
    int n = MDNS.queryService("http", "tcp");
    for (int i = 0; i < n; i++) {
        Serial.printf("  mDNS http: %s (%s:%d)\n",
            MDNS.hostname(i).c_str(),
            MDNS.IP(i).toString().c_str(),
            MDNS.port(i));
        if (MDNS.hostname(i) == "rainpump" || MDNS.hostname(i) == "rainpump.local") {
            serverBaseUrl.remove(0);
            serverBaseUrl += "http://";
            serverBaseUrl += MDNS.IP(i).toString();
            serverBaseUrl += ":";
            serverBaseUrl += MDNS.port(i);
            serverBaseUrl += "/update";
            Serial.print("rainpump found: "); Serial.println(serverBaseUrl);
        }
    }

    ArduinoOTA.setHostname(MDNS_NAME);
    ArduinoOTA.setPassword(OTA_PASSWD);
    ArduinoOTA.onStart([]() { Serial.println("OTA start"); });
    ArduinoOTA.onEnd([]()   { Serial.println("\nOTA end"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA: %u%%\r", progress / (total / 100));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error[%u]\n", error);
    });
    ArduinoOTA.begin();

    server.on("/",        handleRoot);
    server.on("/setaddr", handleSetAddr);
    server.onNotFound(handleNotFound);
    server.begin();
    MDNS.addService("http", "tcp", 80);
    Serial.println("HTTP server started");

    updateSensors();
    pollInterval.restart();
}

// ── loop ──────────────────────────────────────────────────────────────────────

void loop() {
    ArduinoOTA.handle();
    server.handleClient();
    if (pollInterval.isExpired()) {
        pollInterval.repeat();
        updateSensors();
    }
}

#endif /* RAINGAUGE485 */
