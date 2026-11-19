/*
 * pistar-cyd-panel — painel "quem está falando" do Pi-Star numa CYD 2.8" (ESP32-2432S028R).
 *
 * Configuração SEM compilar: no primeiro boot a CYD cria a rede WiFi "PiStar-CYD-XXXX".
 * Conecte nela pelo celular e abra http://192.168.4.1 para escolher a rede da casa,
 * a senha, o endereço do Pi-Star e o fuso. Tudo fica gravado na memória da placa.
 * Para reconfigurar: segure o botão BOOT por 3 segundos (ou ligue segurando BOOT).
 *
 * Lê http://<pi-star>/cyd/status.json e /cyd/flags/<cc>.565 (gerados pelo coletor no Pi-Star).
 * Bibliotecas: TFT_eSPI (com o User_Setup da CYD), ArduinoJson 7.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>

#define BOOT_PIN   0          // botão BOOT da CYD
#define BRIGHTNESS 200
#define PORTAL_TIMEOUT_MS (5UL * 60 * 1000)   // sem configurar em 5 min -> reinicia e tenta de novo

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

const unsigned long POLL_IDLE_MS   = 2000;   // consulta normal
const unsigned long POLL_ACTIVE_MS = 1000;   // enquanto alguém fala
const unsigned long STALE_MS       = 15000;  // sem resposta por mais que isso -> aviso

TFT_eSPI tft = TFT_eSPI();
JsonDocument doc;
Preferences prefs;
WebServer server(80);
DNSServer dns;

String cfgSsid, cfgPass, cfgHost, cfgTz;
bool haveData = false, active = false, wifiOk = false, useHttps = false;
unsigned long lastPoll = 0, lastOk = 0, bootDown = 0;
int fails = 0;
String sig = "", hdrClock = "";
IPAddress hostIP; bool hostResolved = false;

// ---------- configuração gravada na placa ----------
void loadCfg() {
  prefs.begin("pcp", true);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgHost = prefs.getString("host", "");
  cfgTz   = prefs.getString("tz", "<-03>3");
  prefs.end();
}
void saveCfg() {
  prefs.begin("pcp", false);
  prefs.putString("ssid", cfgSsid); prefs.putString("pass", cfgPass);
  prefs.putString("host", cfgHost); prefs.putString("tz", cfgTz);
  prefs.end();
}

// ---------- utilidades de tela ----------
void card(int x, int y, int w, int h) { tft.fillRoundRect(x, y, w, h, 5, CARD); tft.drawRoundRect(x, y, w, h, 5, LINE); }
String fit(String s, int font, int maxw) { while (s.length() > 1 && tft.textWidth(s, font) > maxw) s.remove(s.length() - 1); return s; }
String hhmm(long ep) {
  time_t t = ep; struct tm ti; localtime_r(&t, &ti);
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); return String(b);
}
String agoStr(long ep) {
  long d = (long)time(nullptr) - ep; char b[24];
  if (ep <= 0 || d < 0) return "";
  if (d < 60) return "agora";
  if (d < 3600) { snprintf(b, sizeof(b), "%ld min atras", d / 60); return String(b); }
  snprintf(b, sizeof(b), "as %s", hhmm(ep).c_str()); return String(b);
}
String mmss(long s) { if (s < 0) s = 0; char b[8]; snprintf(b, sizeof(b), "%02ld:%02ld", s / 60, s % 60); return String(b); }
String clockStr() {
  struct tm ti; if (!getLocalTime(&ti, 20)) return "--:--";
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", ti.tm_hour, ti.tm_min); return String(b);
}

// =====================================================================
//  PORTAL DE CONFIGURAÇÃO (rede própria + página web)
// =====================================================================
String apName() {
  uint64_t mac = ESP.getEfuseMac();
  char b[24]; snprintf(b, sizeof(b), "PiStar-CYD-%04X", (unsigned)(mac >> 32) & 0xFFFF); return String(b);
}
String esc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
  }
  return o;
}

const char* TZS[][2] = {
  {"<-03>3", "Brasilia (UTC-3)"}, {"<-04>4", "Amazonas (UTC-4)"}, {"<-05>5", "Acre (UTC-5)"},
  {"<-02>2", "Fernando de Noronha (UTC-2)"}, {"WET0WEST,M3.5.0/1,M10.5.0", "Portugal"},
  {"EST5EDT,M3.2.0,M11.1.0", "EUA leste"}, {"PST8PDT,M3.2.0,M11.1.0", "EUA oeste"},
  {"CET-1CEST,M3.5.0,M10.5.0/3", "Europa central"}, {"GMT0BST,M3.5.0/1,M10.5.0", "Reino Unido"},
  {"UTC0", "UTC"},
};

void handleRoot() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i); if (!s.length() || opts.indexOf("\"" + esc(s) + "\"") >= 0) continue;
    opts += "<option value=\"" + esc(s) + "\">" + esc(s) + " (" + WiFi.RSSI(i) + " dBm)</option>";
  }
  String tz;
  for (auto& z : TZS) tz += String("<option value=\"") + esc(z[0]) + "\"" + (cfgTz == z[0] ? " selected" : "") + ">" + z[1] + "</option>";
  String h =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Pi-Star CYD</title><style>body{font:16px system-ui;background:#0b0f14;color:#e6edf3;margin:0;padding:18px}"
    "h1{font-size:20px}label{display:block;margin:14px 0 4px;color:#8b9bb0}input,select{width:100%;box-sizing:border-box;"
    "padding:10px;font-size:16px;border-radius:8px;border:1px solid #223042;background:#141b24;color:#e6edf3}"
    "button{margin-top:20px;width:100%;padding:12px;font-size:17px;border:0;border-radius:8px;background:#fc8019;color:#000;font-weight:700}"
    "small{color:#8b9bb0}</style></head><body><h1>Pi-Star CYD Panel</h1>"
    "<form method=post action=/save>"
    "<label>Rede WiFi da casa</label><input name=ssid list=nets value=\"" + esc(cfgSsid) + "\" required><datalist id=nets>" + opts + "</datalist>"
    "<label>Senha do WiFi</label><input name=pass type=password placeholder='" + String(cfgPass.length() ? "(manter a atual)" : "") + "'>"
    "<label>Endereco do Pi-Star</label><input name=host value=\"" + esc(cfgHost) + "\" placeholder='192.168.0.100' required>"
    "<small>Digite o <b>IP</b> do Pi-Star (ex.: 192.168.0.100), o nome (pi-star.local), ou um <b>dominio externo</b> (ex.: pistar.exemplo.com.br para acesso via internet).</small>"
    "<label>Fuso horario</label><select name=tz>" + tz + "</select>"
    "<button>Salvar e reiniciar</button></form></body></html>";
  server.send(200, "text/html", h);
}

void handleSave() {
  String s = server.arg("ssid"), p = server.arg("pass"), h = server.arg("host"), z = server.arg("tz");
  s.trim(); h.trim();
  if (!s.length() || !h.length()) { server.send(400, "text/plain", "Rede e endereco sao obrigatorios"); return; }
  if (h.startsWith("https://")) h = h.substring(8);
  else if (h.startsWith("http://")) h = h.substring(7);
  while (h.endsWith("/")) h.remove(h.length() - 1);
  cfgSsid = s; if (p.length()) cfgPass = p; cfgHost = h; if (z.length()) cfgTz = z;
  saveCfg();
  server.send(200, "text/html", "<meta charset=utf-8><body style='font:18px system-ui;background:#0b0f14;color:#e6edf3;padding:20px'>"
              "Salvo. A tela vai reiniciar e conectar em <b>" + esc(cfgSsid) + "</b>.</body>");
  delay(1500);
  ESP.restart();
}

void portal(const char* reason) {
  String ap = apName();
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap.c_str());
  dns.start(53, "*", WiFi.softAPIP());                 // abre a página sozinho em muitos celulares
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); });
  server.begin();

  tft.fillScreen(BG);
  card(6, 6, 308, 228);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(ORANGE, CARD); tft.drawString("Configurar", 160, 16, 4);
  tft.setTextColor(DIM, CARD);    tft.drawString(reason, 160, 46, 2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TXT, CARD);
  tft.drawString("1. No celular, conecte na rede WiFi:", 16, 78, 2);
  tft.setTextColor(YELLOW, CARD); tft.drawString(ap, 30, 98, 4);
  tft.setTextColor(TXT, CARD);
  tft.drawString("2. Abra no navegador:", 16, 134, 2);
  tft.setTextColor(YELLOW, CARD); tft.drawString("http://192.168.4.1", 30, 154, 4);
  tft.setTextColor(TXT, CARD);
  tft.drawString("3. Escolha a rede, a senha e o Pi-Star.", 16, 192, 2);

  unsigned long t0 = millis();
  while (millis() - t0 < PORTAL_TIMEOUT_MS) { dns.processNextRequest(); server.handleClient(); delay(5); }
  ESP.restart();                                        // ninguém configurou: tenta conectar de novo
}

// =====================================================================
//  PI-STAR: endereço (IP, nome .local, ou domínio externo) e leitura
// =====================================================================
WiFiClientSecure secureClient;

String baseUrl() {
  // Domínio externo (HTTPS): contém ponto mas não é .local
  if (cfgHost.indexOf('.') >= 0 && !cfgHost.endsWith(".local")) {
    useHttps = true;
    return "https://" + cfgHost;
  }
  useHttps = false;
  // Nome mDNS (.local): resolve via MDNS
  if (cfgHost.endsWith(".local")) {
    if (!hostResolved) {
      IPAddress ip = MDNS.queryHost(cfgHost.substring(0, cfgHost.length() - 6), 2000);
      if (ip == IPAddress(0, 0, 0, 0)) return "";
      hostIP = ip; hostResolved = true;
    }
    return "http://" + hostIP.toString();
  }
  // IP direto
  return "http://" + cfgHost;
}

#define FLAG_W 60
#define FLAG_H 40
#define FLAG_CACHE 8
uint16_t flagBuf[FLAG_CACHE][FLAG_W * FLAG_H];
char flagCC[FLAG_CACHE][3] = {{0}};
int flagNext = 0;

uint16_t* getFlag(const char* cc) {
  if (!cc || strlen(cc) != 2) return nullptr;
  for (int i = 0; i < FLAG_CACHE; i++) if (!strcmp(flagCC[i], cc)) return flagBuf[i];
  String base = baseUrl(); if (!base.length() || WiFi.status() != WL_CONNECTED) return nullptr;
  HTTPClient http;
  http.setConnectTimeout(useHttps ? 5000 : 2000);
  http.setTimeout(useHttps ? 8000 : 3000);
  String url = base + "/cyd/flags/" + cc + ".565";
  if (useHttps) http.begin(secureClient, url);
  else http.begin(url);
  uint16_t* out = nullptr;
  int code = http.GET();
  int size = http.getSize();
  if (code == 200 && size == FLAG_W * FLAG_H * 2) {
    int slot = flagNext; flagNext = (flagNext + 1) % FLAG_CACHE;
    uint8_t* buf = (uint8_t*)flagBuf[slot];
    size_t total = 0, need = FLAG_W * FLAG_H * 2;
    WiFiClient* stream = http.getStreamPtr();
    unsigned long t0 = millis();
    while (total < need && millis() - t0 < 10000) {
      if (stream->available()) {
        size_t got = stream->readBytes(buf + total, need - total);
        total += got;
      } else {
        delay(10);
      }
    }
    if (total == need) { strncpy(flagCC[slot], cc, 2); flagCC[slot][2] = 0; out = flagBuf[slot]; }
    else flagCC[slot][0] = 0;
  }
  http.end();
  return out;
}

bool fetch() {
  if (WiFi.status() != WL_CONNECTED) { wifiOk = false; WiFi.reconnect(); return false; }
  wifiOk = true;
  String base = baseUrl();
  bool ok = false;
  if (base.length()) {
    HTTPClient http;
    http.setConnectTimeout(useHttps ? 5000 : 2000);
    http.setTimeout(useHttps ? 6000 : 2500);
    if (useHttps) http.begin(secureClient, base + "/cyd/status.json");
    else http.begin(base + "/cyd/status.json");
    if (http.GET() == 200 && !deserializeJson(doc, http.getString()) && (doc["ok"] | false)) {
      haveData = true; active = doc["active"] | false; ok = true;
    }
    http.end();
  }
  if (ok) fails = 0;
  else if (++fails >= 3) hostResolved = false;          // o Pi-Star pode ter mudado de IP: procura de novo
  return ok;
}

// =====================================================================
//  TELA PRINCIPAL
// =====================================================================
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
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TXT, BG); tft.drawString(m, 160, 100, 2);
  tft.setTextColor(DIM, BG);
  tft.drawString("Pi-Star: " + cfgHost, 160, 130, 2);
  tft.drawString("Esta tela: " + WiFi.localIP().toString(), 160, 150, 2);
  tft.drawString("Segure BOOT 3 s para configurar", 160, 190, 2);
  sig = "";
}

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
  if (strlen(loss)) q += String("  perda ") + loss;
  tft.drawString(q, 306, 153, 2);
}

void drawPanel() {
  JsonObject c = doc["cur"];
  if (c.isNull()) { message("Nenhuma estacao ouvida ainda"); return; }
  String s = String((const char*)(c["call"] | "")) + (long)(c["start"] | 0L) + (active ? "A" : "-")
           + (const char*)(c["dur"] | "") + doc["today"].as<int>();
  if (s != sig) {
    sig = s;
    tft.fillRect(0, 27, 320, 213, BG);
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
    if (active) { tft.fillRoundRect(236, 40, 70, 20, 4, RED); tft.setTextColor(TXT, RED); tft.drawString("NO AR", 271, 50, 2); }
    else        { tft.drawRoundRect(236, 40, 70, 20, 4, LINE); tft.setTextColor(DIM, CARD); tft.drawString("ultimo", 271, 50, 2); }
    tft.drawFastHLine(12, 110, 296, LINE);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ORANGE, CARD); tft.drawString((const char*)(c["tg"] | ""), 14, 125, 4);
    tft.setTextDatum(MR_DATUM); tft.setTextColor(TXT, CARD);
    tft.drawString(String((const char*)(c["mode"] | "")) + " " + (const char*)(c["slot"] | ""), 306, 118, 2);
    String src = (const char*)(c["src"] | "");
    tft.setTextColor(src == "RF" ? GREEN : BLUE, CARD);
    tft.drawString(src == "RF" ? "RF (local)" : src, 306, 134, 2);
    card(6, 172, 308, 64);
    tft.setTextDatum(ML_DATUM); tft.setTextColor(DIM, CARD); tft.drawString("Ultimos", 14, 182, 1);
    char b[48]; snprintf(b, sizeof(b), "hoje: %d estacoes / %d paises", (int)(doc["today"] | 0), (int)(doc["countries"] | 0));
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

// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  tft.init(); tft.setRotation(1); tft.fillScreen(BG);
#ifdef TFT_BL
  ledcAttach(TFT_BL, 5000, 8); ledcWrite(TFT_BL, BRIGHTNESS);
#endif
  loadCfg();
  if (!cfgSsid.length() || !cfgHost.length()) portal("Primeiro uso");
  if (digitalRead(BOOT_PIN) == LOW) portal("Botao BOOT pressionado");

  tft.setTextDatum(MC_DATUM); tft.setTextColor(TXT, BG);
  tft.drawString("Pi-Star CYD Panel", 160, 90, 4);
  tft.setTextColor(DIM, BG);
  tft.drawString("conectando em " + cfgSsid + "...", 160, 130, 2);
  tft.drawString("Segure BOOT 3 s para configurar", 160, 190, 2);
  WiFi.mode(WIFI_STA); WiFi.setHostname("pistar-cyd");
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    if (digitalRead(BOOT_PIN) == LOW) portal("Botao BOOT pressionado");
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) portal("Nao conectou no WiFi");
  wifiOk = true;
  Serial.println(WiFi.localIP());
  MDNS.begin("pistar-cyd");
  secureClient.setInsecure();  // aceita qualquer certificado (túneis, self-signed, etc)
  configTzTime(cfgTz.c_str(), "pool.ntp.org", "time.google.com");

  tft.fillScreen(BG);
  if (fetch()) lastOk = millis();
  header(true);
  if (haveData) drawPanel(); else message("Procurando o Pi-Star...");
  lastPoll = millis();
}

void loop() {
  unsigned long now = millis();
  // segurar BOOT por 3 s abre a configuração
  if (digitalRead(BOOT_PIN) == LOW) { if (!bootDown) bootDown = now; else if (now - bootDown > 3000) portal("Reconfigurar"); }
  else bootDown = 0;

  if (now - lastPoll >= (active ? POLL_ACTIVE_MS : POLL_IDLE_MS)) {
    lastPoll = now;
    if (fetch()) {
      bool first = (lastOk == 0) || sig == "";
      lastOk = now;
      if (first) header(true);
      drawPanel();
    } else if (now - lastOk > STALE_MS && sig != "") {
      message(haveData ? "Pi-Star sem resposta" : "Procurando o Pi-Star...");
      active = false;
    }
  }
  header(false);
  delay(20);
}
