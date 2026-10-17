# Third-party notices

Traffic is supplied by [ADS-B.lol](https://www.adsb.lol/). Its
[API documentation](https://www.adsb.lol/docs/open-data/api/) lists the data
license as **ODbL 1.0**. Preserve attribution and review the data license when
redistributing data or a derived database. The public service provides no
guarantee of uninterrupted coverage. Aircraft routes use its public
[standing-data endpoint](https://vrs-standing-data.adsb.lol/).

Firmware dependencies are pinned in `platformio.ini`: Arduino ESP32 2.0.17
(through PlatformIO espressif32 6.10.0), TFT_eSPI 2.5.43, LVGL 8.4.0 and
ArduinoJson 6.21.5. They retain their own copyright and license terms.
Notices for the display/UI/JSON libraries are included under `Licenses/`.
Arduino ESP32/framework and Espressif SDK components retain their upstream
LGPL/Apache and third-party notices; see
https://github.com/espressif/arduino-esp32 and https://github.com/espressif/esp-idf.

The Mozilla/certifi CA roots are accompanied by `data/LICENSE.certifi`.
The optional PC companion uses pyserial 3.5 (BSD license).
The optional programming tool is esptool 4.5.1 (GPL-2.0-or-later), installed
separately rather than bundled. This package does not relicense third-party code.
