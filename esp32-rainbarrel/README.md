To build:
 pio run
To upload:
 pio run -t upload
or
 pio run -e rainpump -t upload
 pio run -e raingauge -t upload

See:
* https://community.platformio.org/t/board-definition-for-adafruit-magtag/20747/2
* https://community.platformio.org/t/how-to-use-multiple-target-boards-with-same-project/16041/5
* https://community.platformio.org/t/esp32-ota-using-platformio/15057/4

For the SMRT-Y decoder test:

 pio run -e native
 .pio/build/native/program test/test_desktop/sample-20210706.h

(The `sample-input.txt` file was created from the Saleae CSV export of the
comparator channel with the `boildown.py` script.)
