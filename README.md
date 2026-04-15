# LANMon

A local network presence monitor for the **Xiaozhi ESP32-C3 (Xmini-C3)** board. LANMon scans your LAN using ARP, displays online devices on a 128×64 OLED with device-type icons, and identifies unknown devices by MAC OUI. All configuration is done through a built-in web portal — no code changes required.

![Board: Xmini-C3](https://img.shields.io/badge/board-Xmini--C3-blue)
![IDE: Arduino](https://img.shields.io/badge/IDE-Arduino-teal)

---

## Features

- ARP scans the entire /24 subnet every 30 seconds
- Displays up to 3 devices per page with pixel-art icons (phone, laptop, router, Pi, etc.)
- Auto-identifies unknown devices by MAC OUI (manufacturer lookup)
- Named device table editable live via web portal — saved to NVS, survives reboots
- NeoPixel LED status: green = all online, amber = partial / subnet conflict, red = none
- WiFi signal strength and scan progress bars on OLED
- 5-minute screensaver (OLED + LED off)
- Subnet conflict detection — warns if device table was built on a different subnet
- No cloud, no app — entirely local

---

## Hardware

| Component | Detail |
|-----------|--------|
| Board | Xiaozhi ESP32-C3 (Xmini-C3) |
| Display | SSD1306 128×64 OLED (I2C) |
| LED | Onboard NeoPixel |
| SDA | GPIO 3 |
| SCL | GPIO 4 |
| NeoPixel | GPIO 2 |
| BOOT btn | GPIO 9 |

---

## Required Libraries

Install both via Arduino Library Manager:

- **U8g2** by olikraus
- **Adafruit NeoPixel** by Adafruit

---

## Arduino IDE Settings

| Setting | Value |
|---------|-------|
| Board | ESP32C3 Dev Module |
| Flash Mode | DIO |
| CPU Frequency | 80 MHz |
| USB CDC On Boot | Enabled |

---

## First-Time Setup

1. Flash the sketch — the board will create a **"LANMon-Setup"** WiFi hotspot
2. Connect your phone or laptop to that hotspot
3. Open **192.168.4.1** in a browser
4. Enter your home WiFi SSID and password
5. Optionally add device names in the portal
6. Save — the board connects to your WiFi and begins scanning

Credentials and device names are stored in NVS and survive power cycles.

**To reset WiFi:** hold BOOT while powering on → returns to hotspot setup mode.

---

## Controls

| Action | Result |
|--------|--------|
| Short press BOOT | Next page |
| Hold BOOT 1 s+ | Force rescan |

---

## Naming Your Devices

The easiest method is the **web portal** — no recompiling needed. Connect to LANMon's IP in a browser at any time (shown on the OLED) to add or edit device names.

Alternatively, pre-populate `IPList.h` at compile time — see the comments in that file for the format. Available device types:

`PHONE` `LAPTOP` `DESKTOP` `TV` `ROUTER` `NAS` `DEV_PI` `PRINTER` `TABLET` `SPEAKER` `DEV_ESP32` `CONSOLE` `DEV_UNKNOWN`

---

## License

MIT — do what you like, no warranty.
