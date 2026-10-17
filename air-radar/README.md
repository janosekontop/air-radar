# Air Radar

A live aircraft radar for the **Waveshare ESP32-S3-LCD-1.3**, viewed through a
prism enclosure. It can fetch aircraft over Wi-Fi or receive them from a computer
over USB. Each owner supplies their own location and network settings.

**Release:** 3.0.0 · **Display:** 240 × 240 ST7789 · **Memory:** 16 MB flash / 8 MB PSRAM

The included firmware is for this specific board and prism mounting:
180° display rotation with horizontal reflection. Other ESP32 boards need a port.

## First-time setup: a preprogrammed radar

You do not need a GitHub account, API key, hosted website or computer for Wi-Fi setup.

1. Power the radar from a USB power supply. A fresh device displays **SET UP YOUR RADAR**.
2. On your phone, join the **Air-Radar-xxxxxx** Wi-Fi network shown on the screen.
   Use the unique access key displayed on the radar. Stay connected if the phone
   warns that this temporary network has no internet.
3. A setup page may open automatically. If it does not, open a browser and enter
   **http://192.168.4.1/** while still connected to the radar's hotspot.
4. Choose **Direct Wi-Fi**. Enter your radar centre's latitude and longitude from
   a map, plus your **2.4 GHz** Wi-Fi network name and password. Click **Save to radar**.
5. Wait for live traffic. The setup hotspot closes after Wi-Fi has been stable for
   three minutes. Leave USB power connected; a computer is no longer required.

The board has no GPS. Update its centre if you move it. Automatic browser location
is available only in supported browsers when using the computer setup route below;
phone setup over the radar hotspot uses manually entered map coordinates.

## Setup from a computer / USB data mode

1. Install [Python 3.10 or newer](https://www.python.org/downloads/).
2. Download this repository using **Code → Download ZIP**, then extract it.
3. On Windows, run **Install Companion.cmd** once.
4. Connect the radar with a **USB data cable**, close other serial monitors and run
   **Start Setup.cmd**. It opens **http://127.0.0.1:8765/** in your browser.
5. Set a location and choose **Direct Wi-Fi** or **USB from computer**, then save.

In USB mode, the computer supplies internet access; a Wi-Fi password is unnecessary.
Keep the computer online, the USB cable connected and the companion running.
Run **Start Setup.cmd** again next time to resume the USB feed. Closing the browser
tab alone does not stop the companion; use **Device options & help → Close companion**.

For macOS/Linux, or an explicit serial port:

```sh
python -m venv .venv
# Activate .venv using your shell's activation command.
python -m pip install -r tools/requirements.txt
python tools/wifi_setup.py --open-browser
# Add --port COM5 on Windows, or your actual /dev/... port on macOS/Linux, if needed.
```

On Windows, **Start Setup.ps1 -Port COM5** also selects a particular board.
USB serial drivers and serial-port permissions depend on your operating system.

## Where is the setup website hosted?

| Setup route | Who serves the page? | Browser address | What must stay running? |
| --- | --- | --- | --- |
| Phone joins radar hotspot | The ESP32 itself | `http://192.168.4.1/` | Radar power; the phone is needed only during setup |
| Computer connects by USB | Python companion on that computer | `http://127.0.0.1:8765/` | Companion during setup, and throughout USB-fed operation |

Plugging a USB cable into a computer does **not** automatically launch software or
open its browser. Run the companion for computer setup. On a phone, automatic
captive-portal opening depends on the OS; the manual address always remains the fallback.

GitHub distributes the source and firmware. It does not host the credential form
or receive your Wi-Fi password. Publishing `setup.html` with GitHub Pages would
not replace either local server or connect to the board.

## Credentials and privacy

Wi-Fi settings and the radar centre are saved on the ESP32. The setup page and
companion do not write passwords to PC settings files, return passwords in status
responses, or send them to a hosted service. There is no default home location or
shared setup password in the distributed firmware.

The board stores settings in **unencrypted NVS**; physical flash access can reveal
credentials. The aircraft provider receives the selected query coordinates and
requesting internet address. See [SECURITY.md](SECURITY.md) for the actual protections
and limitations.

## Installing firmware on a blank board

Use only the supported 16 MB Waveshare board. Install the companion dependencies
above, then install the optional flashing tool:

```sh
python -m pip install esptool==4.5.1
```

- **Flash Firmware.cmd** installs the checked images in `Firmware/` while preserving settings.
- **Factory Flash for New Owner.cmd** erases **all flash**, including saved credentials
  and other projects' stored data, then installs fresh firmware. Use this when
  transferring a previously used device to a different owner.

Close the companion before flashing so it releases the serial port.

## Build and test

Install the pinned development tools in a Python environment:

```sh
python -m pip install -r tools/requirements-dev.txt
python -m unittest discover -s tools -p "test_*.py" -v
python tools/verify_firmware.py
python -m platformio run
```

To upload a source build, close the companion and run:

```sh
python -m platformio run --target upload --upload-port COM5
```

Replace `COM5` with the board's actual port. The PlatformIO configuration pins the
platform and firmware libraries. GitHub Actions checks the Python tests, packaged
firmware checksums and a source build; it cannot test physical Wi-Fi or the LCD.

[RELEASE-VERIFICATION.json](RELEASE-VERIFICATION.json) records the hardware and
internal checks for the shipping release. The USB onboarding walkthrough and live
prism display were also confirmed on 2026-10-17. A separate phone captive-portal
walkthrough has not been completed. These are internal checks, not security certification.

## Repository contents

- `src/`, `include/`, `platformio.ini`: firmware and hardware profile.
- `data/setup.html`: the shared local setup page; public TLS roots are also in `data/`.
- `tools/`: companion, USB feed, tests, build and flashing helpers.
- `Firmware/`: ready-to-flash images and SHA-256 manifest.
- `Licenses/`, `THIRD-PARTY-NOTICES.md`: dependency notices.
- `docs/PUBLISHING.md`: publishing this clean repository from your own GitHub account.

Runtime caches, logs, configured-device dumps and private backups are excluded.
Never add a full-flash or NVS dump to a public repository.

## Data and licenses

Traffic and estimated routes come from [ADS-B.lol](https://www.adsb.lol/), subject
to receiver coverage, service availability and its data license. The display is a
hobby project and must not be used for navigation or safety decisions. Routes
are estimates rather than confirmed flight plans.

Third-party components retain their own licenses; see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). No separate license for the original
project code has been selected. Choose one deliberately before describing a public
repository as open source.
