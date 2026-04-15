#pragma once
// IPList.h — Optional: assign friendly names and device types to known IPs.
// Maps the last octet of your IP address (e.g. 192.168.1.X) to a display name
// and icon type.
//
// This file is intentionally left empty — you do not need to edit it.
// The easiest way to name your devices is through the web provisioning portal:
//   1. Flash the sketch
//   2. Connect to the "LANMon-Setup" WiFi hotspot on first boot
//   3. Open 192.168.4.1 in a browser and enter your WiFi credentials
//   4. Add device names in the portal — they are saved to NVS and persist across reboots
//
// Alternatively, you can populate this table at compile time:
//
// static const IPEntry IP_TABLE[] = {
//   //  host   name           type
//   {  254,   "Router",      ROUTER  },   // 192.168.1.254
//   {  100,   "My Laptop",   LAPTOP  },   // 192.168.1.100
//   {  101,   "My Phone",    PHONE   },   // 192.168.1.101
// };
// static const int IP_TABLE_COUNT = sizeof(IP_TABLE) / sizeof(IPEntry);
//
// Available types: PHONE, LAPTOP, DESKTOP, TV, ROUTER, NAS, DEV_PI,
//                  PRINTER, TABLET, SPEAKER, DEV_ESP32, CONSOLE, DEV_UNKNOWN
//
// NOTE: If you add a compile-time table above, remove the two lines below.

static const IPEntry IP_TABLE[] = {};
static const int IP_TABLE_COUNT = 0;
