/*
 * LANMon — Local network presence display with auto-discovery
 * Xiaozchi ESP32-C3 (Xmini-C3)
 *
 * Required libraries: U8g2, Adafruit NeoPixel
 *
 * Device names loaded from IPList.h (compiled-in defaults) and then from NVS.
 * Edit via the provisioning portal — any saved table persists across reboots.
 * Unknown IPs (not in table) shown as full IP with unknown icon.
 *
 * First boot: board creates "LANMon-Setup" WiFi hotspot.
 * Connect phone/laptop to it, open browser → 192.168.4.1.
 * Enter WiFi credentials and optionally edit the device list.
 * Credentials + device table saved to NVS and survive reboots/power cycles.
 * To reset WiFi: hold BOOT while powering on → returns to hotspot setup.
 *
 * BOOT short press → next page
 * BOOT hold 1s+   → force rescan
 *
 * Arduino IDE: ESP32C3 Dev Module, DIO, 80MHz, USB CDC On Boot Enabled
 */

extern "C" void esp_brownout_init() {}

// NOTE: LANMon.h must be the last #include so it is processed before
// Arduino IDE inserts auto-generated function prototypes.
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Adafruit_NeoPixel.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include "esp_netif.h"
#include "LANMon.h"
#include "IPList.h"
#include "OUIList.h"

// ─── CONFIG ────────────────────────────────────────────────────────────────────
#define SCAN_INTERVAL_MS    30000
#define ARP_FLOOD_DELAY_MS      8
#define ARP_REPLY_WAIT_MS    2000
#define LONG_PRESS_MS        5000
#define DEVICES_PER_PAGE        3
#define MAX_IP_ENTRIES         40   // max runtime device-name table entries

#define SCREENSAVER_MS   (5UL * 60UL * 1000UL)  // 5 minutes inactivity

// ─── PINS ──────────────────────────────────────────────────────────────────────
#define PIN_BOOT   9
#define PIN_LED    2
#define PIN_SDA    3
#define PIN_SCL    4

// ─── DISPLAY GEOMETRY ──────────────────────────────────────────────────────────
// Blue zone: rows 28-63 (36px). 3 cells × 42px wide across 128px (+ 2px remainder).
// Each cell: 16×16 icon centred horizontally, 6px label below.
// Icon top: 28 + 6 padding = 34. Label baseline: 34 + 16 icon + 2 gap + 5 ascent = 57.
#define ZONE_BLUE_TOP    28
#define ICON_SZ           8     // source bitmap size
#define ICON_SCALE        2     // pixel-doubling → 16×16 on screen
#define CELL_W           42     // cell width (3 × 42 + 2 gaps = 128)
#define ICON_TOP         34     // y of scaled icon top edge
#define LABEL_Y          57     // y baseline of label text

// ─── ICONS (8×8, MSB = left pixel) ────────────────────────────────────────────
static const uint8_t ICON_PHONE[8]   = {0x3C,0x42,0x42,0x42,0x5A,0x42,0x42,0x3C};
static const uint8_t ICON_LAPTOP[8]  = {0x7E,0x42,0x42,0x7E,0x00,0xFF,0x00,0x00};
static const uint8_t ICON_DESKTOP[8] = {0x7E,0x42,0x42,0x7E,0x18,0x3C,0x66,0x00};
static const uint8_t ICON_TV[8]      = {0xFF,0x81,0x81,0x81,0xFF,0x18,0x3C,0x00};
static const uint8_t ICON_ROUTER[8]  = {0x24,0x24,0x00,0x7E,0xFF,0x81,0x81,0xFF};
static const uint8_t ICON_NAS[8]     = {0xFE,0xFE,0x82,0xFE,0x82,0xFE,0x82,0xFE};
static const uint8_t ICON_PI[8]      = {0x3C,0x7E,0xDB,0xFF,0xFF,0xDB,0x7E,0x3C};
static const uint8_t ICON_PRINT[8]   = {0x3C,0x7E,0xFF,0x81,0xFF,0x00,0x3C,0x3C};
static const uint8_t ICON_TABLET[8]  = {0xFF,0x81,0x81,0x99,0x81,0xFF,0x10,0x00};
static const uint8_t ICON_SPEAK[8]   = {0x08,0x14,0x3E,0x7F,0x7F,0x3E,0x14,0x08};
static const uint8_t ICON_ESP[8]     = {0x3C,0xFF,0x81,0xBD,0xBD,0x81,0xFF,0x3C};
// Gamepad silhouette: shoulder bumpers, body, detail row, grips
static const uint8_t ICON_CONSOLE[8] = {0x66,0xFF,0xBD,0xFF,0xFF,0x66,0x42,0x00};
// Large question mark — shown for any device not in the named table or OUI list
static const uint8_t ICON_UNKWN[8]   = {0x38,0x44,0x04,0x08,0x10,0x10,0x00,0x10};

// Returns icon bitmap for a device type value (passed as int to avoid
// user-defined type in signature, which causes Arduino prototype issues).
const uint8_t* getIcon(int t) {
  switch (t) {
    case PHONE:   return ICON_PHONE;
    case LAPTOP:  return ICON_LAPTOP;
    case DESKTOP: return ICON_DESKTOP;
    case TV:      return ICON_TV;
    case ROUTER:  return ICON_ROUTER;
    case NAS:     return ICON_NAS;
    case DEV_PI:  return ICON_PI;
    case PRINTER: return ICON_PRINT;
    case TABLET:  return ICON_TABLET;
    case SPEAKER:   return ICON_SPEAK;
    case DEV_ESP32:  return ICON_ESP;
    case CONSOLE:    return ICON_CONSOLE;
    default:         return ICON_UNKWN;
  }
}

// ─── DYNAMIC DEVICE TABLE ──────────────────────────────────────────────────────
static Device devices[MAX_DEVICES];
static int    numDevices = 0;

// ─── RUNTIME IP TABLE ──────────────────────────────────────────────────────────
// Loaded from NVS on boot (falls back to compiled-in IP_TABLE if no NVS data).
// Editable via provisioning portal — changes persist across reboots.
static IPEntry runtimeTable[MAX_IP_ENTRIES];
static int     runtimeTableCount = 0;

// ─── OBJECTS & STATE ───────────────────────────────────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, PIN_SCL, PIN_SDA, U8X8_PIN_NONE);
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);
static WebServer   lanServer(80);

static Preferences prefs;
static String      wifiSSID;
static String      wifiPass;

static int      currentPage    = 0;
static uint32_t lastScanMs     = 0;
static bool     bootWasDown    = false;
static uint32_t bootDownMs     = 0;
static bool     ssaverActive   = false;
static uint32_t lastActivityMs = 0;
static bool     bstEnabled     = false;  // loaded from NVS; set during provisioning
static uint8_t  ssaverDimPct   = 50;     // 0–100 in steps of 10; loaded from NVS

// Set true when the current WiFi subnet differs from the subnet the IP table
// was built for — displayed as a "FIX IP LIST" warning until resolved.
static bool    subnetConflict  = false;
static uint8_t conflictOldSub[3];   // subnet stored when table was last saved
static uint8_t conflictNewSub[3];   // subnet of current network

// ─── LED ───────────────────────────────────────────────────────────────────────
void setLed(uint8_t r, uint8_t g, uint8_t b) {
  led.setPixelColor(0, led.Color(r, g, b));
  led.show();
}

void updateLedStatus() {
  if (ssaverActive)    { setLed( 0,  0,  0); return; }  // off during UI sleep
  if (subnetConflict)  { setLed(40, 15,  0); return; }  // amber = IP table conflict
  int online = 0;
  for (int i = 0; i < numDevices; i++) if (devices[i].online) online++;
  if      (online == 0)          setLed(40,  0,  0);
  else if (online == numDevices) setLed( 0, 40,  0);
  else                           setLed(40, 15,  0);
}

// ─── LOOKUP HELPERS ────────────────────────────────────────────────────────────
// Returns int (index or -1) so no user types appear in signatures.

int lookupIP(uint8_t host) {
  for (int i = 0; i < runtimeTableCount; i++)
    if (runtimeTable[i].host == host) return i;
  return -1;
}

// Returns OUI_TABLE index for the given MAC, or -1 if not found.
// Only the first 3 bytes (OUI) are compared.
int lookupOUI(const uint8_t* mac) {
  for (int i = 0; i < OUI_TABLE_COUNT; i++) {
    if (OUI_TABLE[i].oui[0] == mac[0] &&
        OUI_TABLE[i].oui[1] == mac[1] &&
        OUI_TABLE[i].oui[2] == mac[2])
      return i;
  }
  return -1;
}

// Keyed on IP host octet — one slot per IP address.
// This prevents duplicates when a device has multiple MACs (e.g. router)
// and handles phones that randomise their MAC between scans.
int findOrCreate(uint8_t host) {
  for (int i = 0; i < numDevices; i++)
    if (devices[i].ip[3] == host) return i;
  if (numDevices >= MAX_DEVICES) return -1;
  int idx = numDevices++;
  memset(&devices[idx], 0, sizeof(Device));
  devices[idx].type = DEV_UNKNOWN;
  return idx;
}

// ─── ARP ───────────────────────────────────────────────────────────────────────
// lwIP internals (etharp_request, etharp_find_addr) must run inside the TCPIP
// task. esp_netif_tcpip_exec() dispatches a callback into that task and blocks
// until it returns — the only safe way to call these from the Arduino task.
// Callbacks return int (= esp_err_t = int32_t on this platform) to avoid
// esp_err_t appearing in IDE-generated prototypes before headers are processed.

struct ArpReqCtx {
  struct netif* nif;
  ip4_addr_t    addr;
};

struct ArpCacheEntry { uint8_t mac[6]; bool valid; };
struct ArpCacheCtx {
  struct netif* nif;
  uint8_t       sub[3];
  ArpCacheEntry entries[254];   // entries[host-1] for host 1..254
};

static int cbGetNif(void* ctx) {
  *(struct netif**)ctx = netif_list;
  return 0;
}

static int cbClearArp(void* ctx) {
  struct netif* nif = *(struct netif**)ctx;
  if (nif) etharp_cleanup_netif(nif);
  return 0;
}

static int cbArpRequest(void* ctx) {
  ArpReqCtx* c = (ArpReqCtx*)ctx;
  etharp_request(c->nif, &c->addr);
  return 0;
}

static int cbReadCache(void* ctx) {
  ArpCacheCtx* c = (ArpCacheCtx*)ctx;
  if (!c->nif) return 0;
  for (int host = 1; host < 255; host++) {
    ip4_addr_t addr;
    IP4_ADDR(&addr, c->sub[0], c->sub[1], c->sub[2], (uint8_t)host);
    struct eth_addr*  eth = nullptr;
    const ip4_addr_t* ip  = nullptr;
    ArpCacheEntry& e = c->entries[host - 1];
    e.valid = false;
    if (etharp_find_addr(c->nif, &addr, &eth, &ip) >= 0 && eth) {
      memcpy(e.mac, eth->addr, 6);
      e.valid = true;
    }
  }
  return 0;
}


// ─── DISPLAY ───────────────────────────────────────────────────────────────────
// Yellow zone (rows 0-13): two filled bars — text is unreadable here due to
// the two-colour panel's alternating yellow/black row pattern.
// Seam (rows 15-27): page navigation dots — filled shapes render fine here.
// Blue zone (rows 28-63): device list — every row solid, all rendering clean.

void drawBars(bool scanning, int scanProgress) {
  // Top bar — WiFi signal strength (rows 0-5)
  // Filled frame + fill; RSSI -90 (weakest) to -40 (strongest) mapped to 0-126px
  u8g2.drawFrame(0, 0, 128, 6);
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int sw = (int)constrain(map(rssi, -90, -40, 0L, 126L), 0L, 126L);
    if (sw > 0) u8g2.drawBox(1, 1, sw, 4);
  }

  // Bottom bar — refresh countdown / scan progress (rows 8-13)
  // Fills left-to-right: time elapsed toward next 30s scan, or scan progress
  u8g2.drawFrame(0, 8, 128, 6);
  int rw = 0;
  if (scanning) {
    rw = (int)((long)scanProgress * 126 / 254);
  } else if (lastScanMs > 0) {
    uint32_t elapsed = millis() - lastScanMs;
    rw = (int)min((long)elapsed * 126 / SCAN_INTERVAL_MS, 126L);
  }
  if (rw > 0) u8g2.drawBox(1, 9, rw, 4);
}

// Draws an 8×8 bitmap pixel-doubled to 16×16 (each source pixel → 2×2 block).
void drawIcon2x(int x, int y, const uint8_t* bmp) {
  for (int row = 0; row < ICON_SZ; row++)
    for (int col = 0; col < ICON_SZ; col++)
      if (bmp[row] & (0x80 >> col))
        u8g2.drawBox(x + col * ICON_SCALE, y + row * ICON_SCALE, ICON_SCALE, ICON_SCALE);
}

void drawPageDots(int totalPages) {
  if (totalPages <= 1) return;
  int x = (128 - (totalPages * 5 - 2)) / 2;
  for (int p = 0; p < totalPages; p++) {
    if (p == currentPage) u8g2.drawBox  (x, 21, 3, 3);
    else                  u8g2.drawFrame(x, 21, 3, 3);
    x += 5;
  }
}

void drawDeviceList() {
  // Online-only index — offline devices kept in table for serial but not displayed.
  // Routers are collected separately and appended last.
  int onlineIdx[MAX_DEVICES];
  int onlineCount = 0;
  int routerIdx[MAX_DEVICES];
  int routerCount = 0;
  for (int i = 0; i < numDevices; i++) {
    if (!devices[i].online) continue;
    if (devices[i].type == ROUTER) routerIdx[routerCount++] = i;
    else                           onlineIdx[onlineCount++] = i;
  }
  for (int i = 0; i < routerCount; i++) onlineIdx[onlineCount++] = routerIdx[i];

  int totalPages = max(1, (onlineCount + DEVICES_PER_PAGE - 1) / DEVICES_PER_PAGE);
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // 3 cell x-positions: 0, 43, 86  (42px each, 1px gap, totals 128px)
  static const int CELL_X[3] = { 0, 43, 86 };

  int start = currentPage * DEVICES_PER_PAGE;
  u8g2.setFont(u8g2_font_4x6_tr);
  for (int i = 0; i < DEVICES_PER_PAGE; i++) {
    int oi = start + i;
    if (oi >= onlineCount) break;
    Device& d = devices[onlineIdx[oi]];
    int cx = CELL_X[i];

    // 16×16 icon centred in cell
    int ix = cx + (CELL_W - ICON_SZ * ICON_SCALE) / 2;
    drawIcon2x(ix, ICON_TOP, getIcon(d.type));

    // Label centred under icon, truncated to fit cell width
    char lbl[11];
    strncpy(lbl, d.name, 10);
    lbl[10] = '\0';
    int lw = u8g2.getStrWidth(lbl);
    u8g2.drawStr(cx + (CELL_W - lw) / 2, LABEL_Y, lbl);
  }
  drawPageDots(totalPages);
}

// Shown in place of the device list when the IP table was built for a different
// subnet. Seam zone shows the fix instruction; blue zone shows old vs new subnet.
void drawConflictWarning() {
  char buf[22];
  u8g2.setFont(u8g2_font_4x6_tr);
  // Seam: instruction (filled text renders fine here)
  u8g2.drawStr(4, 24, "Reboot + hold BOOT");
  // Blue zone: prominent heading + subnet detail
  u8g2.setFont(u8g2_font_5x7_tr);
  int hw = u8g2.getStrWidth("FIX IP LIST");
  u8g2.drawStr((128 - hw) / 2, 38, "FIX IP LIST");
  u8g2.setFont(u8g2_font_4x6_tr);
  snprintf(buf, sizeof(buf), "Was: %d.%d.%d.x",
    conflictOldSub[0], conflictOldSub[1], conflictOldSub[2]);
  u8g2.drawStr(2, 49, buf);
  snprintf(buf, sizeof(buf), "Now: %d.%d.%d.x",
    conflictNewSub[0], conflictNewSub[1], conflictNewSub[2]);
  u8g2.drawStr(2, 57, buf);
}

void drawScreen(bool scanning, int scanProgress) {
  u8g2.clearBuffer();
  drawBars(scanning, scanProgress);
  if (subnetConflict) drawConflictWarning();
  else                drawDeviceList();
  // Board IP — permanently centred at bottom row
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    u8g2.setFont(u8g2_font_4x6_tr);
    int w = u8g2.getStrWidth(ipStr);
    u8g2.drawStr((128 - w) / 2, 63, ipStr);
  }
  u8g2.sendBuffer();
}

// ─── SCREENSAVER ───────────────────────────────────────────────────────────────
// Yellow zone: one thick signal-strength bar (rows 0-11).
// Blue zone: large 24 h clock centred vertically.
void drawScreensaver() {
  u8g2.clearBuffer();

  // Single thick signal bar filling the yellow zone
  u8g2.drawFrame(0, 0, 128, 12);
  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int sw   = (int)constrain(map(rssi, -90, -40, 0L, 126L), 0L, 126L);
    if (sw > 0) u8g2.drawBox(1, 1, sw, 10);
  }

  // 24 h clock — large font filling the blue zone (rows 28-63)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    u8g2.setFont(u8g2_font_inb27_mn);
    int w = u8g2.getStrWidth(buf);
    u8g2.drawStr((128 - w) / 2, 59, buf);
  } else {
    // NTP not yet synced
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(22, 50, "Syncing time...");
  }

  u8g2.sendBuffer();
}

// ─── SCAN ──────────────────────────────────────────────────────────────────────
void runScan() {
  if (!ssaverActive) setLed(0, 0, 40);
  // Keep previous online state intact during the scan so the display
  // continues showing the last known device list throughout the flood
  // and reply-wait. Status is cleared just before results are applied.

  // Get the lwIP netif pointer from inside the TCPIP task
  struct netif* nif = nullptr;
  esp_netif_tcpip_exec((esp_netif_callback_fn)cbGetNif, &nif);
  if (!nif) { updateLedStatus(); return; }

  IPAddress local = WiFi.localIP();
  uint8_t sub[3]  = { local[0], local[1], local[2] };
  uint8_t myHost  = local[3];

  // Flush stale ARP cache before flooding so only devices that reply to THIS
  // scan appear as online — without this, disconnected devices linger for
  // lwIP's default cache expiry (~20 minutes).
  esp_netif_tcpip_exec((esp_netif_callback_fn)cbClearArp, &nif);

  // ARP flood — each request dispatched into TCPIP task via esp_netif_tcpip_exec
  ArpReqCtx reqCtx;
  reqCtx.nif = nif;
  for (int host = 1; host < 255; host++) {
    if (host == myHost) continue;
    IP4_ADDR(&reqCtx.addr, sub[0], sub[1], sub[2], (uint8_t)host);
    esp_netif_tcpip_exec((esp_netif_callback_fn)cbArpRequest, &reqCtx);
    if ((host & 0x0F) == 0) {
      if      (!ssaverActive)       drawScreen(true, host);
      else if (ssaverDimPct < 100)  drawScreensaver();
    }
    delay(ARP_FLOOD_DELAY_MS);
  }

  // Wait for replies
  uint32_t deadline = millis() + ARP_REPLY_WAIT_MS;
  while (millis() < deadline) {
    if      (!ssaverActive)       drawScreen(true, 254);
    else if (ssaverDimPct < 100)  drawScreensaver();
    delay(150);
  }

  // Read entire ARP cache in one callback inside the TCPIP task
  ArpCacheCtx cacheCtx;
  cacheCtx.nif    = nif;
  cacheCtx.sub[0] = sub[0];
  cacheCtx.sub[1] = sub[1];
  cacheCtx.sub[2] = sub[2];
  esp_netif_tcpip_exec((esp_netif_callback_fn)cbReadCache, &cacheCtx);

  // Clear online status now — results from this scan replace previous state
  for (int i = 0; i < numDevices; i++) devices[i].online = false;

  // Update device table from cache results
  for (int host = 1; host < 255; host++) {
    if (host == myHost) continue;
    ArpCacheEntry& e = cacheCtx.entries[host - 1];
    if (!e.valid) continue;
    bool allZero = true;
    for (int j = 0; j < 6; j++) if (e.mac[j]) { allZero = false; break; }
    if (allZero) continue;

    int di = findOrCreate((uint8_t)host);
    if (di < 0) continue;

    memcpy(devices[di].mac, e.mac, 6);   // update MAC (may change e.g. phone randomisation)
    devices[di].ip[0] = sub[0]; devices[di].ip[1] = sub[1];
    devices[di].ip[2] = sub[2]; devices[di].ip[3] = (uint8_t)host;
    devices[di].online     = true;
    devices[di].lastSeenMs = millis();

    // 1st priority: user's named table (NVS-backed, falls back to IPList.h)
    // 2nd priority: OUI manufacturer lookup — gives type + vendor label
    // 3rd priority: DEV_UNKNOWN — shows full IP as label, "?" icon
    int ii = lookupIP((uint8_t)host);
    if (ii >= 0) {
      strncpy(devices[di].name, runtimeTable[ii].name, NAME_LEN - 1);
      devices[di].name[NAME_LEN - 1] = '\0';
      devices[di].type = runtimeTable[ii].type;
    } else {
      int oi = lookupOUI(devices[di].mac);
      if (oi >= 0) {
        strncpy(devices[di].name, OUI_TABLE[oi].name, NAME_LEN - 1);
        devices[di].name[NAME_LEN - 1] = '\0';
        devices[di].type = OUI_TABLE[oi].type;
      } else {
        // Show just the last octet (.210) — fits the 42px cell, full IP is on serial
        snprintf(devices[di].name, NAME_LEN, ".%d", host);
        devices[di].type = DEV_UNKNOWN;
      }
    }
  }

  lastScanMs = millis();
  updateLedStatus();
  printDevices();
}

// ─── BOOT BUTTON ───────────────────────────────────────────────────────────────
void handleButton() {
  bool down = (digitalRead(PIN_BOOT) == LOW);
  if (down && !bootWasDown) {
    bootWasDown    = true;
    bootDownMs     = millis();
    lastActivityMs = millis();  // any press resets inactivity timer
  }
  if (!down && bootWasDown) {
    uint32_t held = millis() - bootDownMs;
    bootWasDown = false;
    // While screensaver is active, any press just returns to normal display
    if (ssaverActive) {
      ssaverActive = false;
      u8g2.setPowerSave(0);  // re-enable display in case 100% dim (power-off) was active
      u8g2.setContrast(200);
      updateLedStatus();
      drawScreen(false, 0);
      return;
    }
    if (held >= LONG_PRESS_MS) {
      runScan();
    } else {
      // Count online devices — must match drawDeviceList() so the modulo
      // wraps correctly back to page 0 after the last page.
      int onlineCount = 0;
      for (int i = 0; i < numDevices; i++)
        if (devices[i].online) onlineCount++;
      int tp = max(1, (onlineCount + DEVICES_PER_PAGE - 1) / DEVICES_PER_PAGE);
      currentPage = (currentPage + 1) % tp;
    }
    drawScreen(false, 0);
  }
}

// ─── SERIAL DUMP ───────────────────────────────────────────────────────────────
static const char* typeName(int t) {
  switch (t) {
    case PHONE:   return "PHONE";
    case LAPTOP:  return "LAPTOP";
    case DESKTOP: return "DESKTOP";
    case TV:      return "TV";
    case ROUTER:  return "ROUTER";
    case NAS:     return "NAS";
    case DEV_PI:  return "PI";
    case PRINTER: return "PRINTER";
    case TABLET:  return "TABLET";
    case SPEAKER:   return "SPEAKER";
    case DEV_ESP32: return "ESP32";
    case CONSOLE:   return "CONSOLE";
    default:        return "UNKNOWN";
  }
}

void printDevices() {
  int online = 0;
  for (int i = 0; i < numDevices; i++) if (devices[i].online) online++;
  Serial.printf("\n=== SCAN COMPLETE — %d online / %d total ===\n", online, numDevices);
  Serial.println(" #  ST  TYPE     IP              MAC                NAME");
  Serial.println("--- --- -------- --------------- ------------------ ----------------");
  for (int i = 0; i < numDevices; i++) {
    Device& d = devices[i];
    Serial.printf("%2d  %-3s %-8s %d.%d.%d.%-4d    %02x:%02x:%02x:%02x:%02x:%02x  %s\n",
      i,
      d.online ? "ON" : "off",
      typeName(d.type),
      d.ip[0], d.ip[1], d.ip[2], d.ip[3],
      d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
      d.name
    );
  }
  Serial.println();
}

// ─── DEVICE STATUS WEB PAGE ────────────────────────────────────────────────────
// Served from the board's own IP on port 80. Auto-refreshes every 30 s.
void handleWebRoot() {
  int online = 0;
  for (int i = 0; i < numDevices; i++) if (devices[i].online) online++;

  IPAddress myIP = WiFi.localIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", myIP[0], myIP[1], myIP[2], myIP[3]);

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<title>LANMon</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='30'>"
    "<style>"
    "body{font-family:sans-serif;max-width:700px;margin:30px auto;padding:16px}"
    "h2{margin-bottom:6px}"
    "p{margin:4px 0 14px;color:#555;font-size:13px}"
    "table{width:100%;border-collapse:collapse;font-size:14px}"
    "td,th{padding:6px 8px;border-bottom:1px solid #ddd;text-align:left}"
    "th{background:#f4f4f4;font-weight:600}"
    "tr:last-child td{border-bottom:none}"
    ".on{color:#229922;font-weight:bold}"
    ".off{color:#aaa}"
    "</style></head><body>"
    "<h2>LANMon &mdash; Network Devices</h2>"
    "<p>Board: <strong>";
  html += ipStr;
  html += "</strong> &nbsp;|&nbsp; ";
  html += online;
  html += " online / ";
  html += numDevices;
  html += " total &nbsp;|&nbsp; auto-refreshes every 30&nbsp;s</p>"
    "<table>"
    "<tr><th>#</th><th>Status</th><th>Type</th><th>IP Address</th>"
    "<th>MAC Address</th><th>Name</th></tr>";

  for (int i = 0; i < numDevices; i++) {
    Device& d = devices[i];
    char ipBuf[16], macBuf[18];
    snprintf(ipBuf,  sizeof(ipBuf),  "%d.%d.%d.%d",
             d.ip[0], d.ip[1], d.ip[2], d.ip[3]);
    snprintf(macBuf, sizeof(macBuf), "%02x:%02x:%02x:%02x:%02x:%02x",
             d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
    html += "<tr><td>";
    html += i;
    html += "</td><td class='";
    html += d.online ? "on'>ON" : "off'>off";
    html += "</td><td>";
    html += typeName(d.type);
    html += "</td><td>";
    html += ipBuf;
    html += "</td><td>";
    html += macBuf;
    html += "</td><td>";
    html += d.name;
    html += "</td></tr>";
  }

  html += "</table></body></html>";
  lanServer.send(200, "text/html", html);
}

// ─── IP TABLE HELPERS ──────────────────────────────────────────────────────────
// These live after typeName() so they can call it for the web UI type dropdown.

// Convert posted int string back to DevType, clamped to valid range.
int typeFromStr(const String& s) {
  int v = s.toInt();
  if (v < PHONE || v > DEV_UNKNOWN) return DEV_UNKNOWN;
  return v;
}

// Save runtimeTable to NVS ("lanmon" namespace, keys "iptbl_cnt" + "iptbl").
// Caller must NOT have prefs open — this function opens and closes it.
void saveIPTable() {
  prefs.begin("lanmon", false);
  prefs.putInt("iptbl_cnt", runtimeTableCount);
  if (runtimeTableCount > 0)
    prefs.putBytes("iptbl", runtimeTable, (size_t)runtimeTableCount * sizeof(IPEntry));
  // Clear the stored subnet so it is recaptured fresh on the next WiFi connect.
  // This lets the user rebuild the table for a new network without getting a
  // conflict warning on the next boot.
  prefs.remove("sub");
  prefs.end();
}

// Load runtimeTable from NVS. Falls back to compiled-in IP_TABLE if no NVS data.
// Caller must NOT have prefs open — this function opens and closes it.
void loadIPTable() {
  prefs.begin("lanmon", true);
  int cnt = prefs.getInt("iptbl_cnt", -1);
  if (cnt > 0 && cnt <= MAX_IP_ENTRIES) {
    runtimeTableCount = cnt;
    prefs.getBytes("iptbl", runtimeTable, (size_t)cnt * sizeof(IPEntry));
    prefs.end();
    Serial.printf("IP table: loaded %d entries from NVS\n", cnt);
    return;
  }
  prefs.end();
  // No NVS table — seed from the compiled-in IPList.h defaults
  runtimeTableCount = min(IP_TABLE_COUNT, MAX_IP_ENTRIES);
  for (int i = 0; i < runtimeTableCount; i++) runtimeTable[i] = IP_TABLE[i];
  Serial.printf("IP table: seeded %d entries from IPList.h\n", runtimeTableCount);
}

// Compare the WiFi subnet the device just connected to against the one stored
// in NVS when the IP table was last saved/used.  A mismatch means the named
// entries (.78, .108 …) now refer to random devices on the new network.
//
// Outcomes:
//   • No stored subnet  → first boot on this network; store it, no warning.
//   • Subnets match     → same network, nothing to do.
//   • Subnets differ    → set subnetConflict; DO NOT update stored subnet yet.
//     The conflict clears only when the user saves a fresh IP table through the
//     portal (saveIPTable removes "sub"), so the corrected subnet is captured on
//     the next connect without triggering another warning.
void checkAndStoreSubnet() {
  IPAddress local = WiFi.localIP();
  uint8_t cur[3] = { local[0], local[1], local[2] };

  prefs.begin("lanmon", false);
  if (prefs.isKey("sub")) {
    uint8_t stored[3];
    prefs.getBytes("sub", stored, 3);
    bool mismatch = (stored[0] != cur[0] || stored[1] != cur[1] || stored[2] != cur[2]);
    if (mismatch && runtimeTableCount > 0) {
      subnetConflict = true;
      memcpy(conflictOldSub, stored, 3);
      memcpy(conflictNewSub, cur,    3);
      Serial.printf("Subnet conflict: table was %d.%d.%d.x, network is %d.%d.%d.x\n",
        stored[0], stored[1], stored[2], cur[0], cur[1], cur[2]);
    }
    // Only update stored subnet when there is no conflict
    if (!mismatch) prefs.putBytes("sub", cur, 3);
  } else {
    // First connect — record this subnet as the baseline
    prefs.putBytes("sub", cur, 3);
    Serial.printf("Subnet stored: %d.%d.%d.x\n", cur[0], cur[1], cur[2]);
  }
  prefs.end();
}

// Build the <select> options for all device types, marking `selected` as chosen.
String typeSelectHtml(int selected) {
  static const char* labels[] = {
    "Phone","Laptop","Desktop","TV","Router","NAS","Pi",
    "Printer","Tablet","Speaker","ESP32","Console","Unknown"
  };
  String s = "";
  for (int i = 0; i <= DEV_UNKNOWN; i++) {
    s += "<option value='";
    s += i;
    s += "'";
    if (i == selected) s += " selected";
    s += ">";
    s += labels[i];
    s += "</option>";
  }
  return s;
}

// ─── WIFI PROVISIONING ─────────────────────────────────────────────────────────
// Scans for nearby networks, then starts AP hotspot "LANMon-Setup".
// Serves a two-section captive portal:
//   • WiFi section: SSID dropdown (from scan) + password with show/hide toggle.
//   • Device list section: view, delete, and add entries to the runtime IP table.
// Credentials saved to NVS then device restarts. Never returns.
void runProvisioningMode() {
  setLed(20, 0, 20);  // purple = provisioning

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(22, 28, "WiFi Setup");
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(2, 38, "Join: LANMon-Setup");
  u8g2.drawStr(2, 47, "Then open browser:");
  u8g2.drawStr(22, 57, "192.168.4.1");
  u8g2.sendBuffer();

  // Scan for nearby networks before switching to AP-only mode
  WiFi.mode(WIFI_STA);
  int scanCount = WiFi.scanNetworks();

  WiFi.mode(WIFI_AP);
  WiFi.softAP("LANMon-Setup");

  DNSServer dns;
  dns.start(53, "*", IPAddress(192, 168, 4, 1));

  WebServer server(80);
  bool shouldRestart = false;

  // ── Main page — WiFi setup + IP table editor ────────────────────────────────
  server.on("/", HTTP_GET, [&]() {
    // CSS shared across all sections
    String html =
      "<!DOCTYPE html><html><head>"
      "<title>LANMon Setup</title>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>"
      "body{font-family:sans-serif;max-width:420px;margin:30px auto;padding:16px}"
      "h2{margin-bottom:4px}h3{margin:22px 0 8px;border-bottom:1px solid #ddd;padding-bottom:4px}"
      "label{display:block;margin:10px 0 3px;font-size:14px}"
      "input,select{width:100%;padding:8px;box-sizing:border-box;font-size:15px;"
      "border:1px solid #ccc;border-radius:4px}"
      ".btn{margin-top:14px;width:100%;padding:11px;background:#0066cc;"
      "color:#fff;border:none;border-radius:6px;font-size:15px;cursor:pointer}"
      ".btn-sm{padding:4px 10px;font-size:12px;border:none;border-radius:3px;"
      "cursor:pointer;color:#fff;width:auto;margin:0}"
      ".btn-del{background:#cc3300}"
      ".pw-row{display:flex;gap:8px;align-items:stretch}"
      ".pw-row input{flex:1}"
      ".pw-row button{flex:0 0 54px;padding:8px;background:#666;color:#fff;"
      "border:none;border-radius:4px;cursor:pointer;font-size:13px;width:auto;margin:0}"
      "table{width:100%;border-collapse:collapse;margin:8px 0;font-size:13px}"
      "td,th{padding:5px 6px;border-bottom:1px solid #ddd;text-align:left}"
      "th{background:#f4f4f4;font-weight:600}"
      "tr:last-child td{border-bottom:none}"
      "</style></head><body>"
      "<h2>LANMon Setup</h2>"

      // ── WiFi section ──
      "<h3>WiFi Network</h3>"
      "<form action='/save_wifi' method='post'>"
      "<label>Network (SSID)</label>"
      "<select name='ssid'>";

    // Populate dropdown from scan results
    if (scanCount > 0) {
      for (int i = 0; i < scanCount; i++) {
        String ssid = WiFi.SSID(i);
        html += "<option value='";
        html += ssid;
        html += "'>";
        html += ssid;
        html += " (";
        html += WiFi.RSSI(i);
        html += " dBm)</option>";
      }
    } else {
      html += "<option value=''>No networks found — type below</option>";
    }

    html +=
      "</select>"
      "<label>Or type SSID manually</label>"
      "<input name='ssid_manual' type='text' placeholder='Leave blank to use dropdown'>"
      "<label>Password</label>"
      "<div class='pw-row'>"
      "<input name='pass' id='pw' type='password' placeholder='WiFi password'>"
      "<button type='button' id='pwtog' onclick=\""
      "var p=document.getElementById('pw');"
      "p.type=p.type=='password'?'text':'password';"
      "document.getElementById('pwtog').textContent=p.type=='password'?'Show':'Hide';"
      "\">Show</button>"
      "</div>"
      "<label>Screensaver dim</label>"
      "<select name='ssdim'>";
    for (int d = 0; d <= 100; d += 10) {
      html += "<option value='";
      html += d;
      html += "'";
      if (d == (int)ssaverDimPct) html += " selected";
      html += ">";
      if (d == 0)   html += "0% (no dim)";
      else if (d == 100) html += "100% (display off)";
      else { html += d; html += "%"; }
      html += "</option>";
    }
    html +=
      "</select>"
      "<label style='display:flex;align-items:center;gap:10px;margin-top:14px'>"
      "<input type='checkbox' name='bst' value='on' style='width:auto;margin:0'";
    html += bstEnabled ? " checked" : "";
    html +=
      "> BST &mdash; British Summer Time (UTC+1)"
      "</label>"
      "<button class='btn' type='submit'>Save &amp; Connect</button>"
      "</form>"

      // ── Known devices section ──
      "<h3>Known Devices</h3>"
      "<table>"
      "<tr><th>IP (.x)</th><th>Name</th><th>Type</th><th></th></tr>";

    for (int i = 0; i < runtimeTableCount; i++) {
      html += "<tr><td>";
      html += runtimeTable[i].host;
      html += "</td><td>";
      html += runtimeTable[i].name;
      html += "</td><td>";
      html += typeName(runtimeTable[i].type);
      html += "</td><td>"
              "<form action='/del_ip' method='post' style='margin:0'>"
              "<input type='hidden' name='host' value='";
      html += runtimeTable[i].host;
      html += "'>"
              "<button class='btn-sm btn-del' type='submit'>Del</button>"
              "</form></td></tr>";
    }

    html +=
      "</table>"
      "<h3>Add Device</h3>"
      "<form action='/add_ip' method='post'>"
      "<label>Last IP octet (1&#8211;254)</label>"
      "<input name='host' type='number' min='1' max='254' required>"
      "<label>Display name</label>"
      "<input name='name' type='text' maxlength='15' required>"
      "<label>Type</label>"
      "<select name='type'>";
    html += typeSelectHtml(PHONE);
    html +=
      "</select>"
      "<button class='btn' type='submit'>Add Device</button>"
      "</form>"
      "</body></html>";

    server.send(200, "text/html", html);
  });

  // ── Save WiFi credentials ───────────────────────────────────────────────────
  server.on("/save_wifi", HTTP_POST, [&]() {
    // Prefer manually typed SSID over dropdown if provided
    String ssid = server.arg("ssid_manual");
    if (ssid.length() == 0) ssid = server.arg("ssid");
    String pass = server.arg("pass");

    if (ssid.length() == 0) {
      server.send(400, "text/plain", "SSID required");
      return;
    }

    bool bst = server.arg("bst") == "on";
    int dimPct = server.arg("ssdim").toInt();
    dimPct = constrain((dimPct / 10) * 10, 0, 100);
    ssaverDimPct = (uint8_t)dimPct;
    prefs.begin("lanmon", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putBool("bst", bst);
    prefs.putUChar("ssdim", ssaverDimPct);
    prefs.end();

    server.send(200, "text/html",
      "<!DOCTYPE html><html><body style='font-family:sans-serif;"
      "max-width:420px;margin:40px auto;padding:16px'>"
      "<h2>Saved!</h2>"
      "<p>Connecting to <strong>" + ssid + "</strong>.</p>"
      "<p>You can close this page.</p>"
      "</body></html>");

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(8, 34, "Credentials saved");
    u8g2.drawStr(28, 46, "Rebooting...");
    u8g2.sendBuffer();
    setLed(0, 40, 0);
    shouldRestart = true;
  });

  // ── Delete IP table entry ───────────────────────────────────────────────────
  server.on("/del_ip", HTTP_POST, [&]() {
    int host = server.arg("host").toInt();
    for (int i = 0; i < runtimeTableCount; i++) {
      if (runtimeTable[i].host == (uint8_t)host) {
        for (int j = i; j < runtimeTableCount - 1; j++)
          runtimeTable[j] = runtimeTable[j + 1];
        runtimeTableCount--;
        saveIPTable();
        break;
      }
    }
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });

  // ── Add / update IP table entry ─────────────────────────────────────────────
  server.on("/add_ip", HTTP_POST, [&]() {
    int host = server.arg("host").toInt();
    String name = server.arg("name");
    int type = typeFromStr(server.arg("type"));

    if (host >= 1 && host <= 254 && name.length() > 0) {
      // Update existing entry if host already listed, otherwise append
      int slot = -1;
      for (int i = 0; i < runtimeTableCount; i++)
        if (runtimeTable[i].host == (uint8_t)host) { slot = i; break; }
      if (slot < 0 && runtimeTableCount < MAX_IP_ENTRIES)
        slot = runtimeTableCount++;
      if (slot >= 0) {
        runtimeTable[slot].host = (uint8_t)host;
        strncpy(runtimeTable[slot].name, name.c_str(), NAME_LEN - 1);
        runtimeTable[slot].name[NAME_LEN - 1] = '\0';
        runtimeTable[slot].type = (DevType)type;
        saveIPTable();
      }
    }
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });

  // ── Captive portal redirect ─────────────────────────────────────────────────
  server.onNotFound([&]() {
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  });

  server.begin();
  while (!shouldRestart) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
  }

  delay(2000);
  ESP.restart();
}

// ─── SETUP & LOOP ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  // Wait up to 3s for USB CDC serial to connect before proceeding.
  // Without this the scan output during setup() is lost before the
  // host-side serial monitor has established the link.
  { uint32_t t = millis(); while (!Serial && millis() - t < 3000) delay(10); }
  pinMode(PIN_BOOT, INPUT_PULLUP);
  led.begin();
  setLed(20, 0, 20);

  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  u8g2.setContrast(200);

  // Check provisioning: BOOT held at power-on clears credentials and enters
  // setup mode. No saved credentials also triggers setup mode (first boot).
  delay(100);  // let GPIO settle before reading button
  prefs.begin("lanmon", false);
  if (digitalRead(PIN_BOOT) == LOW) {
    prefs.remove("ssid");
    prefs.remove("pass");
    Serial.println("BOOT held — WiFi credentials cleared");
  }
  bool needProvisioning = !prefs.isKey("ssid");
  if (!needProvisioning) {
    wifiSSID   = prefs.getString("ssid");
    wifiPass   = prefs.getString("pass");
    bstEnabled = prefs.getBool("bst", false);
  }
  ssaverDimPct = prefs.getUChar("ssdim", 50);  // always load; default 50%
  prefs.end();
  Serial.printf("BST: %s\n", bstEnabled ? "on (UTC+1)" : "off (UTC)");

  // Load device name table from NVS (falls back to compiled IPList.h defaults)
  loadIPTable();

  if (needProvisioning) {
    runProvisioningMode();  // never returns
  }

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(28, 34, "LAN MON");
  u8g2.drawStr(12, 44, "Connecting...");
  u8g2.sendBuffer();

  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    setLed(20, 0, (millis() & 512) ? 20 : 0);
  }

  if (WiFi.status() != WL_CONNECTED) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(5, 34, "WiFi failed");
    u8g2.drawStr(0, 44, wifiSSID.c_str());
    u8g2.sendBuffer();
    setLed(40, 0, 0);
    while (true) delay(1000);
  }

  checkAndStoreSubnet();   // detect subnet change before first scan

  // Sync NTP time for screensaver clock (non-blocking — available within seconds)
  configTime(bstEnabled ? 3600 : 0, 0, "pool.ntp.org", "time.cloudflare.com");
  Serial.println("NTP sync started");

  // Start device status web server — accessible at http://<board-ip>/
  lanServer.on("/", HTTP_GET, handleWebRoot);
  lanServer.begin();
  Serial.printf("Web UI: http://%s/\n", WiFi.localIP().toString().c_str());

  lastActivityMs = millis();  // start inactivity timer after setup

  runScan();
  drawScreen(false, 0);
}

void loop() {
  handleButton();
  lanServer.handleClient();

  // Screensaver activation check (never blocks further processing)
  if (!ssaverActive && millis() - lastActivityMs >= SCREENSAVER_MS) {
    ssaverActive = true;
    setLed(0, 0, 0);
    if (ssaverDimPct >= 100) {
      u8g2.setPowerSave(1);  // display off for 100% dim
    } else {
      // Quadratic curve: linear contrast mapping is not perceptually visible on
      // SSD1306 — squaring the ratio makes mid-range dim levels actually appear dim.
      // e.g. 60% dim → contrast 32 (vs 80 with linear), which is clearly visible.
      uint32_t f = (uint32_t)(100 - ssaverDimPct);
      u8g2.setContrast((uint8_t)(200UL * f * f / 10000UL));
      drawScreensaver();
    }
  }

  // WiFi reconnect — runs regardless of screensaver state
  if (WiFi.status() != WL_CONNECTED) {
    if (!ssaverActive) setLed(40, 0, 0);
    if (!ssaverActive) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(5, 10, "WiFi lost");
      u8g2.drawStr(5, 40, "Reconnecting");
      u8g2.sendBuffer();
    }
    WiFi.reconnect();
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(500);
    if (WiFi.status() == WL_CONNECTED) {
      checkAndStoreSubnet();
      runScan();
      if      (!ssaverActive)       drawScreen(false, 0);
      else if (ssaverDimPct < 100)  drawScreensaver();
    }
    return;
  }

  // Auto-scan — runs regardless of screensaver state; updates devices[], web UI, serial
  if (millis() - lastScanMs >= SCAN_INTERVAL_MS) {
    runScan();
    if      (!ssaverActive)       drawScreen(false, 0);
    else if (ssaverDimPct < 100)  drawScreensaver();
    return;
  }

  // Periodic display refresh
  static uint32_t lastDisplayMs = 0;
  uint32_t refreshMs = ssaverActive ? 1000 : 500;   // clock updates 1/s, bars 2/s
  if (millis() - lastDisplayMs >= refreshMs) {
    lastDisplayMs = millis();
    if      (!ssaverActive)           drawScreen(false, 0);
    else if (ssaverDimPct < 100)      drawScreensaver();
    // 100% dim: display powered off — nothing to draw
  }

  delay(50);
}
