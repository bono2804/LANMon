#pragma once

// Sizes used by structs — defined here so structs can reference them
#define NAME_LEN     16
#define MAX_DEVICES  30

// Device type used in structs and icon lookup
enum DevType {
  PHONE, LAPTOP, DESKTOP, TV, ROUTER, NAS, DEV_PI,
  PRINTER, TABLET, SPEAKER, DEV_ESP32, CONSOLE, DEV_UNKNOWN
};

struct IPEntry {
  uint8_t host;        // last octet of IP (e.g. 254 for 192.168.1.254)
  char    name[NAME_LEN];
  DevType type;
};

// MAC OUI (first 3 bytes) to device type mapping — used for auto-identification
// when a device's IP is not in the user's named table.
struct OUIEntry {
  uint8_t oui[3];
  DevType type;
  char    name[NAME_LEN];
};

struct Device {
  uint8_t  mac[6];
  uint8_t  ip[4];
  char     name[NAME_LEN];
  DevType  type;
  bool     online;
  bool     nameResolved;
  uint32_t lastSeenMs;
};
