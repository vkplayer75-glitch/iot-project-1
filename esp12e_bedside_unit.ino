/*  DecuGuard bedside unit: ESP-12E (NodeMCU) -> Firebase Realtime Database
    Libraries: "Firebase Arduino Client Library for ESP8266 and ESP32" (mobizt), "Adafruit ADS1X15", "DFRobotDFPlayerMini"
    Board: NodeMCU 1.0 (ESP-12E Module)

    WIRING (NodeMCU pin = GPIO)
      I2C bus      SDA D2(GPIO4)  SCL D1(GPIO5)   -> ADS1115 (0x48) and PCF8574 (0x20, extra part: ESP-12E has too few pins)
      FSR zones    ADS1115 A0..A3 = left shoulder, right shoulder, sacrum, left heel
                   NodeMCU A0     = right heel   (each FSR: 3.3V -> FSR -> node -> 10k -> GND)
      HC-SR04      ECHO left D5(GPIO14), ECHO right D6(GPIO12)  (5V echo -> divider 1k/2k to 3.3V)
                   TRIG left = PCF8574 P0, TRIG right = PCF8574 P1
      PIR          bedside D7(GPIO13), room D0(GPIO16)
      DFPlayer     DF-TX -> D3(GPIO0), DF-RX <- D4(GPIO2) through 1k   (unplug DF-TX while flashing)
      Outputs on PCF8574 (sink type: 3.3V -> LED/buzzer -> pin): P2 green, P3 yellow, P4 red, P5 buzzer
      SD card: 0001.mp3 "please reposition", 0002.mp3 "please stay in bed"
*/
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <Adafruit_ADS1X15.h>
#include <DFRobotDFPlayerMini.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

#define WIFI_SSID "YOUR_WIFI"
#define WIFI_PASS "YOUR_PASSWORD"
#define API_KEY   "YOUR_API_KEY"
#define DB_URL    "https://YOUR_PROJECT-default-rtdb.firebaseio.com"
#define DEVICE_ID "bed-01"                 // must match Device ID in the web app

const String ROOT = String("/devices/") + DEVICE_ID;
const uint8_t ECHO_L = 14, ECHO_R = 12, PIR_BED = 13, PIR_ROOM = 16;
const float ADC_FULL = 26400.0;            // ADS1115 counts at 3.3 V: calibrate with a known load

Adafruit_ADS1115 ads;
SoftwareSerial dfSerial(0, 2);             // RX, TX
DFRobotDFPlayerMini mp3;
FirebaseData fbdo; FirebaseAuth auth; FirebaseConfig config;

uint8_t pcf = 0xFF;
float edgeCm = 25, turnMin = 120, fsMm = 80;
long lastCmd = 0; unsigned long tHist = 0, tPush = 0, tCmd = 0, tSet = 0, buzOff = 0, highSince = 0;
bool turnSaid = false, exitSaid = false;

void pcfBit(uint8_t b, bool high) {
  if (high) pcf |= (1 << b); else pcf &= ~(1 << b);
  Wire.beginTransmission(0x20); Wire.write(pcf); Wire.endTransmission();
}
void led(uint8_t b, bool on) { pcfBit(b, !on); }            // active low
void buzz(unsigned long ms)  { led(5, true); buzOff = millis() + ms; }

int readCm(uint8_t i, uint8_t echo) {
  pcfBit(i, false); pcfBit(i, true); delayMicroseconds(12); pcfBit(i, false);
  long us = pulseIn(echo, HIGH, 30000);
  return us ? us / 58 : 400;
}
int pct(int16_t raw) { return constrain((int)(raw / ADC_FULL * 100.0), 0, 100); }

void act(const String& a, int v) {
  if (a == "voice_turn") mp3.play(1);
  else if (a == "voice_stay") mp3.play(2);
  else if (a == "buzzer") buzz(3000);
  else if (a == "silence") { led(5, false); mp3.stop(); }
  else if (a == "volume") mp3.volume(constrain(v, 0, 30));
}

void safetyLogic(int p[5], int l, int r) {          // runs on the device, works even if the browser is closed
  int mx = 0; for (int i = 0; i < 5; i++) mx = max(mx, p[i]);
  bool occupied = mx >= 8;
  float capPct = 32.0 / fsMm * 100.0;
  if (occupied && mx > capPct) {
    if (!highSince) highSince = millis();
    else if (!turnSaid && millis() - highSince > (unsigned long)(turnMin * 60000UL)) { turnSaid = true; mp3.play(1); }
  } else { highSince = 0; turnSaid = false; }
  bool danger = occupied && min(l, r) < edgeCm;
  if (danger && !exitSaid) { exitSaid = true; buzz(5000); mp3.play(2); }
  if (!danger) exitSaid = false;
  led(2, mx < 33); led(3, mx >= 33 && mx < 66); led(4, mx >= 66);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(4, 5); Wire.setClock(400000); pcfBit(0, false);
  ads.begin(); ads.setGain(GAIN_ONE);
  pinMode(ECHO_L, INPUT); pinMode(ECHO_R, INPUT); pinMode(PIR_BED, INPUT); pinMode(PIR_ROOM, INPUT);
  dfSerial.begin(9600); if (mp3.begin(dfSerial)) mp3.volume(20);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  config.api_key = API_KEY; config.database_url = DB_URL;
  config.token_status_callback = tokenStatusCallback;
  fbdo.setBSSLBufferSize(2048, 1024);
  Firebase.signUp(&config, &auth, "", "");          // anonymous sign-in (enable in Firebase Auth)
  Firebase.begin(&config, &auth); Firebase.reconnectWiFi(true);
}

void loop() {
  if (buzOff && millis() > buzOff) { led(5, false); buzOff = 0; }
  if (!Firebase.ready()) return;

  if (millis() - tPush > 2000) {
    tPush = millis();
    int p[5];
    for (int i = 0; i < 4; i++) p[i] = pct(ads.readADC_SingleEnded(i));
    p[4] = constrain((int)(analogRead(A0) / 1023.0 * 100), 0, 100);
    int l = readCm(0, ECHO_L), r = readCm(1, ECHO_R);
    safetyLogic(p, l, r);
    FirebaseJson j;
    j.set("p/shL", p[0]); j.set("p/shR", p[1]); j.set("p/sacrum", p[2]); j.set("p/heelL", p[3]); j.set("p/heelR", p[4]);
    j.set("edge/l", l); j.set("edge/r", r);
    j.set("pir/bed", (bool)digitalRead(PIR_BED)); j.set("pir/room", (bool)digitalRead(PIR_ROOM));
    j.set("ts/.sv", "timestamp");                    // server time, used by the app's trend charts
    if (millis() - tHist > 30000 || tHist == 0) {    // history ring buffer: 240 points = last 2 hours
      static int slot = 0; tHist = millis();
      Firebase.RTDB.setJSON(&fbdo, ROOT + "/history/" + String(slot++ % 240), &j);
    }
    j.set("rssi", WiFi.RSSI()); j.set("up", (int)(millis() / 1000));
    Firebase.RTDB.setJSON(&fbdo, ROOT + "/latest", &j);
  }
  if (millis() - tCmd > 1500) {                      // commands from the web app
    tCmd = millis();
    if (Firebase.RTDB.getJSON(&fbdo, ROOT + "/cmd")) {
      FirebaseJson& j = fbdo.jsonObject(); FirebaseJsonData id, a, v;
      j.get(id, "id"); j.get(a, "action"); j.get(v, "value");
      if (id.success && id.intValue != lastCmd) { lastCmd = id.intValue; act(a.stringValue, v.intValue); }
    }
  }
  if (millis() - tSet > 30000) {                     // thresholds saved from the web app
    tSet = millis();
    if (Firebase.RTDB.getJSON(&fbdo, ROOT + "/settings")) {
      FirebaseJson& j = fbdo.jsonObject(); FirebaseJsonData d;
      if (j.get(d, "edgeCm") && d.success) edgeCm = d.floatValue;
      if (j.get(d, "turnMin") && d.success) turnMin = d.floatValue;
      if (j.get(d, "fsMm") && d.success) fsMm = d.floatValue;
    }
  }
}
