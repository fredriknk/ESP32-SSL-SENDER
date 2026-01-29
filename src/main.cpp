#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

#ifdef WIFI_ENTERPRISE
#include <esp_wpa2.h>
#include <esp_wifi.h>
#endif

#include <Adafruit_NeoPixel.h>

#include "secrets.h"

//#define WIFI_ENTERPRISE  // comment out if  using WPA2-PSK


// ===================== RGB LED =====================
static const int LED_PIN = 2; // M5Stamp C3
Adafruit_NeoPixel rgb(1, LED_PIN, NEO_GRB + NEO_KHZ800);

static void led(uint8_t r, uint8_t g, uint8_t b) {
  rgb.setPixelColor(0, rgb.Color(r, g, b));
  rgb.show();
}
static void blink(uint8_t r, uint8_t g, uint8_t b, int onMs=120, int offMs=120) {
  led(r,g,b); delay(onMs);
  led(0,0,0); delay(offMs);
}


// ===================== SETTINGS =====================
static const uint32_t POST_PERIOD_MS      = 20000;
static const uint32_t NTP_RESYNC_MS       = 6UL * 60UL * 60UL * 1000UL;  // every 6h
static const uint32_t WIFI_MIN_BACKOFF_MS = 1000;
static const uint32_t WIFI_MAX_BACKOFF_MS = 60000;

static const uint32_t HTTP_TIMEOUT_MS     = 8000;   // total read timeout
static const uint32_t TLS_TIMEOUT_MS      = 8000;   // socket timeout
static const uint32_t TLS_HANDSHAKE_MS    = 12000;  // handshake timeout

// ===================== SIMPLE OUTAGE QUEUE =====================
// Keeps last N payloads in RAM and retries later.
// If you also want persistence across reboot, store these in NVS/Preferences.
static const int QUEUE_CAP = 12;
static char queueBuf[QUEUE_CAP][256];
static uint8_t qHead = 0, qTail = 0, qCount = 0;

static bool queuePush(const char* json) {
  if (qCount >= QUEUE_CAP) {
    // drop oldest to keep most recent data
    qTail = (qTail + 1) % QUEUE_CAP;
    qCount--;
  }
  strncpy(queueBuf[qHead], json, sizeof(queueBuf[qHead]) - 1);
  queueBuf[qHead][sizeof(queueBuf[qHead]) - 1] = '\0';
  qHead = (qHead + 1) % QUEUE_CAP;
  qCount++;
  return true;
}

static bool queuePop(char* out, size_t outSz) {
  if (qCount == 0) return false;
  strncpy(out, queueBuf[qTail], outSz - 1);
  out[outSz - 1] = '\0';
  qTail = (qTail + 1) % QUEUE_CAP;
  qCount--;
  return true;
}

// ===================== TIME (for TLS validation) =====================
static bool timeIsValid() {
  time_t now = 0;
  time(&now);
  return (now > 1700000000); // ~2023-11; adjust if you want
}

static bool syncTimeOnce(uint32_t maxWaitMs = 6000) {
  Serial.println("Syncing clock for TLS...");
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");

  const uint32_t start = millis();
  while (millis() - start < maxWaitMs) {
    if (timeIsValid()) return true;
    blink(255, 180, 0, 80, 120); // orange while waiting
  }
  return timeIsValid();
}
#ifdef WIFI_ENTERPRISE
// ===================== EDUROAM CONNECT (WPA2-Enterprise) =====================
static bool configureEnterprise() {
  // Configure WPA2-Enterprise credentials
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)username, strlen(username));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t*)username, strlen(username));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t*)password, strlen(password));
  esp_wifi_sta_wpa2_ent_enable();
  return true;
}
#endif

static bool connect(uint32_t timeoutMs = 30000) {
  Serial.println("WiFiEnterprise: connecting...");
  WiFi.setSleep(false);          // reduces random disconnects on some setups
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);
  #ifdef WIFI_ENTERPRISE
  configureEnterprise();
  WiFi.begin(ssid);
  #else
  WiFi.begin(ssid, password);
  #endif

  const uint32_t start = millis();
  while (!WiFi.isConnected() && millis() - start < timeoutMs) {
    blink(0, 0, 255); // blue blink = connecting
  }

  if (WiFi.isConnected()) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
    led(0, 0, 125); // connected
    return true;
  }

  Serial.println("WiFi connect FAILED");
  #ifdef WIFI_ENTERPRISE
  esp_wifi_sta_wpa2_ent_disable();
  #endif
  return false;
}

// ===================== WIFI EVENT HANDLING + BACKOFF =====================
static volatile bool wifiUp = false;

static uint32_t wifiBackoffMs = WIFI_MIN_BACKOFF_MS;
static uint32_t nextWifiAttemptMs = 0;

static void scheduleWifiReconnect() {
  const uint32_t now = millis();
  nextWifiAttemptMs = now + wifiBackoffMs;
  wifiBackoffMs = min(wifiBackoffMs * 2, WIFI_MAX_BACKOFF_MS);
}

static void resetWifiBackoff() {
  wifiBackoffMs = WIFI_MIN_BACKOFF_MS;
  nextWifiAttemptMs = 0;
}

static void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiUp = true;
      resetWifiBackoff();
      Serial.println("WiFi event: GOT_IP");
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiUp = false;
      Serial.println("WiFi event: DISCONNECTED");
      // WPA2-Enterprise sometimes needs re-enable after disconnects:
      #ifdef WIFI_ENTERPRISE
      esp_wifi_sta_wpa2_ent_disable();
      #endif
      scheduleWifiReconnect();
      break;

    default:
      break;
  }
}

// ===================== HTTPS POST (with timeout + return code) =====================
static int postJson(const char* json) {
  WiFiClientSecure client;
  client.setCACert(ISRG_Root_X1);

  // timeouts help avoid “hang forever” when upstream is flaky
  client.setTimeout(TLS_TIMEOUT_MS / 1000);          // seconds
  client.setHandshakeTimeout(TLS_HANDSHAKE_MS);      // ms (ESP32 core supports this)

  HTTPClient https;
  https.setTimeout(HTTP_TIMEOUT_MS);

  if (!https.begin(client, API_URL)) return -100;    // begin failed

  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-API-Key", API_TOKEN);

  led(160, 0, 255); // purple sending
  int code = https.POST((uint8_t*)json, strlen(json));

  // Only read body if you want (can block if server misbehaves)
  String resp = https.getString();
  https.end();

  Serial.printf("POST -> HTTP %d, resp: %s\n", code, resp.c_str());
  return code;
}

// ===================== BUILD JSON SAFELY =====================
static void buildMeasurementJson(char* out, size_t outSz, int rssi) {
  // Example values (replace with your m.* fields)
  float conc_ppm = 123;
  uint32_t faults = 0xaaaaaaaa;
  int32_t temp_raw = 2951;
  float temp_c = 22.56;
  bool crc_ok = true;
  bool engineering = true;
  uint32_t ref_avg = 100;
  uint32_t act_avg = 98;
  uint32_t serial_number = 12345679;

  // Add a timestamp so you can backfill correctly when offline
  time_t now = 0; time(&now);

  if (engineering) {
    snprintf(out, outSz,
      "{"
        "\"measurement\":\"inir\","
        "\"tags\":{\"measurement_type\":\"inir\",\"inir_serial\":%lu},"
        "\"fields\":{"
          "\"conc_ppm\":%.3f,"
          "\"faults\":%lu,"
          "\"temp_raw\":%ld,"
          "\"temp_c\":%.2f,"
          "\"crc_ok\":%s,"
          "\"rssi\":%d,"
          "\"ref_avg\":%lu,"
          "\"act_avg\":%lu,"
          "\"ts\":%ld"
        "}"
      "}",
      (unsigned long)serial_number,
      conc_ppm,
      (unsigned long)faults,
      (long)temp_raw,
      temp_c,
      (crc_ok ? "true" : "false"),
      rssi,
      (unsigned long)ref_avg,
      (unsigned long)act_avg,
      (long)now
    );
  } else {
    snprintf(out, outSz,
      "{"
        "\"measurement\":\"inir\","
        "\"tags\":{\"measurement_type\":\"inir\",\"inir_serial\":%lu},"
        "\"fields\":{"
          "\"conc_ppm\":%.3f,"
          "\"faults\":%lu,"
          "\"temp_raw\":%ld,"
          "\"temp_c\":%.2f,"
          "\"crc_ok\":%s,"
          "\"rssi\":%d,"
          "\"ts\":%ld"
        "}"
      "}",
      (unsigned long)serial_number,
      conc_ppm,
      (unsigned long)faults,
      (long)temp_raw,
      temp_c,
      (crc_ok ? "true" : "false"),
      rssi,
      (long)now
    );
  }
}

// ===================== FLUSH QUEUE (retry with limits) =====================
static void flushQueue() {
  if (!wifiUp) return;
  if (!timeIsValid()) return;

  // Try sending a few queued items per loop so we don’t starve everything else
  const int maxPerFlush = 3;
  for (int i = 0; i < maxPerFlush; i++) {
    char payload[256];
    if (!queuePop(payload, sizeof(payload))) return;

    int code = postJson(payload);
    if (code >= 200 && code < 300) {
      blink(0, 0, 255, 120, 80); // blue = success
      continue;
    }

    // If failed, push it back and stop (avoid spinning)
    queuePush(payload);

    // red = fail
    blink(255, 0, 0, 200, 120);
    return;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  rgb.begin();
  led(0,0,0);

  WiFi.onEvent(onWiFiEvent);

  // Initial connect attempt
  bool ok = connect();
  if (ok) {
    wifiUp = true;
    resetWifiBackoff();
    syncTimeOnce();
  } else {
    wifiUp = false;
    scheduleWifiReconnect();
  }
}

void loop() {
  const uint32_t now = millis();

  // --- WiFi reconnect logic (backoff) ---
  if (!wifiUp && (int32_t)(now - nextWifiAttemptMs) >= 0) {
    blink(255, 0, 0, 60, 60); // red blink while trying
    if (connect()) {
      wifiUp = true;
      resetWifiBackoff();
      syncTimeOnce();
    } else {
      wifiUp = false;
      scheduleWifiReconnect();
    }
  }

  // --- Periodic NTP resync (TLS robustness) ---
  static uint32_t lastNtp = 0;
  if (wifiUp && (now - lastNtp > NTP_RESYNC_MS || !timeIsValid())) {
    if (syncTimeOnce()) lastNtp = now;
  }

  // --- Heartbeat ---
  if (wifiUp) {
    led(0, 125, 0);
    delay(120);
    led(0, 0, 0);
    delay(80);
  } else {
    // offline heartbeat
    led(40, 0, 0);
    delay(80);
    led(0, 0, 0);
    delay(120);
  }

  // --- Build + enqueue measurement every POST_PERIOD_MS ---
  static uint32_t lastSample = 0;
  if (now - lastSample >= POST_PERIOD_MS) {
    char json[256];
    buildMeasurementJson(json, sizeof(json), WiFi.RSSI());
    queuePush(json);
    lastSample = now;
  }

  // --- Try flushing queued data if possible ---
  flushQueue();
}
