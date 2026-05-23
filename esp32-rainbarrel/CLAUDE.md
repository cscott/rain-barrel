# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Upload Commands

```bash
pio run                              # Build default envs (rainpump32, raingauge32)
pio run -t upload                    # OTA upload to all default envs
pio run -e rainpump32 -t upload      # OTA upload rainpump only
pio run -e raingauge32 -t upload     # OTA upload raingauge only
pio run -e native                    # Build desktop/native version
pio test -e native                   # Run unit tests (desktop only)
.pio/build/native/program test/test_desktop/sample-20230729.h  # Run SMRT-Y decoder manually
```

OTA upload targets use `--auth=goaway` and resolve via mDNS: `rainpump.local` and `raingauge.local`. WiFi credentials are in `include/config.h`.

## Architecture Overview

This is a PlatformIO project controlling a rain barrel irrigation system using a pair of ESP32 Feather boards.

### Two Devices, One Source File

`src/v2rain.cpp` compiles to **either** `rainpump32` or `raingauge32` using compile-time defines (`RAINPUMP_V2` vs `RAINGAUGE_V2`). Never define both. The two devices communicate:

- **raingauge32** reads three barrel water levels via ADS1115 ADC and periodically HTTP-GETs the pump at `/update?level1=...&level2=...&level3=...`
- **rainpump32** hosts a WebServer on port 80, controls a motorized water valve (city vs rain), runs the pump, and aggregates data from flow meters and SMRT-Y soil sensors

Both devices publish to Home Assistant via MQTT (192.168.198.32, using the `arduino-home-assistant` library fork).

### Hardware Components (V2)

| Component | Role |
|-----------|------|
| ST7529_LCD (240×128, 4-level grayscale) | Transflective display via software SPI |
| CAP1298 | Capacitive touch (4 buttons A/B/C/D + proximity) over I2C |
| ADP1650 | LED backlight driver over I2C |
| ADS1115 (raingauge only) | 16-bit ADC for 3 ultrasonic water level sensors |
| Flow meter ATtiny85s | I2C slaves (addr 0x08+offset) counting irrigation flow pulses |
| SMRT-Y Snitches (Pi Pico) | I2C slaves (addr 0x24/0x25) decoding proprietary soil sensor data |

### SMRT-Y Snitch Subsystem

`src/smrtysnitch.cpp` runs on **Raspberry Pi Pico** (not ESP32). It acts as an I2C slave that decodes the proprietary SMRT-Y soil moisture sensor RF protocol using two RP2040 PIO state machines defined in `src/smrty.pio`:
- `smrty`: records transition timestamps in the RX FIFO
- `watchdog`: sends a timeout marker if the pin stays idle too long

**Important**: `smrty.pio.h` (the PIO binary) must be compiled manually using [wokwi pioasm](https://wokwi.com/tools/pioasm) — PlatformIO does not auto-compile `.pio` files for this target.

The decode logic is in `lib/smrty_decode/smrty_decode.c` — a pure C library that also compiles natively for desktop tests. Test samples in `test/test_desktop/sample-*.h` were generated from Saleae logic analyzer CSV exports using `boildown.py`.

### Display System

The ST7529 LCD uses 4 grayscale levels. Colors are mapped:
- `COLOR1=0x00` (white), `COLOR2=0x7F` (light), `COLOR3=0xBF` (dark), `COLOR4=0xFF` (black)

Custom bitmap fonts are in `include/bluebold-*.h` and `include/bluehigh-*.h`. To regenerate: `mkfont.sh` calls `Adafruit-GFX-Library/fontconvert/fontconvert` (must be built separately from a sibling directory).

### Timing Intervals

All `AsyncDelay` intervals intentionally use small prime-number offsets (e.g., `11*SECONDS_MS + 3`) to prevent multiple timers from firing simultaneously and stacking I2C/WiFi/display workload.

### State Machine (rainpump)

Three pump states: `STATE_CITY=0`, `STATE_AUTO=1`, `STATE_RAIN=2`. In AUTO, the pump switches from rain to city when any barrel falls below `WATER_ALARM_LOW` (5%), and back to rain after recovering to `WATER_ALARM_LOW + WATER_ALARM_HYSTERESIS` (8%). The motorized valve only runs for 1 minute when switching (to avoid overheating), then idles. An anti-bounce timer inhibits the pump for 15 seconds after the pressure switch opens.

### Other Targets

- `flowmeter1/2/3`: ATtiny85 (Digispark) pulse counters for irrigation flow meters
- `snitch1/2`: Raspberry Pi Pico SMRT-Y decoders (manual upload via BOOTSEL)
- `native`: Desktop build for testing `smrty_decode` only; uses `src/desktop.c`
- Various `*tester` envs: ESP8266 Huzzah development/testing boards
