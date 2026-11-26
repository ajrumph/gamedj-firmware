/*
  Game Watching DJ — camera + relay firmware with phone-based Wi-Fi setup
  -------------------------------------------------------------------
  Board: AI Thinker ESP32-CAM.

  Libraries: WiFiManager by tzapu (v2.0.17 or newer).
             Everything else ships with the ESP32 board package.

  What the customer does:
    1. Plug in. LED blinks slowly = setup mode.
    2. On their phone, join the Wi-Fi network "GameDJ-Setup-XXXX".
    3. A setup page opens. Pick home Wi-Fi, type password, tap Save.
    4. LED goes solid = connected. The unit registers itself with the website.
    5. On the same Wi-Fi, they sign in at the website and tap "Add my Game DJ".
       (No pairing code. Until it's added to an account, the unit keeps TV audio.)

  Re-entering setup later (new router or new Wi-Fi password):
    Unplug, plug back in, and within 3 seconds unplug and plug in again
    (or press the reset button twice). LED blinks slowly = setup mode.
    This keeps the unit on its account.

  LED:
    quick double-blink = booting (press reset again now to enter setup)
    slow blink         = setup mode, join GameDJ-Setup-XXXX from your phone
    solid              = connected and running
    fast blink         = problem (Wi-Fi down or server unreachable)

  Bench tools (Serial Monitor, 115200 baud, line ending "Newline"):
    factory  -> erase Wi-Fi + device token, reboot into setup. The next time it
                registers, the website releases it from any account, so the
                customer can add it. Run before boxing a unit you tested.
    status   -> print board, device ID, Wi-Fi, registration state
    music    -> switch relay to music (wiring check; next snapshot may switch back)
    tv       -> switch relay to TV audio
*/

// ============================ BOARD =================================
// The only supported board is the AI Thinker ESP32-CAM (the flash command may
// still pass -DBOARD_AI_THINKER; without it this default applies).
#if !defined(BOARD_AI_THINKER)
  #define BOARD_AI_THINKER
#endif
// =====================================================================

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <HTTPUpdate.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <WiFiServer.h>
void remoteLog(const String& msg);
void checkForUpdate();

// ====================== SETTINGS — CHANGE THESE ======================
// Base44 function addresses (score-sync-dj)
const char* REGISTER_URL = "https://score-sync-dj.base44.app/functions/registerDevice";
const char* SNAPSHOT_URL = "https://score-sync-dj.base44.app/functions/analyzeFrame";
// PIN_TEST=1 builds a bench-test firmware: after boot and the update check it
// cycles relay pins 13, 14, 15 and 2 together (2 s on, 2 s off) and logs readbacks.
// A normal power cycle re-checks for updates first, so it can be replaced by a normal release.
#define PIN_TEST 0
const char* FIRMWARE_VERSION = "1.6.1";

// Over-the-air updates. Each board reads <MANIFEST_BASE><board folder>/manifest.json:
//   {"version":"1.3.1","url":"https://.../GameWatchingDJ-1.3.1.bin"}
// To move updates to Base44 later, ship one release that changes MANIFEST_BASE.
const char* MANIFEST_BASE = "https://raw.githubusercontent.com/ajrumph/gamedj-updates/main/";
const unsigned long UPDATE_CHECK_MS = 6UL * 60 * 60 * 1000;   // every 6 hours

// Remote diagnostics: short event messages (boot, audio switches, errors) go to
// this private ntfy.sh topic so problems can be read without a USB cable.
// Set REMOTE_LOG to false to turn it off.
const bool  REMOTE_LOG     = true;
const char* REMOTE_LOG_URL = "https://ntfy.sh/gamedj-ajrumph-8e597536";

// AEDIKO 2-channel relay module: turns ON (music) when its IN pin is pulled LOW
// (confirmed Nov 19: red light on with LOW). For OFF (TV) the pin is released
// (input, no drive) so 3.3 V logic can't leave a 5 V module half-on.
const int RELAY_ON  = LOW;


const unsigned long DOUBLE_RESET_WINDOW_MS = 3000;
const int           MAX_FAILURES_BEFORE_TV = 5;   // fall back to TV audio if server stops answering
// =====================================================================

#if defined(BOARD_AI_THINKER)
  const char* BOARD_NAME = "AI Thinker ESP32-CAM";
  const char* BOARD_FOLDER = "esp32cam";
  // Avoid GPIO12 (can stop the board booting) and GPIO0/GPIO4.
  const int RELAY_PIN_L = 13;
  const int RELAY_PIN_R = 14;
  const int STATUS_LED  = 33;   // red LED on back of board, ON when LOW
  const int LED_ON_LEVEL = LOW;
  const int FLASH_LED   = 4;    // bright white flash LED — keep off

  #define PWDN_GPIO_NUM     32
  #define RESET_GPIO_NUM    -1
  #define XCLK_GPIO_NUM      0
  #define SIOD_GPIO_NUM     26
  #define SIOC_GPIO_NUM     27
  #define Y9_GPIO_NUM       35
  #define Y8_GPIO_NUM       34
  #define Y7_GPIO_NUM       39
  #define Y6_GPIO_NUM       36
  #define Y5_GPIO_NUM       21
  #define Y4_GPIO_NUM       19
  #define Y3_GPIO_NUM       18
  #define Y2_GPIO_NUM        5
  #define VSYNC_GPIO_NUM    25
  #define HREF_GPIO_NUM     23
  #define PCLK_GPIO_NUM     22

#else
  #error "Pick a board in the BOARD section at the top of the sketch."
#endif

const int LED_OFF_LEVEL = (LED_ON_LEVEL == HIGH) ? LOW : HIGH;

// ---------------------------------------------------------------------
enum LedMode { LED_BOOT, LED_SETUP, LED_OK, LED_ERROR };
volatile LedMode ledMode = LED_BOOT;

enum AudioSource { AUDIO_TV, AUDIO_MUSIC };
AudioSource audioSource = AUDIO_TV;

Preferences prefs;
WiFiManager wm;

String deviceId;      // unique per board, from the chip's MAC
String apName;        // "GameDJ-Setup-XXXX"
String deviceToken;   // issued by Base44 when the unit registers

WiFiClientSecure tlsClient;
HTTPClient snapHttp;
int consecutiveFailures = 0;

// ============================ LED ====================================
void ledSet(bool on) { digitalWrite(STATUS_LED, on ? LED_ON_LEVEL : LED_OFF_LEVEL); }

void ledTask(void*) {
  bool on = false;
  for (;;) {
    switch (ledMode) {
      case LED_OK:
        ledSet(true);
        vTaskDelay(pdMS_TO_TICKS(200));
        break;
      case LED_SETUP:
        on = !on; ledSet(on);
        vTaskDelay(pdMS_TO_TICKS(600));
        break;
      case LED_ERROR:
        on = !on; ledSet(on);
        vTaskDelay(pdMS_TO_TICKS(120));
        break;
      case LED_BOOT:
      default:
        ledSet(true);  vTaskDelay(pdMS_TO_TICKS(80));
        ledSet(false); vTaskDelay(pdMS_TO_TICKS(120));
        ledSet(true);  vTaskDelay(pdMS_TO_TICKS(80));
        ledSet(false); vTaskDelay(pdMS_TO_TICKS(700));
        break;
    }
  }
}

// =========================== AUDIO ===================================
// Music: drive the pin LOW (proven to switch the relay on in 1.3.2).
// TV: release the pin (input) so the module's own 5V pull-up turns it fully off
// (proven off in 1.3.1). Order matters: set OUTPUT first, then LOW.
// Relay boards come as "low-trigger" (jumper on Low, or active-low boards) and
// "high-trigger" (jumper on High). Auto-detected at boot from the released pins:
// a low-trigger board holds the input high (reads 1 with the internal pull-up AND
// pull-down); a high-trigger board holds it low (reads 0 on both). Anything else
// (unconnected, ambiguous) defaults to low-trigger.
bool relayActiveLow = true;
String relayPolarityInfo = "";
void detectRelayPolarity() {
  const int pins[2] = {RELAY_PIN_L, RELAY_PIN_R};
  int up = 0, down = 0;
  for (int i = 0; i < 2; i++) {
    pinMode(pins[i], INPUT_PULLUP);   delay(30); up   += digitalRead(pins[i]);
    pinMode(pins[i], INPUT_PULLDOWN); delay(30); down += digitalRead(pins[i]);
    pinMode(pins[i], INPUT);
  }
  relayActiveLow = !(up == 0 && down == 0);
  relayPolarityInfo = String("relay polarity: ") + (relayActiveLow ? "low-trigger" : "HIGH-trigger") +
                      " (pull-up reads " + up + "/2, pull-down reads " + down + "/2)";
}

void relayPin(int pin, bool on) {
  if (on) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, relayActiveLow ? LOW : HIGH);
  } else {
    pinMode(pin, INPUT);
  }
}

void setAudio(AudioSource src) {
  bool on = (src == AUDIO_MUSIC);
  relayPin(RELAY_PIN_L, on);
  relayPin(RELAY_PIN_R, on);
  bool changed = (src != audioSource);
  audioSource = src;
  if (changed) remoteLog(String("audio -> ") + (src == AUDIO_MUSIC ? "MUSIC" : "TV") +
                         " (pins " + RELAY_PIN_L + "=" + digitalRead(RELAY_PIN_L) + ", " + RELAY_PIN_R + "=" + digitalRead(RELAY_PIN_R) + ")");
}

// ========================== HELPERS ==================================
// Pulls "value" out of {"key":"value"} without needing a JSON library.
String jsonString(const String& body, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return "";
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

// Pulls a number out of {"key":12} (or "key":"12"); returns -1 if missing.
long jsonNumber(const String& body, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return -1;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return -1;
  int i = colon + 1;
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '"')) i++;
  if (i >= (int)body.length() || body[i] < '0' || body[i] > '9') return -1;
  return body.substring(i).toInt();
}

// Pulls a decimal number such as 1.3 out of {"key":1.3}; returns the value in
// milliseconds, or -1 if missing. (1.5.0: the server may now ask for sub-2-second checks.)
long jsonSecondsAsMs(const String& body, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return -1;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return -1;
  int i = colon + 1;
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '"')) i++;
  if (i >= (int)body.length() || body[i] < '0' || body[i] > '9') return -1;
  float v = body.substring(i).toFloat();
  if (v <= 0) return -1;
  return (long)(v * 1000.0f);
}

// How often to ask the server. The server may stretch this (steady game
// footage) via "next_in_seconds"; clamped so a bad reply can never starve
// or flood it. Missing field -> the original 3 s cadence.
unsigned long snapshotIntervalMs = 3000;
const unsigned long MIN_INTERVAL_MS = 1000, MAX_INTERVAL_MS = 12000;  // 1.5.0: floor lowered to 1 s

void remoteLog(const String& msg) {
  Serial.println("[log] " + msg);
  if (!REMOTE_LOG || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  h.setTimeout(5000);
  if (!h.begin(c, REMOTE_LOG_URL)) return;
  h.addHeader("Content-Type", "text/plain");
  h.POST(deviceId.substring(8) + " v" + FIRMWARE_VERSION + ": " + msg);
  h.end();
}

void factoryReset() {
  Serial.println("Factory reset: erasing Wi-Fi and device token...");
  prefs.clear();
  wm.resetSettings();
  delay(500);
  ESP.restart();
}

void printStatus() {
  Serial.printf("Board: %s\n", BOARD_NAME);
  Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
  Serial.printf("Device ID: %s\n", deviceId.c_str());
  Serial.printf("Setup network: %s\n", apName.c_str());
  Serial.printf("Wi-Fi: %s (%s)\n", WiFi.SSID().c_str(),
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
  Serial.printf("Registered: %s\n", deviceToken.length() ? "yes" : "no");
  Serial.printf("Audio: %s\n", audioSource == AUDIO_MUSIC ? "MUSIC" : "TV");
}

void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "factory") factoryReset();
  else if (cmd == "status") printStatus();
  else if (cmd == "music") setAudio(AUDIO_MUSIC);
  else if (cmd == "tv") setAudio(AUDIO_TV);
  else if (cmd == "update") checkForUpdate();
}

// ======================= DOUBLE RESET ================================
// A flag is written to flash at boot and cleared after 3 seconds.
// If the board boots again while the flag is still set, the user
// reset/replugged twice quickly -> open the setup portal.
bool checkDoubleReset() {
  if (prefs.getBool("drd", false)) {
    prefs.putBool("drd", false);
    Serial.println("Double reset detected -> setup mode");
    return true;
  }
  prefs.putBool("drd", true);
  ledMode = LED_BOOT;
  delay(DOUBLE_RESET_WINDOW_MS);
  prefs.putBool("drd", false);
  return false;
}

// ======================== REGISTRATION ===============================
// The unit introduces itself to the website. No pairing code: the website
// creates a Device record the first time it sees this device ID, and the
// customer claims it from their account while on the same home Wi-Fi.
//   Sends:   {"device_id","token","firmware","board"}  (token empty if new or factory reset)
//   Returns: {"token":"...","claimed":true|false}
// An empty token tells the website this is a fresh start, so it issues a new
// token and releases the unit from any previous account.
bool registerDevice() {
  WiFiClientSecure client;
  client.setInsecure();  // TODO for wide release: pin Base44's root certificate
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, REGISTER_URL)) return false;
  http.addHeader("Content-Type", "application/json");

  String body = String("{\"device_id\":\"") + deviceId +
                "\",\"token\":\"" + deviceToken +
                "\",\"firmware\":\"" + FIRMWARE_VERSION +
                "\",\"board\":\"" + BOARD_NAME + "\"}";
  int status = http.POST(body);
  String resp = http.getString();
  http.end();
  Serial.printf("Register response %d\n", status);

  if (status != 200) return false;
  String token = jsonString(resp, "token");
  if (token.length() == 0) return false;
  if (token != deviceToken) {
    deviceToken = token;
    prefs.putString("token", token);
  }
  Serial.println(resp.indexOf("\"claimed\":true") >= 0
                 ? "Registered. Added to an account."
                 : "Registered. Waiting for the owner to tap Add on the website.");
  return true;
}

// Keep trying until the website answers. TV audio stays on meanwhile.
void registerUntilDone() {
  while (!registerDevice()) {
    ledMode = LED_ERROR;
    Serial.println("Could not reach the website to register, retrying in 10 s...");
    delay(10000);
  }
}

// ===================== WI-FI + SETUP PORTAL ==========================
// Connection health reported to the website with every check.
unsigned int wifiDrops = 0;          // times Wi-Fi dropped since this boot
String resetReasonText = "unknown";  // why the board last restarted
wifi_power_t gTxPower = WIFI_POWER_19_5dBm;   // full power: a weak uplink was dropping checks
bool stableMarked = false;

String resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    default:                return "unknown";
  }
}

void lowerTxPower() {
#if defined(BOARD_AI_THINKER)
  // Normal power is 15 dBm (about 1.6x the range of the old 11 dBm setting).
  // The Wi-Fi start-up current spike can freeze or reset the board on weak
  // power, so if the last boot died before running 2 minutes, this boot uses 11 dBm.
  WiFi.setTxPower(gTxPower);
#endif
  WiFi.setSleep(false);
}

void configurePortal() {
  WiFi.mode(WIFI_STA);
  lowerTxPower();
  wm.setTitle("Game Watching DJ Setup");
  wm.setClass("invert");                       // dark theme
  std::vector<const char*> menu = {"wifi", "exit"};
  wm.setMenu(menu);                            // hide advanced pages
  wm.setConnectTimeout(20);
  wm.setWiFiAPChannel(11);                     // channel 1 is usually crowded
  wm.setWiFiAutoReconnect(true);
  wm.setAPCallback([](WiFiManager*) {
    lowerTxPower();
    ledMode = LED_SETUP;
    Serial.printf("Setup portal open: join \"%s\" from a phone\n", apName.c_str());
  });
}

// ---- Remembered networks: up to 4, most recently used first. -----------
// Every network the unit joins (including through the setup page) is saved here.
// At start-up and when the link drops, it joins the strongest one it can see.
const int MAX_NETS = 4;
String netSsid(int i) { return prefs.getString((String("ns") + i).c_str(), ""); }
String netPass(int i) { return prefs.getString((String("np") + i).c_str(), ""); }

void saveNetwork(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return;
  if (netSsid(0) == ssid && netPass(0) == pass) return;   // already first, no flash write
  String s[MAX_NETS], p[MAX_NETS];
  int n = 0;
  s[n] = ssid; p[n] = pass; n++;
  for (int i = 0; i < MAX_NETS && n < MAX_NETS; i++) {
    String ss = netSsid(i);
    if (ss.length() == 0 || ss == ssid) continue;
    s[n] = ss; p[n] = netPass(i); n++;
  }
  for (int i = 0; i < MAX_NETS; i++) {
    prefs.putString((String("ns") + i).c_str(), i < n ? s[i] : String(""));
    prefs.putString((String("np") + i).c_str(), i < n ? p[i] : String(""));
  }
  Serial.printf("Saved Wi-Fi network \"%s\" (%d remembered)\n", ssid.c_str(), n);
}

// Join the strongest remembered network. Only useful with 2+ remembered; with
// one, the normal path below does the same job.
bool joinKnownNetworks(unsigned long timeoutMs) {
  WiFiMulti multi;
  int count = 0;
  for (int i = 0; i < MAX_NETS; i++) {
    String ss = netSsid(i);
    if (ss.length() == 0) continue;
    multi.addAP(ss.c_str(), netPass(i).c_str());
    count++;
  }
  if (count < 2) return false;
  WiFi.mode(WIFI_STA);
  lowerTxPower();
  return multi.run(timeoutMs) == WL_CONNECTED;
}

bool trySavedWiFi(unsigned long timeoutMs) {
  WiFi.begin();
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

void connectWiFi(bool forceSetup) {
  // Open setup if asked to (double reset) or if this unit has never joined Wi-Fi.
  bool usePortal = forceSetup || !wm.getWiFiIsSaved();

  while (true) {
    bool connected;

    if (usePortal) {
      Serial.printf("Opening setup network %s\n", apName.c_str());
      // Start the radio in AP mode at low power first, so the setup network
      // doesn't come up at full power and brown out the board.
      WiFi.mode(WIFI_AP_STA);
      lowerTxPower();
      delay(200);
      // First-time setup / new network: wait on the portal until the user saves.
      wm.setConfigPortalTimeout(0);
      ledMode = LED_SETUP;
      connected = wm.startConfigPortal(apName.c_str());
      // User tapped "Exit" without saving -> fall back to the saved network.
      if (!connected) connected = trySavedWiFi(15000);
    } else {
      Serial.println("Joining saved Wi-Fi");
      // Already set up: join saved Wi-Fi. If the router is slow to come back
      // after a power cut, open the portal for 3 minutes, then retry — never
      // get stuck in setup mode forever.
      ledMode = LED_BOOT;
      wm.setConfigPortalTimeout(180);
      connected = joinKnownNetworks(20000);
      if (!connected) connected = wm.autoConnect(apName.c_str());
    }

    if (connected) {
      Serial.printf("Wi-Fi connected: %s  IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      saveNetwork(WiFi.SSID(), WiFi.psk());
      return;
    }

    ledMode = LED_ERROR;
    Serial.println("Wi-Fi not connected, retrying...");
    delay(5000);
    usePortal = !wm.getWiFiIsSaved();
  }
}

// ====================== OVER-THE-AIR UPDATES =========================
// true if version a is newer than b ("1.10.0" > "1.9.3")
bool isNewer(const String& a, const String& b) {
  int ai = 0, bi = 0;
  for (int part = 0; part < 3; part++) {
    long av = a.substring(ai).toInt(), bv = b.substring(bi).toInt();
    if (av != bv) return av > bv;
    ai = a.indexOf('.', ai); bi = b.indexOf('.', bi);
    if (ai < 0 || bi < 0) return ai >= 0 && bi < 0;
    ai++; bi++;
  }
  return false;
}

// Checks the manifest; if a newer version exists, installs it and reboots.
// Only call while on TV audio. Returns quietly on any problem.
void checkForUpdate() {
  String manifestUrl = String(MANIFEST_BASE) + BOARD_FOLDER + "/manifest.json";
  Serial.printf("Checking for update: %s\n", manifestUrl.c_str());

  WiFiClientSecure client;
  client.setInsecure();  // TODO for wide release: pin root certificates
  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, manifestUrl)) return;
  int status = http.GET();
  String body = (status == 200) ? http.getString() : "";
  http.end();
  if (status != 200) {
    Serial.printf("Update check failed: HTTP %d\n", status);
    if (status != 404) remoteLog("update check failed: HTTP " + String(status));
    return;
  }

  String latest = jsonString(body, "version");
  String binUrl = jsonString(body, "url");
  if (latest.length() == 0 || binUrl.length() == 0) return;
  if (!isNewer(latest, FIRMWARE_VERSION)) {
    Serial.printf("Firmware up to date (%s)\n", FIRMWARE_VERSION);
    return;
  }

  Serial.printf("Updating %s -> %s\n", FIRMWARE_VERSION, latest.c_str());
  remoteLog("updating to " + latest);
  setAudio(AUDIO_TV);            // game audio during the update
  ledMode = LED_SETUP;           // slow blink while downloading
  snapHttp.end();

  WiFiClientSecure binClient;
  binClient.setInsecure();
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(binClient, binUrl);
  // Only reached if the update did not happen (success reboots).
  Serial.printf("Update failed (%d): %s\n", httpUpdate.getLastError(),
                httpUpdate.getLastErrorString().c_str());
  remoteLog("update failed: " + httpUpdate.getLastErrorString());
  ledMode = LED_OK;
}

// =========================== CAMERA ==================================
bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;   // 10 MHz camera clock: less radio noise next to the Wi-Fi antenna
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;   // 640x480 is plenty for game-vs-commercial
  config.jpeg_quality = 14;          // slightly smaller uploads, same 640x480 picture
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST; // always send the newest frame

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  return true;
}

// ========================= LIVE VIEW (LAN) ===========================
// Local video stream so the website's Live check page can open a smooth view
// without uploading anything. Runs on core 0 (the main loop and the AI uploads
// run on core 1) and shares the same camera. Only reachable from the home
// network, and only with the per-boot key the unit reports to the website.
String liveKey = "";
WiFiServer liveServer(81);
const unsigned long LIVE_FRAME_GAP_MS = 150;        // ~6 fps cap leaves headroom for uploads
const unsigned long LIVE_MAX_SESSION_MS = 20UL * 60UL * 1000UL;

static bool liveKeyOk(const String& reqLine) {
  int k = reqLine.indexOf("k=");
  if (k < 0 || liveKey.length() == 0) return false;
  return reqLine.substring(k + 2, k + 2 + liveKey.length()).equalsIgnoreCase(liveKey);
}

static void liveHandle(WiFiClient& c) {
  c.setTimeout(2);
  unsigned long t0 = millis();
  String line = "";
  while (c.connected() && millis() - t0 < 2000) {          // first request line only
    if (c.available()) {
      char ch = c.read();
      if (ch == '\n') break;
      if (line.length() < 160) line += ch;
    } else delay(2);
  }
  while (c.available()) c.read();                           // drop the rest of the headers
  if (!liveKeyOk(line)) {
    c.print("HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\nForbidden");
    return;
  }
  if (line.indexOf("GET /stream") == 0) {
    c.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=gdjframe\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n");
    unsigned long start = millis();
    while (c.connected() && WiFi.status() == WL_CONNECTED && millis() - start < LIVE_MAX_SESSION_MS) {
      unsigned long f0 = millis();
      camera_fb_t* fb = esp_camera_fb_get();
      if (!fb) { delay(50); continue; }
      c.printf("--gdjframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)fb->len);
      size_t sent = 0; unsigned long w0 = millis(); bool ok = true;
      while (sent < fb->len) {                              // chunked so a stalled phone can't hold the camera buffer
        size_t n = c.write(fb->buf + sent, min((size_t)1436, fb->len - sent));
        if (n == 0) { if (millis() - w0 > 1500) { ok = false; break; } delay(2); }
        else { sent += n; w0 = millis(); }
      }
      esp_camera_fb_return(fb);
      if (!ok) break;
      c.print("\r\n");
      unsigned long used = millis() - f0;
      if (used < LIVE_FRAME_GAP_MS) delay(LIVE_FRAME_GAP_MS - used);
    }
    return;
  }
  if (line.indexOf("GET /jpg") == 0) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { c.print("HTTP/1.1 503 Busy\r\nConnection: close\r\n\r\n"); return; }
    c.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)fb->len);
    c.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return;
  }
  // Simple page that shows the stream (open http://<unit address>:81/?k=<key>).
  String page = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                "<body style='margin:0;background:#000;color:#fff;font-family:sans-serif'>"
                "<div style='padding:8px;font-size:14px'>Game DJ live view</div>"
                "<img style='width:100%;display:block' src='/stream?k=" + liveKey + "'></body>";
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)page.length());
  c.print(page);
}

static void liveTask(void*) {
  liveServer.begin();
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClient c = liveServer.available();
      if (c) { liveHandle(c); c.stop(); }
    }
    delay(20);
  }
}

// ========================= SNAPSHOTS =================================
unsigned long uploadsOk = 0, uploadsFailed = 0, uploadsRetried = 0;   // since boot, reported to the app
void handleFailure() {
  consecutiveFailures++;
  if (consecutiveFailures >= MAX_FAILURES_BEFORE_TV) {
    ledMode = LED_ERROR;
    setAudio(AUDIO_TV);  // safe default: never leave the game muted
  }
}

// Scene-change burst: compare JPEG sizes of consecutive frames; a big jump
// means the picture changed a lot, so ask the server sooner than the 3 s cadence.
size_t lastFrameLen = 0;
const unsigned long BURST_MIN_GAP_MS = 1200, BURST_PEEK_MS = 400;
const int BURST_MAX_IN_ROW = 3; const int BURST_JUMP_PCT = 28;
int burstsInRow = 0; unsigned long lastPeek = 0;

bool sceneChanged() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return false;
  size_t n = fb->len; esp_camera_fb_return(fb);
  if (lastFrameLen == 0) return false;
  size_t d = n > lastFrameLen ? n - lastFrameLen : lastFrameLen - n;
  return d * 100 >= lastFrameLen * (size_t)BURST_JUMP_PCT;
}

void sendSnapshot() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    handleFailure();
    return;
  }

  int status = -1;
  for (int attempt = 1; attempt <= 2; attempt++) {   // one quick retry on a dropped upload
  if (!snapHttp.begin(tlsClient, SNAPSHOT_URL)) { status = -1; tlsClient.stop(); delay(250); continue; }
  snapHttp.addHeader("Content-Type", "image/jpeg");
  snapHttp.addHeader("X-Device-Id", deviceId);
  snapHttp.addHeader("X-Device-Token", deviceToken);
  snapHttp.addHeader("X-Current-Audio", audioSource == AUDIO_MUSIC ? "music" : "tv");
  snapHttp.addHeader("X-Firmware", FIRMWARE_VERSION);
  snapHttp.addHeader("X-Board", BOARD_NAME);
  snapHttp.addHeader("X-Local-Ip", WiFi.localIP().toString());
  snapHttp.addHeader("X-Live-Key", liveKey);
  snapHttp.addHeader("X-Wifi-Rssi", String(WiFi.RSSI()));
  { String ss = WiFi.SSID(); ss.replace("\r",""); ss.replace("\n",""); snapHttp.addHeader("X-Wifi-Ssid", ss.substring(0, 32));
    String bs = WiFi.BSSIDstr(); snapHttp.addHeader("X-Wifi-Bssid", bs.substring(bs.length() - 8)); }
  snapHttp.addHeader("X-Reset-Reason", resetReasonText);
  snapHttp.addHeader("X-Wifi-Drops", String(wifiDrops));
  snapHttp.addHeader("X-Uptime-S", String((unsigned long)(millis() / 1000)));
  snapHttp.addHeader("X-Upload-Ok", String(uploadsOk));
  snapHttp.addHeader("X-Upload-Failed", String(uploadsFailed));
  snapHttp.addHeader("X-Upload-Retried", String(uploadsRetried));

  lastFrameLen = fb->len;
  status = snapHttp.POST(fb->buf, fb->len);
  if (status > 0) break;
  snapHttp.end(); tlsClient.stop();
  if (attempt == 1) { uploadsRetried++; delay(250); }
  }
  esp_camera_fb_return(fb);

  if (status == 200) {
    String resp = snapHttp.getString();
    snapHttp.end();
    { static unsigned long lastOk = 0;
      if (millis() - lastOk > 60000) { lastOk = millis(); remoteLog("check reply: " + resp.substring(0, 200)); } }
    String action = jsonString(resp, "action");   // expects {"action":"music"} or {"action":"tv"}
    action.toLowerCase();
    if (action == "music") setAudio(AUDIO_MUSIC);
    else if (action == "tv") setAudio(AUDIO_TV);
    else if (action != "hold") {
      static unsigned long lastOdd = 0;
      if (millis() - lastOdd > 60000) {
        lastOdd = millis();
        remoteLog("unexpected reply: " + resp.substring(0, 160));
      }
    }
    long nxtMs = jsonSecondsAsMs(resp, "next_in_seconds");
    if (nxtMs > 0) {
      unsigned long ms = (unsigned long)nxtMs;
      if (ms < MIN_INTERVAL_MS) ms = MIN_INTERVAL_MS;
      if (ms > MAX_INTERVAL_MS) ms = MAX_INTERVAL_MS;
      snapshotIntervalMs = ms;
    } else {
      snapshotIntervalMs = 3000;
    }
    consecutiveFailures = 0;
    uploadsOk++;
    ledMode = LED_OK;
    return;
  }

  snapHttp.end();
  uploadsFailed++;
  if (status <= 0) tlsClient.stop();   // drop a dead kept-alive socket so the next try starts clean
  Serial.printf("Snapshot failed: HTTP %d\n", status);
  static unsigned long lastFailLog = 0;
  if (millis() - lastFailLog > 60000) {
    lastFailLog = millis();
    remoteLog("snapshot failed: HTTP " + String(status) + ", heap " + String(ESP.getFreeHeap()) + " max block " + String(ESP.getMaxAllocHeap()) + ", rssi " + String(WiFi.RSSI()) + ", fails " + String(consecutiveFailures + 1));
  }
  if (status <= 0) {
    static unsigned long lastDiag = 0;
    if (millis() - lastDiag > 60000) {
      lastDiag = millis();
      const char* host = "score-sync-dj.base44.app";
      IPAddress ip; bool dnsOk = WiFi.hostByName(host, ip);
      WiFiClientSecure t; t.setInsecure(); t.setTimeout(8);
      unsigned long t0 = millis();
      int ok = dnsOk ? t.connect(host, 443) : 0;
      char err[100] = ""; int e = t.lastError(err, sizeof(err));
      remoteLog(String("diag dns=") + (dnsOk ? "ok " + ip.toString() : "FAIL") + " dns-server=" + WiFi.dnsIP().toString() + " gw=" + WiFi.gatewayIP().toString() + " tls=" + String(ok) + " err=" + String(e) + " " + String(err) + " in " + String(millis() - t0) + "ms");
      t.stop();
    }
  }
  // Self-heal: a connection that keeps failing while Wi-Fi looks fine gets a clean restart.
  if (status <= 0 && consecutiveFailures + 1 >= 10 && millis() > 90000UL) {
    remoteLog("10 failed checks in a row, restarting");
    delay(300);
    ESP.restart();
  }

  if (status == 401 || status == 403) {
    // Website no longer recognizes this token -> register again as new.
    Serial.println("Token rejected. Registering again.");
    setAudio(AUDIO_TV);
    deviceToken = "";
    prefs.remove("token");
    registerUntilDone();
    return;
  }
  handleFailure();
}

// ============================ MAIN ===================================
void setup() {
  // Wi-Fi start-up current can trip the brown-out detector on both boards and
  // cause a reboot loop (two quick blinks repeating, no setup network).
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // Relay off (TV) at boot: pins released. Detect the board's trigger polarity first.
  detectRelayPolarity();
  relayPin(RELAY_PIN_L, false);
  relayPin(RELAY_PIN_R, false);

  if (FLASH_LED >= 0) {
    pinMode(FLASH_LED, OUTPUT);
    digitalWrite(FLASH_LED, LOW);
  }
  pinMode(STATUS_LED, OUTPUT);
  ledSet(false);

  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(200);
  Serial.printf("\nGame Watching DJ firmware %s on %s\n", FIRMWARE_VERSION, BOARD_NAME);

  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);

  // Unique ID from the chip MAC, e.g. "A4CF12B3C4D5"; setup network uses last 4.
  uint64_t mac = ESP.getEfuseMac();
  char idBuf[13];
  snprintf(idBuf, sizeof(idBuf), "%02X%02X%02X%02X%02X%02X",
           (uint8_t)(mac), (uint8_t)(mac >> 8), (uint8_t)(mac >> 16),
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  deviceId = idBuf;
  apName = "GameDJ-Setup-" + deviceId.substring(8);

  Serial.println("Step 1/4: reading saved settings");
  prefs.begin("gamedj", false);
  deviceToken = prefs.getString("token", "");
  resetReasonText = resetReasonName(esp_reset_reason());
  if (esp_reset_reason() == ESP_RST_BROWNOUT) gTxPower = WIFI_POWER_11dBm;   // only back off after a real power dip

  Serial.println("Step 2/4: checking for double reset (3 s)");
  bool forceSetup = checkDoubleReset();
  Serial.println("Step 3/4: starting Wi-Fi");
  configurePortal();
  connectWiFi(forceSetup);
  Serial.println("Step 4/4: registering with the website");
  registerUntilDone();

  if (!initCamera()) {
    ledMode = LED_ERROR;
    delay(5000);
    ESP.restart();
  }

  {
    char kb[17];
    snprintf(kb, sizeof(kb), "%08X%08X", (unsigned)esp_random(), (unsigned)esp_random());
    liveKey = kb;
    xTaskCreatePinnedToCore(liveTask, "live", 6144, nullptr, 1, nullptr, 0);
  }

  tlsClient.setInsecure();       // TODO for wide release: pin Base44's root certificate
  snapHttp.setReuse(true);       // keep the HTTPS connection open between snapshots
  snapHttp.setTimeout(10000);

  ledMode = LED_OK;
  printStatus();
  remoteLog(String("boot ok on ") + BOARD_NAME + ", Wi-Fi " + WiFi.SSID() + " " + String(WiFi.RSSI()) + " dBm, reset reason " + resetReasonText + ", tx power " + String((int)gTxPower / 4.0, 1) + " dBm");

  remoteLog(relayPolarityInfo);

  // Relay self-test: music for 1.5 s, then back to TV. Red light should flash.
  setAudio(AUDIO_MUSIC);
  delay(1500);
  setAudio(AUDIO_TV);

  checkForUpdate();   // once at start-up, while still on TV audio

#if PIN_TEST
  {
    const int testPins[4] = {13, 14, 15, 2};
    remoteLog("PIN TEST running: pins 13,14,15,2 cycle 2s on/2s off (on = driven LOW)");
    for (int n = 0; ; n++) {
      bool on = (n % 2 == 0);
      for (int i = 0; i < 4; i++) {
        if (on) { pinMode(testPins[i], OUTPUT); digitalWrite(testPins[i], LOW); }
        else    { pinMode(testPins[i], INPUT); }
      }
      delay(30);
      remoteLog(String("pintest ") + (on ? "ON " : "OFF ") + "readback 13=" + digitalRead(13) + " 14=" + digitalRead(14) +
                " 15=" + digitalRead(15) + " 2=" + digitalRead(2));
      delay(1900);
    }
  }
#endif
}

void loop() {
  static unsigned long lastShot = 0;
  static unsigned long lastUpdateCheck = millis();
  handleSerial();

  static unsigned long offlineSince = 0, lastRejoin = 0;
  if (WiFi.status() != WL_CONNECTED) {
    ledMode = LED_ERROR;
    if (audioSource != AUDIO_TV) setAudio(AUDIO_TV);
    if (offlineSince == 0) { offlineSince = millis(); lastRejoin = millis(); wifiDrops++; }
    // Self-heal: the Wi-Fi chip's own auto-reconnect sometimes gives up on a weak
    // signal. Re-join by hand every 25 s, and restart if still offline after 2 min.
    if (millis() - offlineSince > 120000) {
      Serial.println("Offline for 2 minutes, restarting");
      ESP.restart();
    }
    if (millis() - lastRejoin > 25000) {
      lastRejoin = millis();
      Serial.println("Wi-Fi down, re-joining");
      WiFi.disconnect();
      delay(200);
      if (!joinKnownNetworks(15000)) WiFi.begin();
    }
    delay(1000);
    return;
  }
  offlineSince = 0;
  { static unsigned long lastPs = 0;   // radio must never nap: modem sleep drops uploads on some routers
    if (millis() - lastPs > 10000) { lastPs = millis(); WiFi.setSleep(false); } }

  // Every 6 hours, but only during game audio (never mid-commercial).
  if (millis() - lastUpdateCheck >= UPDATE_CHECK_MS && audioSource == AUDIO_TV) {
    lastUpdateCheck = millis();
    checkForUpdate();
  }

  unsigned long since = millis() - lastShot;
  bool due = since >= snapshotIntervalMs;
  if (!due && since >= BURST_MIN_GAP_MS && burstsInRow < BURST_MAX_IN_ROW &&
      millis() - lastPeek >= BURST_PEEK_MS && WiFi.status() == WL_CONNECTED) {
    lastPeek = millis();
    if (sceneChanged()) { due = true; burstsInRow++; }
  }
  if (due) {
    if (since >= snapshotIntervalMs) burstsInRow = 0;
    lastShot = millis();
    sendSnapshot();
  }
  delay(20);
}
