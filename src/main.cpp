#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

#include <esp_wpa2.h>
#include <esp_wifi.h>

#include <Adafruit_NeoPixel.h>

#include "secrets.h"

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

// ===================== TIME (for TLS validation) =====================
static void syncTime() {
  Serial.println("Synching clock for ssl");
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  time_t now = 0;
  for (int i = 0; i < 40; i++) {
    time(&now);
    if (now > 1700000000) return;
    blink(255, 180, 0); // orange while waiting for time
  }
}

// ===================== HTTPS POST =====================
static bool postToNodeRed(int rssi=-1) {
  WiFiClientSecure client;
  client.setCACert(ISRG_Root_X1);

  HTTPClient https;
  if (!https.begin(client, API_URL)) return false;

  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-API-Key", API_TOKEN);

  // Example values (replace with your m.* fields)
  float conc_ppm = 123;
  uint32_t faults = 0xaaaaaaaa;
  int32_t temp_raw = 2951;
  float temp_c = 22.56;
  bool crc_ok = true;
  bool engineering = true;
  uint32_t ref_avg = 100;
  uint32_t act_avg = 98;
  uint32_t serial_number = 12345678;
  //{"measurement":"env_chamber",
  // "tags":{"measurement_type":"picarro"},
  // "fields":{"nc1":0,"NH4":2.6231714285714287,"N20":0.33442380952380946,"CO2":469.31328571428577,"CH4":2.240438095238096,"H20":0.2723047619047619,"nc2":0,"nc3":0,"time":1769677128683,"motor_speed":0,"ch4setpoint":0}}
  String json = "{";
  json += "\"measurement\":\"inir\",";
  json += "\"tags\":{\"measurement_type\":\"inir\",\"inir_serial\":" + String(serial_number) + "},";
  json += "\"fields\":{";
  json += "\"conc_ppm\":" + String(conc_ppm, 3) + ",";
  json += "\"faults\":" + String(faults) + ",";
  json += "\"temp_raw\":" + String(temp_raw) + ",";
  json += "\"temp_c\":" + String(temp_c, 2) + ",";
  json += "\"crc_ok\":" + String(crc_ok ? "true" : "false");
  json += ",\"rssi\":" + String(rssi);

  if (engineering) {
    json += ",\"ref_avg\":" + String(ref_avg);
    json += ",\"act_avg\":" + String(act_avg);
  }

  json += "}}";


  led(160, 0, 255); // purple sending
  int code = https.POST(json);
  String resp = https.getString();
  https.end();

  Serial.printf("POST -> HTTP %d, resp: %s\n", code, resp.c_str());
  return (code >= 200 && code < 300);
}

// ===================== EDUROAM CONNECT =====================
static bool connectEduroam() {
  Serial.println("WiFiEnterprise: Starting connection to WPA2-Enterprise network");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("Username: ");
  Serial.println(username);
  
  // Disconnect any existing connection
  WiFi.disconnect(true);
  delay(1000);
  
  // Set WiFi mode to station
  WiFi.mode(WIFI_STA);
  
  Serial.println("WiFiEnterprise: Configuring WPA2-Enterprise settings");
  
  // Configure WPA2-Enterprise
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)username, strlen(username));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t *)username, strlen(username));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t *)password, strlen(password));
  
  // Enable WPA2-Enterprise
  esp_wifi_sta_wpa2_ent_enable();
  //esp_wifi_sta_wpa2_ent_set_ca_cert(CA_PEM, sizeof(CA_PEM)); // Use NULL to skip server certificate validation (not recommended for production)
  
  Serial.println("WiFiEnterprise: Attempting to connect...");
  
  // Begin connection
  WiFi.begin(ssid);

  Serial.print("Connecting to eduroam");
  unsigned long start = millis();
  while (!WiFi.isConnected() && millis() - start < 30000) {
    blink(0, 0, 255); // blue blink = connecting
    Serial.print(".");
    Serial.print(String(WiFi.status()).c_str());
  }
  Serial.println();

  if (WiFi.isConnected()) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
    led(0, 0, 125); // blue = connected
    return true;
  }

  Serial.println("WiFi connect FAILED");
  esp_wifi_sta_wpa2_ent_disable();
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  rgb.begin();
  led(0,0,0);

  if (connectEduroam()) {
    syncTime(); // helps HTTPS TLS validation (Let’s Encrypt)
  }
}

void loop() {
  if (!WiFi.isConnected()) {
    // disconnected
    blink(255, 0, 0, 100, 100); // red fast blink
    return;
  }

  // Connected: show green heartbeat
  led(0, 125, 0);
  delay(400);
  led(0, 0, 0);
  delay(100);

  static unsigned long lastPost = 0;
  if (millis() - lastPost >= 20000) {
    WiFi.RSSI();
    bool ok = postToNodeRed(WiFi.RSSI());
    if (ok) {
      blink(0, 0, 255, 150, 150); // blue = success
    } else {
      blink(255, 0, 0, 255, 200); // red = fail
    }
    lastPost = millis();
  }
}