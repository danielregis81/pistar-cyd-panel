/*
 * pistar-cyd-panel — painel "quem está falando" do Pi-Star numa CYD 2.8" (ESP32-2432S028R).
 *
 * Lê http://<PISTAR_HOST>/cyd/status.json (gerado pelo coletor que roda no Pi-Star)
 * e as bandeiras em /cyd/flags/<cc>.565 (RGB565 60x40).
 *
 * Configuração: copie config.example.h para config.h e preencha.
 * Bibliotecas: TFT_eSPI (com o User_Setup da CYD), ArduinoJson 7.
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#if __has_include("config.h")
#include "config.h"
#else
#error "Crie o config.h: copie config.example.h para config.h e preencha WiFi e PISTAR_HOST."
#endif
#ifndef BRIGHTNESS
#define BRIGHTNESS 200
#endif

// ---------- cores (tema escuro) ----------
#define BG      0x0861
#define CARD    0x18E3
#define LINE    0x2945
#define TXT     0xDEDB
#define DIM     0x8410
#define GREEN   0x3606
#define YELLOW  0xFEA0
#define RED     0xF9A6
#define BLUE    0x4D9F
#define ORANGE  0xFC80

// ---------- textos da tela ----------
#define S_ONAIR    "NO AR"
#define S_LAST     "ultimo"
#define S_RECENT   "Ultimos"
#define S_TODAY    "hoje: %d estacoes / %d paises"
#define S_NOW      "agora"
#define S_MINAGO   "%ld min atras"
#define S_AT       "as %s"
#define S_LOSS     "perda"
#define S_WAIT     "Aguardando Pi-Star..."
#define S_NORESP   "Pi-Star sem resposta"
#define S_NODATA   "Nenhuma estacao ouvida ainda"
#define S_RFLOCAL  "RF (local)"

const unsigned long POLL_IDLE_MS   = 2000;   // consulta normal
const unsigned long POLL_ACTIVE_MS = 1000;   // enquanto alguém fala
const unsigned long STALE_MS       = 15000;  // sem resposta por mais que isso -> aviso

TFT_eSPI tft = TFT_eSPI();
JsonDocument doc;
bool haveData = false, active = false, wifiOk = false;
unsigned long lastPoll = 0, lastOk = 0;
String sig = "", hdrClock = "";

// ---------- bandeiras: cache em RAM (8 x 4,8 KB) ----------
#define FLAG_W 60
#define FLAG_H 40
#define FLAG_CACHE 8
uint16_t flagBuf[FLAG_CACHE][FLAG_W * FLAG_H];
char flagCC[FLAG_CACHE][3] = {{0}};
int flagNext = 0;

uint16_t* getFlag(const char* cc) {
  if (!cc || strlen(cc) != 2) return nullptr;
  for (int i = 0; i < FLAG_CACHE; i++) if (!strcmp(flagCC[i], cc)) return flagBuf[i];
  if (WiFi.status() != WL_CONNECTED) return nullptr;
  HTTPClient http; http.setConnectTimeout(2000); http.setTimeout(3000);
  http.begin(String("http://") + PISTAR_HOST + "/cyd/flags/" + cc + ".565");
  uint16_t* out = nullptr;
  if (http.GET() == 200 && http.getSize() == FLAG_W * FLAG_H * 2) {
    int slot = flagNext; flagNext = (flagNext + 1) % FLAG_CACHE;
    size_t got = http.getStreamPtr()->readBytes((uint8_t*)flagBuf[slot], FLAG_W * FLAG_H * 2);
    if (got == FLAG_W * FLAG_H * 2) { strncpy(flagCC[slot], cc, 2); flagCC[slot][2] = 0; out = flagBuf[slot]; }
    else flagCC[slot][0] = 0;
  }
  http.end();
  return out;
}

// ---------- utilidades ----------
String hhmm(long ep) {
  time_t t = ep; struct tm ti; localtime_r(&t, &ti);
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); return String(b);
}
String agoStr(long ep) {
  long d = (long)time(nullptr) - ep; char b[24];
  if (ep <= 0 || d < 0) return "";
  if (d < 60) return S_NOW;
  if (d < 3600) { snprintf(b, sizeof(b), S_MINAGO, d / 60); return String(b); }
  snprintf(b, sizeof(b), S_AT, hhmm(ep).c_str()); return String(b);
}
String mmss(long s) { if (s < 0) s = 0; char b[8]; snprintf(b, sizeof(b), "%02ld:%02ld", s / 60, s % 60); return String(b); }
String clockStr() {
  struct tm ti; if (!getLocalTime(&ti, 20)) return "--:--";
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); return String(b);
}
void card(int x, int y, int w, int h) { tft.fillRoundRect(x, y, w, h, 5, CARD); tft.drawRoundRect(x, y, w, h, 5, LINE); }
// corta o texto até caber na largura
String fit(String s, int font, int maxw) { while (s.length() > 1 && tft.textWidth(s, font) > maxw) s.remove(s.length() - 1); return s; }

// ---------- cabeçalho: indicativo do hotspot + relógio ----------
void header(bool force) {
  String c = clockStr();
  if (!force && c == hdrClock) return;
  hdrClock = c;
  tft.fillRect(0, 0, 320, 26, CARD);
  tft.drawFastHLine(0, 26, 320, LINE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(ORANGE, CARD);
  tft.drawString(haveData ? (const char*)(doc["station"] | "Pi-Star") : "Pi-Star", 8, 13, 2);
  tft.setTextColor(DIM, CARD); tft.drawString("DMR", 100, 13, 2);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(wifiOk ? GREEN : RED, CARD);
  tft.drawString(c, 314, 13, 4);
}

void message(const char* m) {
  tft.fillRect(0, 27, 320, 213, BG);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(DIM, BG); tft.drawString(m, 160, 130, 2);
  sig = "";
}

// ---------- linha que muda a cada segundo (cronômetro / "há X min") ----------
void drawDynamic() {
  JsonObject c = doc["cur"];
  tft.fillRect(12, 142, 296, 22, CARD);
  long st = c["start"] | 0L;
  tft.setTextDatum(ML_DATUM);
  if (active) {
    tft.setTextColor(RED, CARD); tft.drawString(mmss((long)time(nullptr) - st), 14, 153, 4);
  } else {
    tft.setTextColor(TXT, CARD); tft.drawString(String((const char*)(c["dur"] | "")) + " s", 14, 153, 2);
    tft.setTextColor(DIM, CARD); tft.drawString(agoStr(st), 80, 153, 2);
  }
  tft.setTextDatum(MR_DATUM); tft.setTextColor(DIM, CARD);
  String q = String("BER ") + (const char*)(c["ber"] | "-");
  const char* loss = c["loss"] | "";
  if (strlen(loss)) q += String("  " S_LOSS " ") + loss;
  tft.drawString(q, 306, 153, 2);
}

// ---------- tela principal ----------
void drawPanel() {
  JsonObject c = doc["cur"];
  if (c.isNull()) { message(S_NODATA); return; }
  String s = String((const char*)(c["call"] | "")) + (long)(c["start"] | 0L) + (active ? "A" : "-")
           + (const char*)(c["dur"] | "") + doc["today"].as<int>();
  if (s != sig) {
    sig = s;
    tft.fillRect(0, 27, 320, 213, BG);
    // ---- chamador ----
    card(6, 32, 308, 136);
    uint16_t* fl = getFlag(c["cc"] | "");
    tft.drawRect(13, 41, FLAG_W + 2, FLAG_H + 2, LINE);
    if (fl) { tft.setSwapBytes(true); tft.pushImage(14, 42, FLAG_W, FLAG_H, fl); tft.setSwapBytes(false); }
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(active ? YELLOW : TXT, CARD);
    tft.drawString((const char*)(c["call"] | ""), 86, 40, 4);
    tft.setTextColor(TXT, CARD);
    tft.drawString(fit((const char*)(c["name"] | ""), 4, 220), 86, 66, 4);
    tft.setTextColor(DIM, CARD);
    tft.drawString(fit((const char*)(c["country"] | ""), 2, 220), 86, 93, 2);
    tft.setTextDatum(MC_DATUM);
    if (active) { tft.fillRoundRect(236, 40, 70, 20, 4, RED); tft.setTextColor(TXT, RED); tft.drawString(S_ONAIR, 271, 50, 2); }
    else        { tft.drawRoundRect(236, 40, 70, 20, 4, LINE); tft.setTextColor(DIM, CARD); tft.drawString(S_LAST, 271, 50, 2); }
    // ---- TG / slot / origem ----
    tft.drawFastHLine(12, 110, 296, LINE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ORANGE, CARD); tft.drawString((const char*)(c["tg"] | ""), 14, 125, 4);
    tft.setTextDatum(MR_DATUM); tft.setTextColor(TXT, CARD);
    tft.drawString(String((const char*)(c["mode"] | "")) + " " + (const char*)(c["slot"] | ""), 306, 118, 2);
    String src = (const char*)(c["src"] | "");
    tft.setTextColor(src == "RF" ? GREEN : BLUE, CARD);
    tft.drawString(src == "RF" ? S_RFLOCAL : src, 306, 134, 2);
    // ---- últimos ouvidos ----
    card(6, 172, 308, 64);
    tft.setTextDatum(ML_DATUM); tft.setTextColor(DIM, CARD); tft.drawString(S_RECENT, 14, 182, 1);
    char b[48]; snprintf(b, sizeof(b), S_TODAY, (int)(doc["today"] | 0), (int)(doc["countries"] | 0));
    tft.setTextDatum(MR_DATUM); tft.drawString(b, 306, 182, 1);
    int i = 0;
    for (JsonObject l : doc["last"].as<JsonArray>()) {
      if (i >= 6) break;
      int x = 14 + (i % 3) * 100, y = 200 + (i / 3) * 18;
      tft.setTextDatum(ML_DATUM); tft.setTextColor(TXT, CARD);
      tft.drawString((const char*)(l["call"] | ""), x, y, 2);
      tft.setTextColor(DIM, CARD); tft.drawString(hhmm(l["start"] | 0L), x + 58, y, 1);
      i++;
    }
  }
  drawDynamic();
}

// ---------- rede ----------
bool fetch() {
  if (WiFi.status() != WL_CONNECTED) { wifiOk = false; WiFi.reconnect(); return false; }
  wifiOk = true;
  HTTPClient http; http.setConnectTimeout(2000); http.setTimeout(2500);
  http.begin(String("http://") + PISTAR_HOST + "/cyd/status.json");
  bool ok = false;
  if (http.GET() == 200 && !deserializeJson(doc, http.getString()) && (doc["ok"] | false)) {
    haveData = true; active = doc["active"] | false; ok = true;
  }
  http.end();
  return ok;
}

void setup() {
  Serial.begin(115200);
  tft.init(); tft.setRotation(1); tft.fillScreen(BG);
#ifdef TFT_BL
  ledcAttach(TFT_BL, 5000, 8); ledcWrite(TFT_BL, BRIGHTNESS);
#endif
  tft.setTextDatum(MC_DATUM); tft.setTextColor(TXT, BG);
  tft.drawString("Pi-Star CYD Panel", 160, 100, 4);
  tft.drawString(String("conectando em ") + WIFI_SSID + "...", 160, 140, 2);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(300);
  wifiOk = (WiFi.status() == WL_CONNECTED);
  Serial.println(wifiOk ? WiFi.localIP().toString() : "WIFI FALHOU");
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.google.com");
  tft.fillScreen(BG);
  if (fetch()) lastOk = millis();
  header(true);
  if (haveData) drawPanel(); else message(S_WAIT);
  lastPoll = millis();
}

void loop() {
  unsigned long now = millis();
  if (now - lastPoll >= (active ? POLL_ACTIVE_MS : POLL_IDLE_MS)) {
    lastPoll = now;
    bool ok = fetch();
    if (ok) {
      bool first = (lastOk == 0) || sig == "";
      lastOk = now;
      if (first) header(true);
      drawPanel();
    } else if (now - lastOk > STALE_MS) {
      message(haveData ? S_NORESP : S_WAIT);
      active = false;
    }
  }
  header(false);
  delay(20);
}
