#include <TinyGPS++.h>
#include <TinyGPSPlus.h>
#include <PPP.h>                // built into ESP32 Arduino core 3.x (SIM800L data connection)
#include <WiFiManager.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>   // the ESP32 does the TLS/HTTPS itself
#include <WiFiClient.h>
#include <HTTPClient.h>

// ============================================================================
// SAFETRACE-AI FIRMWARE: BIG BANG 6  (SIM800L + Wi-Fi, ESP32 does the TLS)
//
// Two ways to reach the internet. The ESP32 builds the HTTPS (TLS) request
// itself either way:
//   1) SIM800L  -> used only as the data connection (PPP over 2G)
//   2) Wi-Fi    -> joins the saved hotspot (set up through the captive portal)
//
// YOU CHOOSE with NET_MODE below:
//   MODE_GSM_FIRST  : use the SIM. If the SIM cannot give data (airtime/data
//                     finished, no 2G coverage, ...) switch to Wi-Fi.
//   MODE_GSM_ONLY   : SIM only. Wi-Fi and the setup page are switched off.
//   MODE_WIFI_ONLY  : Wi-Fi only. The SIM800L is never started.
//   MODE_WIFI_FIRST : use Wi-Fi. If there is no Wi-Fi, switch to the SIM.
//
// HOW TO CHANGE THE HOTSPOT (same as Version 3):
//  A) Connect GPIO15 to GND (a jumper wire or a switch) for about 1 second.
//     Works at power-up and also while the device is running -> the setup
//     page opens.
//  B) Or just wait: if there has been NO working internet (SIM and Wi-Fi) for
//     2 minutes, the setup page opens by itself.
//
//  WIRING
//   SIM800L TX  -> ESP32 GPIO27 (GSM_RX_PIN)
//   SIM800L RX  -> ESP32 GPIO26 (GSM_TX_PIN)
//   SIM800L GND -> ESP32 GND (common ground is a must)
//   SIM800L VCC -> its OWN supply of 3.7-4.2 V that can give 2 A for a moment
//                  + a 1000 uF capacitor close to the module. Do NOT power it
//                  from the ESP32 3V3 or 5V pin.
//   NEO-6M TX   -> ESP32 GPIO16,  NEO-6M RX -> ESP32 GPIO17
//
// SETUP PAGE: on any phone/laptop connect to the Wi-Fi called "SafeTrace-Setup"
// (password: safetrace123). A page opens (or go to 192.168.4.1), tap
// "Configure WiFi", pick the hotspot, type its password, Save.
// NOTE: the device you use for setup should NOT be the phone that is providing
// the hotspot unless your device can turn on hotspot and wifi at the same time.
// Use a second phone/laptop, or switch the hotspot off on your
// phone while you do the setup, then switch it back on.
// ============================================================================

// ======================= CHOOSE THE CONNECTION MODE =======================
#define MODE_GSM_FIRST   0 //DOESNT REFER TO ACTUAL/ANY PIN CONNECTION
#define MODE_GSM_ONLY    1
#define MODE_WIFI_ONLY   2
#define MODE_WIFI_FIRST  3
const int NET_MODE = MODE_GSM_FIRST;

bool sosPending = false;
// ==========================================================================

const int NET_FAIL_LIMIT       = 2;        // failed sends in a row before switching to the other connection
const unsigned long AVOID_MS   = 600000;   // after switching away, prefer the other connection for 10 minutes

// ---------------- GSM (SIM800L) ----------------
const char GSM_APN[]  = "internet.ng.airtel.com";   // Airtel
const char *SIM_PIN   = NULL;                       // put "1234" here only if the SIM has a PIN lock
const int  GSM_BAUD   = 115200;                     // SIM800L speed (autobaud; try 9600 if it never responds)


// ---------------- Setup portal ----------------
const char PORTAL_NAME[] = "SafeTrace-Setup";
const char PORTAL_PASS[] = "safetrace123";            // min 8 characters
const unsigned long PORTAL_TIMEOUT_S    = 180;        // portal closes after this
const unsigned long PORTAL_AFTER_DOWN   = 120000;     //  ms of no working internet before portal opens

// ---------------- Server (HTTPS) ----------------
const char SERVER_URL[] = "https://safetrace-ai.onrender.com/api/v1/hardware/location"; // "https://sample-proxy-server.onrender.com";
const char DEVICE_ID[]  = "ST-6840";
const char DEVICE_KEY[] = "78a72f39071fad178f98351c8c28329e";

const bool INSECURE_TLS = true;   // demo only: encrypts but doesn't verify the server


// ---------------- Options ----------------
const bool SEND_BATTERY = false;
const bool SEND_SOS_FIELD = true;

const unsigned long TRACK_INTERVAL = 30000;
const unsigned long SOS_DEBOUNCE   = 2000;
const float DIVIDER_RATIO    = 2.0;    // 1:1 resistor divider


// ---------------- Pins ----------------
#define GPS_RX_PIN      16 // GPS TX COMES HERE
#define GPS_TX_PIN      17 // GPS RX COMES HERE
#define GPS_BAUDRATE    9600
#define BATT_ADC_PIN    34 // PIN TO READ BATTERY RATING
#define SOS_BUTTON_PIN  4 // PIN TO SEND SOS SIGNAL
#define WIFI_SETUP_PIN  15 // internal pull-up; connect to GND to FORCE open the Wi-Fi setup page
#define SOS_LED 2 // pin that signals sos button has been pressed and successfully sent 
#define GSM_RX_PIN 27   // ESP32 RX  <-  SIM800 TX
#define GSM_TX_PIN 26   // ESP32 TX  ->  SIM800 RX 

// ---------------- State ----------------
enum LinkType { LINK_NONE, LINK_GSM, LINK_WIFI };

TinyGPSPlus gps;
unsigned long lastTrack  = 0;
unsigned long lastSos    = 0;
unsigned long lastStatus = 0;

bool modemBegun   = false;
bool inDataMode   = false;
int  dataFailures = 0;


LinkType activeLink = LINK_NONE;        // the connection currently in use
LinkType avoidLink  = LINK_NONE;        // a connection that just failed
unsigned long avoidUntil = 0;
int  netFailures = 0;                   // failed sends in a row on the active connection
unsigned long noInternetSince = 0;      // 0 = internet is fine

// ============================================================================
// Helpers
// ============================================================================
bool gsmAllowed()  { return NET_MODE != MODE_WIFI_ONLY; }
bool wifiAllowed() { return NET_MODE != MODE_GSM_ONLY; }

const char *linkName(LinkType l) {
  return l == LINK_GSM ? "SIM (GSM)" : (l == LINK_WIFI ? "Wi-Fi" : "none");
}

// ================================================================================
// SETUP / LOOP MAIN FUNCTIONS
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial2.setRxBufferSize(1024);
  Serial2.begin(GPS_BAUDRATE, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);
  pinMode(WIFI_SETUP_PIN, INPUT_PULLUP);
  pinMode(SOS_LED, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);

  Serial.println(F("\n=================================================="));
  Serial.println(F("  SAFETRACE-AI FIRMWARE  (WI-FI / GSM / HTTPS)"));
  Serial.println(F("=================================================="));
  Serial.println(F("  *** PHASE 0: Sending real GPS fixes ***"));

  Serial.print(F("  Connection mode: "));
  switch (NET_MODE) {
    case MODE_GSM_FIRST:  Serial.println(F("SIM first, Wi-Fi as backup")); break;
    case MODE_GSM_ONLY:   Serial.println(F("SIM only"));                   break;
    case MODE_WIFI_ONLY:  Serial.println(F("Wi-Fi only"));                 break;
    case MODE_WIFI_FIRST: Serial.println(F("Wi-Fi first, SIM as backup")); break;
  }

  // GPIO15 tied to GND at power-up forces the setup page
  bool forcePortal = false;
  if (wifiAllowed() && digitalRead(WIFI_SETUP_PIN) == LOW) {
    delay(1000);
    forcePortal = (digitalRead(WIFI_SETUP_PIN) == LOW);
  }

  if (forcePortal) {
    openPortal();
  } else if (!ensureNetwork()) {
    // Neither the SIM nor a saved Wi-Fi works
    if (wifiAllowed()) {
      openPortal();
    } else {
      Serial.println(F("[NET] No data connection. Check the SIM (airtime/data, coverage) and the SIM800L wiring."));
    }
  }

  lastTrack = millis() - TRACK_INTERVAL - 1;

}

void loop() {
  feedGPS();

  if (millis() - lastStatus > 5000) {
    lastStatus = millis();
    printStatus();
  }

  if (wifiAllowed()) {
    // GPIO15 tied to GND for 1 second while running opens the setup page
    if (digitalRead(WIFI_SETUP_PIN) == LOW) {
      delay(1000);
      if (digitalRead(WIFI_SETUP_PIN) == LOW) {
        openPortal();
      }
    }

  // Open the setup page by itself if there has been no working internet for a while
    if (noInternetSince != 0 && millis() - noInternetSince > PORTAL_AFTER_DOWN) {
      openPortal();
      noInternetSince = 0;
    }
  }

  // SOS button (reads LOW when pressed)
  if (digitalRead(SOS_BUTTON_PIN) == LOW && (millis() - lastSos > SOS_DEBOUNCE)) {
    lastSos = millis();
    Serial.println(F("\n[SOS] Button pressed!"));
    handleSos(); //normal sos operation
  }

  // Regular updates
  if (millis() - lastTrack > TRACK_INTERVAL) {
    if (gps.location.isValid() && gps.location.age() < 3000 && gps.satellites.value() >= 4) {
      sendReading(gps.location.lat(), gps.location.lng(), gps.speed.kmph(),
                  gps.satellites.value(), false);
      lastTrack = millis();
    }
  }
  
  if (sosPending && gps.location.isValid()) {
  sosPending = false;
  handleSos(); //sends sos immediately after reconnecting if signal was lost during signal attempt
  }

}








void feedGPS() {
  while (Serial2.available() > 0) {
    gps.encode(Serial2.read());
  }
}

//TO READ BATTERY VOLTAGE IF IT EVENTUALLY IS CONFIGURED
float readBatteryVolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogReadMilliVolts(BATT_ADC_PIN);
    delay(2);
  }
  return ((sum / 16.0f) * DIVIDER_RATIO) / 1000.0f;
}











//=================================================================================
//NETWORK: DATA AND WIFI. 
//OPENS THE CAPTIVE PORTAL SETUP PAGE. Blocks until you save a network or the timeout passes.
//============================================================================
void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void gsmOff() {
  if (modemBegun) {
    PPP.end();
    modemBegun = false;
    inDataMode = false;
    dataFailures = 0;
  }
}

void openPortal() {
  Serial.println(F("\n[PORTAL] Setup page is open."));
  Serial.print(F("[PORTAL] Connect to Wi-Fi \""));
  Serial.print(PORTAL_NAME);
  Serial.print(F("\" (password "));
  Serial.print(PORTAL_PASS);
  Serial.println(F("), then open 192.168.4.1"));

  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(20);
  wm.setBreakAfterConfig(true);   // close the page after Save, even if the hotspot is OFF right now;
                                  // the details stay saved and the ESP32 keeps retrying
  bool ok = wm.startConfigPortal(PORTAL_NAME, PORTAL_PASS);

  if (ok) {
    Serial.print(F("[PORTAL] Saved and connected to \""));
    Serial.print(WiFi.SSID());
    Serial.println(F("\""));
  } else {
    Serial.println(F("[PORTAL] Closed without a connection."));
  }

  // Tidy up so only one connection is in use afterwards
  netFailures = 0;
  noInternetSince = 0;
  if (activeLink == LINK_GSM && PPP.connected()) {
    wifiOff();                                  // keep using the SIM
  } else if (activeLink == LINK_NONE && WiFi.status() == WL_CONNECTED) {
    gsmOff();                                   // nothing was working: use the new Wi-Fi now
    activeLink = LINK_WIFI;
  }
}

//CONNECTS USING THE WIFI-NETWORK SAVED IN FLASH FROM CAPTIVE PORTAL
bool ensureWifi(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println(F("[WIFI] Connecting to the saved network ..."));
  WiFi.mode(WIFI_STA);
  WiFi.begin();                       // no arguments = use saved credentials

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    feedGPS();
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WIFI] Connected to \""));
    Serial.print(WiFi.SSID());
    Serial.print(F("\"  IP: "));
    Serial.print(WiFi.localIP());
    Serial.print(F("  signal: "));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));
    return true;
  }

  Serial.println(F("[WIFI] FAILED. Check the hotspot is ON and 2.4 GHz."));
  Serial.println(F("       To use a different hotspot, connect GPIO15 to GND for 1 second."));
  return false;
}

// CONNECTS USING DATA FROM SIM IN GSM: Brings up the SIM800L data connection. Returns true when the ESP32 has an IP.
bool ensureData(unsigned long registerTimeoutMs) {
  if (PPP.connected()) return true;

  // 1) Start the modem
  if (!modemBegun) {
    Serial.println(F("[GSM] Starting the SIM800L ..."));
    PPP.setApn(GSM_APN);
    if (SIM_PIN) PPP.setPin(SIM_PIN);
    PPP.setPins(GSM_TX_PIN, GSM_RX_PIN);
    if (!PPP.begin(PPP_MODEM_SIM800, 1, GSM_BAUD)) {
      Serial.println(F("[GSM] The modem did not respond. Check: SIM800L power (2 A peaks),"));
      Serial.println(F("      common GND, TX/RX not swapped (GSM TX -> GPIO27, GSM RX -> GPIO26)."));
      return false;
    }
    modemBegun = true;
    inDataMode = false;
  }

  // 2) Wait until the SIM is registered on the 2G network
  unsigned long start = millis();
  Serial.print(F("[GSM] Waiting for the network"));
  while (!PPP.attached() && millis() - start < registerTimeoutMs) {
    delay(250);
    feedGPS();
    Serial.print('.');
  }
  Serial.println();

  if (!PPP.attached()) {
    Serial.println(F("[GSM] Not registered on the network. Check: SIM has data/airtime, 2G coverage,"));
    Serial.println(F("      antenna fitted, SIM seated, SIM PIN lock off."));
  } else {
    // 3) Switch to data mode (PPP) and wait for an IP address
    if (!inDataMode) {
      Serial.print(F("[GSM] Signal (AT+CSQ): "));
      Serial.println(PPP.cmd("AT+CSQ", 3000));
    }
    Serial.println(F("[GSM] Switching to data mode ..."));
    PPP.mode(ESP_MODEM_MODE_DATA);
    inDataMode = true;

    unsigned long t = millis();
    while (!PPP.connected() && millis() - t < 45000) {
      delay(250);
      feedGPS();
      Serial.print('.');
    }
    Serial.println();
  }

  if (PPP.connected()) {
    dataFailures = 0;
    Serial.print(F("[GSM] Data connection UP. IP: "));
    Serial.println(PPP.localIP());
    return true;
  }

  // Failed: after two failures do a full restart of the modem next time
  Serial.println(F("[GSM] No data connection yet."));
  if (++dataFailures >= 2) {
    Serial.println(F("[GSM] Restarting the modem link for the next try."));
    PPP.end();
    modemBegun = false;
    inDataMode = false;
    dataFailures = 0;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Choosing / switching the connection
// Only ONE connection is kept up at a time, so the data always goes out
// through the one we chose.
// ---------------------------------------------------------------------------
bool linkUp(LinkType l) {
  if (l == LINK_GSM)  return PPP.connected();
  if (l == LINK_WIFI) return WiFi.status() == WL_CONNECTED;
  return false;
}

bool connectTo(LinkType l) {
  if (l == LINK_GSM) {
    if (WiFi.getMode() != WIFI_OFF) wifiOff();
    return ensureData(120000);
  }
  gsmOff();
  return ensureWifi(20000);
}

bool ensureNetwork() {
  if (linkUp(activeLink)) return true;

  LinkType first  = (NET_MODE == MODE_GSM_FIRST || NET_MODE == MODE_GSM_ONLY) ? LINK_GSM : LINK_WIFI;
  LinkType second = (first == LINK_GSM) ? LINK_WIFI : LINK_GSM;

  // If the first choice has just failed, try the other one first
  if (avoidLink == first && (long)(avoidUntil - millis()) > 0) {
    LinkType tmp = first; first = second; second = tmp;
  }

  LinkType order[2] = { first, second };
  for (int i = 0; i < 2; i++) {
    LinkType l = order[i];
    if (l == LINK_GSM  && !gsmAllowed())  continue;
    if (l == LINK_WIFI && !wifiAllowed()) continue;

    Serial.print(F("[NET] Trying "));
    Serial.println(linkName(l));
    if (connectTo(l)) {
      activeLink = l;
      Serial.print(F("[NET] Using "));
      Serial.println(linkName(l));
      return true;
    }
  }
  activeLink = LINK_NONE;
  return false;
}

// Called when a send failed because of the connection itself
// (no link, no reply, or the carrier's "buy data" web page instead of the API)
void noteNetworkFailure() {
  if (noInternetSince == 0) noInternetSince = millis();
  netFailures++;
  if (netFailures >= NET_FAIL_LIMIT) {
    netFailures = 0;
    bool otherAllowed = (activeLink == LINK_GSM) ? wifiAllowed() : gsmAllowed();
    if (activeLink != LINK_NONE && otherAllowed) {
      Serial.print(F("[NET] "));
      Serial.print(linkName(activeLink));
      Serial.println(F(" is not giving internet. Switching to the other connection."));
      avoidLink  = activeLink;
      avoidUntil = millis() + AVOID_MS;
      activeLink = LINK_NONE;       // forces ensureNetwork() to choose again
    }
  }
}

void noteInternetOk() {
  netFailures = 0;
  noInternetSince = 0;
}
//=========================================================================
//END OF NETWORK
//=================================================================================









//=================================================================================
//LOCATION DATA TRANSFER PROTOCOLS
// HTTPS POST (TLS is done here on the ESP32; the data goes out over the
// SIM800L or Wi-Fi, whichever is active)
//============================================================================
//packages reading to be sent
String buildPayload(double lat, double lon, double speedKmh, uint32_t sats, bool sos) {
  String p = "{\"deviceId\":\"" + String(DEVICE_ID) + "\"";
  p += ",\"deviceKey\":\"" + String(DEVICE_KEY) + "\"";
  p += ",\"lat\":" + String(lat, 6);
  p += ",\"lon\":" + String(lon, 6);
  p += ",\"speed\":" + String(speedKmh, 1);
  p += ",\"sats\":" + String(sats);
  if (SEND_BATTERY) p += ",\"batt\":" + String(readBatteryVolts(), 2);
  if (SEND_SOS_FIELD && sos) p += ",\"isSos\":true";
  p += "}";
  return p;
}

//sends over internet
bool postJson(const String &payload) {
  if (!ensureNetwork()) {
    Serial.println(F("[NET] No internet connection available (SIM and Wi-Fi)."));
    if (noInternetSince == 0) noInternetSince = millis();
    return false;
    /*if (!ensureWifi(20000)) return false;
    if (!ensureData(120000)) return false;*/
  }


  WiFiClientSecure client;
  if (INSECURE_TLS) client.setInsecure();

  client.setTimeout(100); // seconds (2G is slow and Render may be waking up)

  HTTPClient http;
  http.setConnectTimeout(50000); // the TLS handshake over 2G can take a while hence 50s
  http.setTimeout(90000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, SERVER_URL)) {
    Serial.println(F("[HTTPS] Could not start the request."));
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  Serial.print(F("[HTTPS] Sending over "));
  Serial.print(linkName(activeLink));
  Serial.println(F("... (on 2G this can take 30-90 s, longer if the server is asleep)"));
  int code = http.POST(payload);

  bool looksLikeHtml = false;
  bool networkProblem = false;

  if (code > 0) {
    String body = http.getString();
    looksLikeHtml = (body.indexOf("<html") >= 0 || body.indexOf("<script") >= 0);

    Serial.print(F("[HTTP STATUS] "));
    Serial.println(code);
    Serial.print(F("[HTTP BODY] "));
    Serial.println(body);

    if (looksLikeHtml){
      networkProblem = true;
      Serial.println(F("[HINT] Got an HTML page, not an API reply - something is blocking the request."));
      Serial.println(F("       (on a SIM this is often the carrier's 'buy data' page)."));
    }
    if (code == 401) Serial.println(F("[HINT] 401 = device ID or key not accepted."));
    if (code == 400 || code == 422) Serial.println(F("[HINT] The server rejected the JSON fields."));
  } else {
    networkProblem = true;
    Serial.print(F("[HTTP ERROR] "));
    Serial.println(http.errorToString(code));
  }
  http.end();
  if (networkProblem) noteNetworkFailure();
  else                noteInternetOk();
  return (code == 200 || code == 201) && !looksLikeHtml;
}


bool sendReading(double lat, double lon, double speedKmh, uint32_t sats, bool sos) {
  String payload = buildPayload(lat, lon, speedKmh, sats, sos);
  Serial.print(F("[PAYLOAD] "));
  Serial.println(payload);

  bool ok = postJson(payload);
  Serial.println(ok ? F("[SEND OK]") : F("[SEND FAILED]"));
  return ok;
}
//===========================================================================
//END OF DATA TRANSFER 
//================================================================================












//================================================================================
// SOS FUNCTION TO HANDLE SOS SIGNAL
//============================================================================
void handleSos() {
  double lat = 0, lon = 0, spd = 0;
  uint32_t sats = 0;
  
  if (gps.location.isValid()) {
    lat = gps.location.lat();
    lon = gps.location.lng();
    spd = gps.speed.kmph();
    sats = gps.satellites.value();
  } else {
    Serial.println(F("[SOS] No GPS fix has been obtained yet - cannot send a position."));
    sosPending = true;
    return;
  }

  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.print(F("[SOS] Send attempt "));
    Serial.println(attempt);
    if (sendReading(lat, lon, spd, sats, true)) {
      lastTrack = millis();
      Serial.println("[SOS] has been sent!!!");
      blinkSosLed();
      return;
    }
    delay(2000);
  }
  Serial.println(F("[SOS] All attempts failed."));
  noSos();
}

// BlinkS the SOS LED twice within 1 second for every successful attempt
void blinkSosLed() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(SOS_LED, HIGH);
    delay(250);
    digitalWrite(SOS_LED, LOW);
    delay(250);
  }
}

void noSos(){ //if all sos attempts fail
  digitalWrite(SOS_LED,HIGH);
  delay(1000);
  digitalWrite(SOS_LED,LOW);
  //delay(500);
}
//============================================================================
//END OF SOS
//=================================================================================













//=================================================================================
// Status line: CONTAINS WHAT SHOULD BE DISPLAYED ON SERIAL MONITOR
//============================================================================
void printStatus() {
  Serial.printf("[STATUS] net=%s gsm=%s wifi=%s | gps chars=%lu sats=%lu fix=%s age=%lums\n",
                linkName(activeLink),
                PPP.connected() ? "UP" : "DOWN",
                WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
                (unsigned long)gps.charsProcessed(),
                (unsigned long)gps.satellites.value(),
                gps.location.isValid() ? "YES" : "no",
                (unsigned long)gps.location.age());
  if (gps.charsProcessed() < 10) {
    Serial.println(F("[GPS] No data from the NEO-6M - check GPS TX -> GPIO16, 3V3 and GND."));
  }
}
//============================================================================
//END OS STATUS LINE
//=================================================================================