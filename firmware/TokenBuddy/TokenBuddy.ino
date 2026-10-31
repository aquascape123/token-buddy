/*
  Token Buddy - ESP32 CYD 2.8" (ESP32-2432S028R, ILI9341 320x240)

  Una "faccia" arancione: i due occhi sono anelli che si riempiono con
  l'utilizzo (sinistro = sessione 5h, destro = settimana) con la percentuale
  al centro.

  Reagisce a Claude Code (eventi degli hook, via bridge):
    sta lavorando -> occhi che si muovono, LED blu
    ha finito     -> sorriso, LED verde lampeggia 3 volte
    ti aspetta    -> occhi che saltellano, LED giallo lampeggiante
  Avvisi: limite all'80% / 95% (LED rosso lampeggiante) e limite azzerato.
  Un tocco sullo schermo chiude l'avviso o il "ti aspetta".

  Dati letti dal bridge Python sul PC (bridge/claude_usage_bridge.py).

  Librerie: TFT_eSPI (configurato per il CYD), ArduinoJson v7, WiFiManager,
            XPT2046_Touchscreen
  Scheda: "ESP32 Dev Module"
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiUdp.h>

// ====== CONFIGURA QUI ======
// Il WiFi si configura dal telefono: se non trova una rete salvata il CYD
// crea la rete "TokenBuddy-Setup"; collegati e scegli la tua rete.
const char* AP_NAME = "TokenBuddy-Setup";
const char* HOSTNAME = "tokenbuddy";        // pagina dei temi: http://tokenbuddy.local
// il bridge si annuncia in rete (UDP 8766): l'indirizzo del PC non va scritto qui
const uint16_t BEACON_PORT = 8766;
const uint32_t POLL_MS = 500;            // il bridge e' in LAN: si puo' chiedere spesso
const uint32_t GOOD_ALERT_MS = 300000;   // "di nuovo disponibile" sparisce dopo 5 min
// ===========================

// LED RGB del CYD (attivo basso)
#define LED_R 4
#define LED_G 16
#define LED_B 17

// altoparlante: GPIO26 -> amplificatore del CYD -> connettore "SPEAK"
#define SPK_PIN 26
#define SPK_CH  4                     // canale LEDC per i toni

// modello di CYD: la configurazione dello schermo arriva da tools/build_firmware.py
// (-DTOKENBUDDY_CYD24 per il 2.4", altrimenti 2.8")
#ifdef TOKENBUDDY_CYD24
const char* BOARD_NAME = "CYD 2.4";
// touch XPT2046 sugli stessi pin dello schermo
#define T_IRQ  36
#define T_MOSI 13
#define T_MISO 12
#define T_CLK  14
#define T_CS   33
#define TOUCH_SPI_HOST HSPI
#else
const char* BOARD_NAME = "CYD 2.8";
// touch XPT2046 (bus SPI separato dal display)
#define T_IRQ  36
#define T_MOSI 32
#define T_MISO 39
#define T_CLK  25
#define T_CS   33
#define TOUCH_SPI_HOST VSPI
#endif

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
// colori del tema attivo (impostati da applyColors, scelti dalla pagina web)
uint16_t C_BG, C_TRACK, C_EYE, C_NUM, C_DARK, C_SOFT, C_OK, C_MID, C_HOT;
#define C_WHITE  RGB(255, 255, 255)
#define C_GOOD   RGB(20, 140, 40)     // fascia "di nuovo disponibile"


struct Theme {
  const char* name;
  uint32_t bg, text, track, eye, num, ok, mid, hot;   // 0xRRGGBB
};
// temi pronti: coppie sfondo/scritte provate per essere leggibili
Theme THEMES[] = {
  {"Orange",   0xFF8C00, 0x461C08, 0x963C0A, 0xFFFFFF, 0x461C08, 0x1EAA3C, 0xFFAF14, 0xDC0000},
  {"Night",    0x121830, 0xF5E8CD, 0x3C466E, 0x202A50, 0xF5E8CD, 0x3CC86E, 0xFFB43C, 0xFF5050},
  {"Terminal", 0x000000, 0x3CFF5A, 0x004614, 0x001908, 0x3CFF5A, 0x3CFF5A, 0xFFC800, 0xFF3C3C},
  {"Game Boy", 0x9BBC0F, 0x0F380F, 0x8BAC0F, 0xC8DC82, 0x0F380F, 0x306230, 0xC88C00, 0xB41E1E},
  {"Mint",     0xAAE6C8, 0x14285A, 0x6EB4A0, 0xFFFFFF, 0x14285A, 0x149650, 0xF09614, 0xD21E28},
  {"Coral",    0xFF7864, 0x5A0A1E, 0xC85046, 0xFFFFFF, 0x5A0A1E, 0x1E9646, 0xFFC828, 0x96001E},
  {"Custom",   0xFF8C00, 0x461C08, 0x963C0A, 0xFFFFFF, 0x461C08, 0x1EAA3C, 0xFFAF14, 0xDC0000},
};
const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
const int CUSTOM = THEME_COUNT - 1;
int themeIdx = 0;

// geometria
#define EYE_R    64                   // raggio esterno anello
#define EYE_IR   51                   // raggio interno anello
#define EYE_S    148                  // lato sprite (anello + spazio per muoversi)
#define EYE_C    74                   // centro dello sprite
#define MOVE_X   8                    // escursione occhi "sta lavorando"
#define MOVE_Y   4                    // escursione occhi "ti aspetta"
#define EYE_Y    100                  // centro verticale occhi
#define EYE_LX   82
#define EYE_RX   238
// zona della bocca: gli sprite degli occhi non devono coprirla
#define MOUTH_X0 140
#define MOUTH_X1 180
#define MOUTH_Y0 150
#define MOUTH_Y1 178
#define ROW_W    150                  // sprite riga del tempo al reset
#define ROW_H    20
#define LABEL_Y  188
#define RESET_Y  206
#define FOOT_Y   218                  // fascia in basso, alta 22

TFT_eSPI tft;
TFT_eSprite eye(&tft);
TFT_eSprite row(&tft);
SPIClass touchSPI(TOUCH_SPI_HOST);
XPT2046_Touchscreen ts(T_CS, T_IRQ);

enum CC { CC_IDLE, CC_WORKING, CC_DONE, CC_WAITING };
enum Mood { M_SMILE, M_FLAT, M_SAD, M_OH, M_BIG, M_WOW };

struct Usage {
  bool ok = false;
  float sPct = 0, wPct = 0;
  uint32_t sReset = 0, wReset = 0;
  String plan, err;
  CC cc = CC_IDLE;
  uint32_t ccSeq = 0;
  int ccN = 0;
  bool typing = false;      // stai scrivendo nella finestra di Claude
  uint32_t keys = 0;        // contatore pressioni dal bridge (giu' + su)
} usage;

struct Alert {
  bool active = false;
  bool good = false;        // verde (limite azzerato) o rosso (soglia)
  String text;
  uint32_t since = 0;
} alert;

uint32_t serverNow = 0, serverNowAt = 0;
uint32_t lastPoll = 0, lastTick = 0, lastFrame = 0;
uint32_t dismissedSeq = 0;           // "fatto"/"ti aspetta" chiusi col tocco
uint32_t doneSeq = 0, doneAt = 0;    // per i 3 lampeggi verdi
bool haveData = false;
int sLevel = 0, wLevel = 0;          // soglia gia' annunciata (0/80/95)
float sPrev = 0, wPrev = 0;
int eyeDx = 0, eyeDy = 0;
int lastMood = -1;
String lastFooter;
uint16_t lastFooterBg = 0;

// modalita' "dorme": quando Claude e' inattivo, faccia pixel addormentata
#define PEEK_MS  15000                // un tocco mostra le percentuali per 15 s
#define SL_X     10                   // sprite della faccia addormentata
#define SL_Y     12
#define SL_W     300
#define SL_H     200
#define BLK      10                   // lato del "pixel" grande
TFT_eSprite face(&tft);
bool sleeping = false;                // true = schermata pixel (dorme o tastiera)
enum Scene { SC_RINGS, SC_SLEEP, SC_TYPING };
Scene scene = SC_RINGS;
int pendingPresses = 0;
String toast;                         // messaggio breve nella riga in basso
uint32_t toastUntil = 0;
WebServer server(80);
Preferences prefs;
WiFiUDP beaconUdp;
String bridgeUrl;                     // trovato dall'annuncio, ricordato tra un riavvio e l'altro
bool soundOn = true;                  // clic dei tasti, si spegne dalla pagina web
uint32_t soundOffAt = 0;               // tasti veri ancora da mostrare sulla tastiera
uint32_t peekUntil = 0;
uint32_t lastSleepFrame = 0;

// carattere pixel: il font 1 di TFT_eSPI (5x7) ingrandito a blocchi
// size N -> cella 6N x 8N, lettera 5N x 7N
void pixelFont(TFT_eSPI& g, uint8_t size) {
  g.setTextFont(1);
  g.setTextSize(size);
}

// la dimensione piu' grande (<= size) con cui il testo sta in maxW
void fitFont(TFT_eSPI& g, const String& s, int maxW, uint8_t size) {
  for (; size > 1; size--) {
    pixelFont(g, size);
    if (g.textWidth(s) <= maxW) return;
  }
  pixelFont(g, 1);
}

uint32_t nowEpoch() {
  if (!serverNow) return 0;
  return serverNow + (millis() - serverNowAt) / 1000;
}

uint16_t levelColor(float p) {
  if (p >= 80) return C_HOT;
  if (p >= 50) return C_MID;
  return C_OK;
}

// numero dentro l'occhio bianco: il giallo non si leggerebbe
uint16_t numColor(float p) {
  return C_NUM;
}

String fmtRemaining(uint32_t resetAt) {
  uint32_t now = nowEpoch();
  if (!resetAt || !now) return "--";
  if (resetAt <= now) return "now";
  uint32_t s = resetAt - now;
  unsigned d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
  char buf[24];
  if (d) snprintf(buf, sizeof(buf), "%ud %uh %um", d, h, m);
  else if (h) snprintf(buf, sizeof(buf), "%uh %02um", h, m);
  else snprintf(buf, sizeof(buf), "%um %02us", m, (unsigned)(s % 60));
  return String(buf);
}

// stato Claude Code "da mostrare": quelli chiusi col tocco non contano piu'
CC shownCC() {
  if ((usage.cc == CC_DONE || usage.cc == CC_WAITING) && usage.ccSeq == dismissedSeq)
    return CC_IDLE;
  return usage.cc;
}

// ---------------- occhi ----------------

// arco da ore 12 in senso orario; in TFT_eSPI l'angolo 0 e' a ore 6
void drawProgress(int x, int y, float pct, uint16_t col) {
  if (pct <= 0) return;
  if (pct >= 99.5) {
    eye.drawSmoothArc(x, y, EYE_R, EYE_IR, 0, 360, col, C_TRACK, false);
    return;
  }
  uint32_t end = 180 + (uint32_t)(pct * 3.6);
  if (end <= 360) {
    eye.drawSmoothArc(x, y, EYE_R, EYE_IR, 180, max(end, 181u), col, C_TRACK, true);
  } else {
    eye.drawSmoothArc(x, y, EYE_R, EYE_IR, 180, 360, col, C_TRACK, false);
    eye.drawSmoothArc(x, y, EYE_R, EYE_IR, 0, end - 360, col, C_TRACK, true);
  }
}

void drawEye(int cx, float pct) {
  const int x = EYE_C + eyeDx, y = EYE_C + eyeDy;
  eye.fillSprite(C_BG);
  // bordino scuro: stacca la barra arancione dallo sfondo arancione
  eye.drawSmoothArc(x, y, EYE_R + 2, EYE_R - 1, 0, 360, C_DARK, C_BG, false);
  eye.drawSmoothArc(x, y, EYE_R, EYE_IR, 0, 360, C_TRACK, C_DARK, false);
  if (usage.ok) drawProgress(x, y, pct, levelColor(pct));
  eye.fillSmoothCircle(x, y, EYE_IR - 3, C_EYE, C_TRACK);

  if (!usage.ok) {
    eye.setTextDatum(MC_DATUM);
    pixelFont(eye, 4);
    eye.setTextColor(C_SOFT);
    eye.drawString("--", x, y);
  } else {
    char num[6];
    int v = (int)round(pct);
    snprintf(num, sizeof(num), "%d", v);
    const uint8_t sn = v >= 100 ? 4 : 5, sp = 3;   // "100" non ci sta a size 5
    pixelFont(eye, sn);
    int wn = eye.textWidth(num) - sn;             // senza lo spazio dopo l'ultima cifra
    pixelFont(eye, sp);
    int wp = eye.textWidth("%") - sp;
    int x0 = x - (wn + 2 * sp + wp) / 2;
    const int bottom = y + 7 * 5 / 2;             // piede comune di numero e %
    eye.setTextDatum(TL_DATUM);
    eye.setTextColor(numColor(pct));
    pixelFont(eye, sn);
    eye.drawString(num, x0, bottom - 7 * sn);
    pixelFont(eye, sp);
    eye.drawString("%", x0 + wn + 2 * sp, bottom - 7 * sp);
  }

  // parte alta intera; in basso si salta la colonna della bocca
  const int sx0 = cx - EYE_C, sy0 = EYE_Y - EYE_C;
  const int split = MOUTH_Y0 - sy0;
  eye.pushSprite(sx0, sy0, 0, 0, EYE_S, split);
  if (cx < 160) {
    eye.pushSprite(sx0, sy0 + split, 0, split, MOUTH_X0 - sx0, EYE_S - split);
  } else {
    int skip = MOUTH_X1 - sx0;
    eye.pushSprite(MOUTH_X1, sy0 + split, skip, split, EYE_S - skip, EYE_S - split);
  }
}

void drawEyes() {
  drawEye(EYE_LX, usage.sPct);
  drawEye(EYE_RX, usage.wPct);
}

// ---------------- bocca ----------------

// bocche pixel ('X' = blocco pieno), stesso stile della faccia che dorme
#define MBLK 5                        // lato del blocco: 8 blocchi = 40 px
const char* const M_SMILE_PX[] = {"X......X", ".X....X.", "..XXXX.."};
const char* const M_BIG_PX[]   = {"XXXXXXXX", "X......X", ".X....X.", "..XXXX.."};
const char* const M_FLAT_PX[]  = {".XXXXXX."};
const char* const M_SAD_PX[]   = {"..XXXX..", ".X....X.", "X......X"};
const char* const M_OH_PX[]    = {".XX.", "X..X", ".XX."};
const char* const M_WOW_PX[]   = {".XXXX.", "X....X", "X....X", ".XXXX."};

void drawMouth(int mood) {
  if (mood == lastMood) return;
  lastMood = mood;
  const char* const* rows;
  int n;
  switch (mood) {
    case M_SMILE: rows = M_SMILE_PX; n = 3; break;
    case M_BIG:   rows = M_BIG_PX;   n = 4; break;
    case M_SAD:   rows = M_SAD_PX;   n = 3; break;
    case M_OH:    rows = M_OH_PX;    n = 3; break;
    case M_WOW:   rows = M_WOW_PX;   n = 4; break;
    default:      rows = M_FLAT_PX;  n = 1; break;
  }
  tft.fillRect(MOUTH_X0, MOUTH_Y0, MOUTH_X1 - MOUTH_X0, MOUTH_Y1 - MOUTH_Y0, C_BG);
  const int w = strlen(rows[0]) * MBLK, h = n * MBLK;
  const int ox = 160 - w / 2, oy = 164 - h / 2;
  for (int y = 0; y < n; y++)
    for (int x = 0; rows[y][x]; x++)
      if (rows[y][x] == 'X') tft.fillRect(ox + x * MBLK, oy + y * MBLK, MBLK, MBLK, C_DARK);
}

int pickMood() {
  if (alert.active) return alert.good ? M_BIG : M_SAD;
  if (usage.typing) return M_SMILE;
  switch (shownCC()) {
    case CC_WAITING: return M_WOW;
    case CC_WORKING: return M_OH;
    case CC_DONE:    return M_BIG;
    default: break;
  }
  if (!usage.ok) return M_FLAT;
  float p = max(usage.sPct, usage.wPct);
  return p >= 80 ? M_SAD : (p >= 50 ? M_FLAT : M_SMILE);
}

// ---------------- testi ----------------

void drawLabels() {
  pixelFont(tft, 2);
  tft.setTextColor(C_SOFT);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("SESSION 5H", EYE_LX, LABEL_Y);
  tft.drawString("WEEK", EYE_RX, LABEL_Y);
}

// testo su sprite: si riscrive ogni secondo senza residui ne' sfarfallio
void drawRow(int cx, int cy, const String& s, uint16_t col) {
  row.fillSprite(C_BG);
  fitFont(row, s, ROW_W, 2);
  row.setTextDatum(MC_DATUM);
  row.setTextColor(col);
  row.drawString(s, ROW_W / 2, ROW_H / 2);
  row.pushSprite(cx - ROW_W / 2, cy - ROW_H / 2);
}

void drawResets() {
  drawRow(EYE_LX, RESET_Y, usage.ok ? fmtRemaining(usage.sReset) : "", C_DARK);
  drawRow(EYE_RX, RESET_Y, usage.ok ? fmtRemaining(usage.wReset) : "", C_DARK);
}

void drawFooter() {
  String text;
  uint16_t bg = C_BG, fg = C_SOFT;
  uint8_t size = 2;
  if (millis() < toastUntil) {
    text = toast;
    fg = C_DARK;
  } else if (alert.active) {
    text = alert.text;
    bg = alert.good ? C_GOOD : C_HOT;
    fg = C_WHITE;
  } else if (usage.typing && usage.ok) {
    text = "Typing...";
    fg = C_DARK;
  } else if (!usage.ok) {
    text = usage.err.substring(0, 50);
    bg = C_HOT;
    fg = C_WHITE;
  } else {
    switch (shownCC()) {
      case CC_WAITING: text = "Claude needs you!"; bg = C_DARK; fg = C_BG; break;
      case CC_WORKING: text = "Claude is working..."; fg = C_DARK; break;
      case CC_DONE:    text = "Done!"; fg = C_DARK; break;
      default:
        if (sleeping && nowEpoch()) {
          // mentre dorme: l'ora (dall'orologio del bridge, fuso italiano)
          time_t e = nowEpoch();
          struct tm lt;
          localtime_r(&e, &lt);
          char buf[6];
          strftime(buf, sizeof(buf), "%H:%M", &lt);
          text = buf;
          size = 3;
        } else {
          String p = usage.plan;
          p.toUpperCase();
          text = "Claude " + p;
        }
    }
    if (usage.ccN > 1 && shownCC() != CC_IDLE) text += "  (" + String(usage.ccN) + ")";
  }
  if (text == lastFooter && bg == lastFooterBg) return;
  lastFooter = text;
  lastFooterBg = bg;
  tft.fillRect(0, FOOT_Y, 320, 240 - FOOT_Y, bg);
  fitFont(tft, text, 312, size);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg);
  tft.drawString(text, 160, FOOT_Y + 12);
}

// ---------------- LED ----------------

void led(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

void updateLed() {
  uint32_t t = millis();
  bool blink = (t / 400) % 2;
  if (alert.active) {
    if (alert.good) led(false, blink, false);
    else led(blink, false, false);
    return;
  }
  if (usage.typing) { led(false, true, true); return; }          // azzurro
  CC cc = shownCC();
  if (cc == CC_WAITING) { led(blink, blink, false); return; }      // giallo
  if (cc == CC_WORKING) { led(false, false, true); return; }       // blu
  if (cc == CC_DONE && t - doneAt < 2400) { led(false, blink, false); return; }
  // a riposo: spento, rosso fisso se un limite e' oltre l'80%
  led(usage.ok && max(usage.sPct, usage.wPct) >= 80, false, false);
}

// ---------------- avvisi ----------------

void raiseAlert(const String& text, bool good) {
  alert.active = true;
  alert.good = good;
  alert.text = text;
  alert.since = millis();
}

int levelOf(float p) { return p >= 95 ? 95 : (p >= 80 ? 80 : 0); }

void checkWindow(const char* name, float pct, float& prev, int& announced) {
  int lvl = levelOf(pct);
  if (lvl > announced) {
    raiseAlert(String(name) + " at " + String(lvl) + "%!", false);
  } else if (prev >= 50 && pct <= prev - 30) {
    // la percentuale e' crollata: la finestra si e' azzerata
    raiseAlert(String(name) + " available again!", true);
  }
  announced = lvl;
  prev = pct;
}

void checkAlerts() {
  if (!usage.ok) return;
  if (!haveData) {          // prima lettura: nessun avviso, solo memoria
    haveData = true;
    sLevel = levelOf(usage.sPct);
    wLevel = levelOf(usage.wPct);
    sPrev = usage.sPct;
    wPrev = usage.wPct;
    return;
  }
  checkWindow("Session", usage.sPct, sPrev, sLevel);
  checkWindow("Week", usage.wPct, wPrev, wLevel);
}

// ---------------- dati ----------------

void listenBeacon() {
  if (!beaconUdp.parsePacket()) return;
  char buf[32] = {0};
  beaconUdp.read(buf, sizeof(buf) - 1);
  if (strncmp(buf, "TOKENBUDDY ", 11)) return;
  String url = "http://" + beaconUdp.remoteIP().toString() + ":" + String(atoi(buf + 11)) + "/usage";
  if (url == bridgeUrl) return;
  bridgeUrl = url;
  prefs.putString("bridge", bridgeUrl);
  Serial.println("bridge trovato: " + bridgeUrl);
}

bool pollBridge() {
  if (WiFi.status() != WL_CONNECTED) {
    usage.ok = false;
    usage.err = "WiFi disconnected";
    return false;
  }
  // statici: la connessione TCP resta aperta tra una richiesta e l'altra
  static WiFiClient net;
  static HTTPClient http;
  static int fails = 0;
  if (bridgeUrl.isEmpty()) {
    usage.ok = false;
    usage.err = "Looking for the bridge...";
    return false;
  }
  http.begin(net, bridgeUrl);
  http.setReuse(true);
  http.setTimeout(3000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("bridge -> %d (%s)\n", code, http.errorToString(code).c_str());
    http.end();
    // un intoppo isolato del WiFi non deve far sparire i dati
    if (++fails < 3) return usage.ok;
    usage.ok = false;
    usage.err = "Bridge unreachable (" + String(code) + ")";
    return false;
  }
  fails = 0;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getString());
  http.end();
  if (e) {
    usage.ok = false;
    usage.err = "Invalid JSON";
    return false;
  }
  serverNow = doc["now"] | 0;
  serverNowAt = millis();
  usage.ok = doc["ok"] | false;
  usage.err = doc["err"] | "Unknown error";
  usage.sPct = doc["s_pct"] | 0.0;
  usage.wPct = doc["w_pct"] | 0.0;
  usage.sReset = doc["s_reset"] | 0;
  usage.wReset = doc["w_reset"] | 0;
  usage.plan = doc["plan"] | "";

  const char* cc = doc["cc"] | "idle";
  usage.cc = !strcmp(cc, "working") ? CC_WORKING
           : !strcmp(cc, "done")    ? CC_DONE
           : !strcmp(cc, "waiting") ? CC_WAITING : CC_IDLE;
  usage.ccSeq = doc["cc_seq"] | 0;
  usage.ccN = doc["cc_n"] | 0;
  usage.typing = doc["typing"] | false;
  uint32_t keys = doc["keys"] | 0;
  if (keys > usage.keys && usage.keys)                 // tasti nuovi da animare
    pendingPresses = min(pendingPresses + (int)(keys - usage.keys + 1) / 2, 20);
  usage.keys = keys;
  if (usage.cc == CC_DONE && usage.ccSeq != doneSeq) {
    doneSeq = usage.ccSeq;
    doneAt = millis();
  }
  return usage.ok;
}

// ridisegna occhi e numeri solo se e' cambiato qualcosa
void refresh() {
  float s = usage.sPct, w = usage.wPct;
  bool ok = usage.ok;
  pollBridge();
  checkAlerts();
  if (updateMode()) return;           // cambio modalita': ha gia' ridisegnato tutto
  if (!sleeping && (s != usage.sPct || w != usage.wPct || ok != usage.ok)) drawEyes();
  if (!sleeping) drawMouth(pickMood());
  drawFooter();
}

// ---------------- tocco e animazioni ----------------

// ---------------- suono ----------------

void tone2(int freq, int ms) {           // tono che si spegne da solo nel loop
  ledcWriteTone(SPK_CH, freq);
  soundOffAt = millis() + ms;
}

void soundTick() {
  if (soundOffAt && millis() >= soundOffAt) {
    ledcWriteTone(SPK_CH, 0);
    soundOffAt = 0;
  }
}

void keyClick() {
  if (soundOn) tone2(1800 + random(400), 4);   // tic breve, leggermente diverso ogni volta
}

void bootChime() {
  const int notes[] = {880, 1175, 1568};
  for (int f : notes) {
    ledcWriteTone(SPK_CH, f);
    delay(90);
  }
  ledcWriteTone(SPK_CH, 0);
}

void shortTap() {
  if (sleeping) {
    peekUntil = millis() + PEEK_MS;     // sbircia le percentuali
    updateMode();
    return;
  }
  if (alert.active) alert.active = false;
  else dismissedSeq = usage.ccSeq;      // chiude "fatto" / "ti aspetta"
  if (updateMode()) return;
  drawMouth(pickMood());
  drawFooter();
}

// tocco breve: come sempre; tenuto premuto 0,8 s: tema successivo
void handleTouch() {
  static bool down = false, longDone = false;
  static uint32_t t0 = 0;
  bool touching = ts.touched();
  if (touching && !down) {
    down = true;
    longDone = false;
    t0 = millis();
  } else if (touching && !longDone && millis() - t0 > 800) {
    longDone = true;
    setTheme((themeIdx + 1) % THEME_COUNT, true);
  } else if (!touching && down) {
    down = false;
    if (!longDone && millis() - t0 > 30) shortTap();
  }
}

// ---------------- temi ----------------

uint16_t hex565(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

uint32_t blend(uint32_t a, uint32_t b, float t) {
  uint32_t out = 0;
  for (int sh = 0; sh <= 16; sh += 8) {
    int ca = (a >> sh) & 255, cb = (b >> sh) & 255;
    out |= (uint32_t)(ca + (cb - ca) * t) << sh;
  }
  return out;
}

void applyColors() {
  const Theme& t = THEMES[themeIdx];
  C_BG = hex565(t.bg);
  C_DARK = C_SOFT = hex565(t.text);
  C_TRACK = hex565(t.track);
  C_EYE = hex565(t.eye);
  C_NUM = hex565(t.num);
  C_OK = hex565(t.ok);
  C_MID = hex565(t.mid);
  C_HOT = hex565(t.hot);
}

void redrawAll() {
  if (!sleeping) {
    drawAwakeScreen();
  } else {
    tft.fillScreen(C_BG);
    lastFooter = "";
    drawFooter();
    lastSleepFrame = 0;
  }
}

void setTheme(int i, bool announce) {
  themeIdx = constrain(i, 0, THEME_COUNT - 1);
  prefs.putUChar("theme", themeIdx);
  applyColors();
  if (announce) {
    toast = String("Theme: ") + THEMES[themeIdx].name + "  " + HOSTNAME + ".local";
    toastUntil = millis() + 4000;
  }
  redrawAll();
}

void loadTheme() {
  prefs.begin("tokenbuddy");
  uint32_t c[6];
  if (prefs.getBytes("custom", c, sizeof(c)) == sizeof(c)) {
    Theme& t = THEMES[CUSTOM];
    t.bg = c[0]; t.text = c[1]; t.eye = c[2]; t.ok = c[3]; t.mid = c[4]; t.hot = c[5];
    t.num = t.text;
    t.track = blend(t.bg, t.text, 0.35);
  }
  themeIdx = constrain((int)prefs.getUChar("theme", 0), 0, THEME_COUNT - 1);
  soundOn = prefs.getBool("sound", true);
  bridgeUrl = prefs.getString("bridge", "");
  applyColors();
}

// ---------------- pagina web dei temi ----------------

const char PAGE[] PROGMEM = R"html(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Token Buddy</title>
<style>body{font-family:system-ui,sans-serif;max-width:420px;margin:20px auto;padding:0 16px;color:#222}
button{font:inherit;cursor:pointer}.t{display:flex;align-items:center;gap:12px;width:100%;margin:6px 0;padding:10px;border:2px solid #ddd;border-radius:10px;background:#fff}
.t.on{border-color:#e8873a}.sw{width:44px;height:28px;border-radius:6px;display:flex;align-items:center;justify-content:center;font-weight:bold}
label{display:flex;justify-content:space-between;align-items:center;margin:8px 0}input[type=color]{width:56px;height:32px;border:0;background:none}
#w{color:#b00020;min-height:1.2em}</style></head><body>
<h2>Token Buddy colors</h2><div id="list"></div>
<h3>Sound</h3><label>Click when you type<input type="checkbox" id="snd" style="width:24px;height:24px"></label>
<h3>Custom</h3>
<label>Background<input type="color" id="bg"></label><label>Text and face<input type="color" id="text"></label>
<label>Inside the rings<input type="color" id="eye"></label><label>Bar under 50%<input type="color" id="ok"></label>
<label>Bar 50-80%<input type="color" id="mid"></label><label>Bar over 80%<input type="color" id="hot"></label>
<p id="w"></p>
<script>
const ids=["bg","text","eye","ok","mid","hot"];let timer;
function lum(h){const c=[1,3,5].map(i=>{let v=parseInt(h.substr(i,2),16)/255;return v<=0.03928?v/12.92:Math.pow((v+0.055)/1.055,2.4)});return 0.2126*c[0]+0.7152*c[1]+0.0722*c[2]}
function ratio(a,b){const x=lum(a),y=lum(b);return (Math.max(x,y)+0.05)/(Math.min(x,y)+0.05)}
function draw(s){const l=document.getElementById("list");l.innerHTML="";s.themes.forEach((t,i)=>{const b=document.createElement("button");
b.className="t"+(i==s.current?" on":"");b.innerHTML='<span class="sw" style="background:'+t.bg+';color:'+t.text+'">Aa</span>'+t.name;
b.onclick=()=>fetch("/theme?i="+i).then(load);l.appendChild(b)});ids.forEach(k=>document.getElementById(k).value=s.custom[k]);document.getElementById("snd").checked=s.sound}
function load(){fetch("/state").then(r=>r.json()).then(draw)}
function custom(){const v={};ids.forEach(k=>v[k]=document.getElementById(k).value);
document.getElementById("w").textContent=ratio(v.bg,v.text)<3?"Background and text are too similar: hard to read.":"";
clearTimeout(timer);timer=setTimeout(()=>fetch("/custom?"+ids.map(k=>k+"="+v[k].substr(1)).join("&")).then(load),250)}
ids.forEach(k=>document.getElementById(k).oninput=custom);
document.getElementById("snd").onchange=e=>fetch("/sound?on="+(e.target.checked?1:0));load();
</script></body></html>)html";

String hexStr(uint32_t c) {
  char b[8];
  snprintf(b, sizeof(b), "#%06lX", (unsigned long)c);
  return b;
}

void handleState() {
  JsonDocument doc;
  doc["current"] = themeIdx;
  doc["sound"] = soundOn;
  JsonArray arr = doc["themes"].to<JsonArray>();
  for (int i = 0; i < THEME_COUNT; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = THEMES[i].name;
    o["bg"] = hexStr(THEMES[i].bg);
    o["text"] = hexStr(THEMES[i].text);
  }
  const Theme& c = THEMES[CUSTOM];
  JsonObject cu = doc["custom"].to<JsonObject>();
  cu["bg"] = hexStr(c.bg); cu["text"] = hexStr(c.text); cu["eye"] = hexStr(c.eye);
  cu["ok"] = hexStr(c.ok); cu["mid"] = hexStr(c.mid); cu["hot"] = hexStr(c.hot);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleCustom() {
  Theme& t = THEMES[CUSTOM];
  uint32_t* fields[] = {&t.bg, &t.text, &t.eye, &t.ok, &t.mid, &t.hot};
  const char* names[] = {"bg", "text", "eye", "ok", "mid", "hot"};
  uint32_t c[6];
  for (int i = 0; i < 6; i++) {
    if (server.hasArg(names[i])) *fields[i] = strtoul(server.arg(names[i]).c_str(), nullptr, 16) & 0xFFFFFF;
    c[i] = *fields[i];
  }
  t.num = t.text;
  t.track = blend(t.bg, t.text, 0.35);
  prefs.putBytes("custom", c, sizeof(c));
  setTheme(CUSTOM, false);
  server.send(200, "text/plain", "ok");
}

void startWeb() {
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/state", handleState);
  server.on("/theme", []() {
    setTheme(server.arg("i").toInt(), false);
    server.send(200, "text/plain", "ok");
  });
  server.on("/custom", handleCustom);
  server.on("/sound", []() {
    soundOn = server.arg("on") == "1";
    prefs.putBool("sound", soundOn);
    if (soundOn) tone2(1500, 60);       // conferma udibile
    server.send(200, "text/plain", "ok");
  });
  server.begin();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// ---------------- modalita' dorme ----------------

// cerchi se c'e' un avviso, un errore, Claude lavora / ha finito / aspetta,
// o l'utente ha toccato lo schermo da poco; tastiera mentre scrivi; se no dorme
Scene wantScene() {
  if (alert.active || !usage.ok) return SC_RINGS;
  if (usage.typing) return SC_TYPING;
  if (shownCC() != CC_IDLE || millis() < peekUntil) return SC_RINGS;
  return SC_SLEEP;
}

void drawAwakeScreen() {
  tft.fillScreen(C_BG);
  lastMood = -1;
  lastFooter = "";
  eyeDx = eyeDy = 0;
  drawLabels();
  drawEyes();
  drawMouth(pickMood());
  drawFooter();
  drawResets();
}

// passa tra cerchi, dorme e tastiera; true se ha cambiato schermata
bool updateMode() {
  Scene want = wantScene();
  if (want == scene) return false;
  scene = want;
  sleeping = scene != SC_RINGS;
  if (sleeping) {
    tft.fillScreen(C_BG);
    lastFooter = "";
    drawFooter();
    lastSleepFrame = 0;
  } else {
    drawAwakeScreen();
  }
  return true;
}

// un blocco del disegno pixel
void blk(int ox, int oy, int x, int y, int s) {
  face.fillRect(ox + x * s, oy + y * s, s, s, C_DARK);
}

// disegna una figura da righe di testo ('X' = pixel pieno)
void pattern(int ox, int oy, const char* const* rows, int n, int s) {
  for (int y = 0; y < n; y++)
    for (int x = 0; rows[y][x]; x++)
      if (rows[y][x] == 'X') blk(ox, oy, x, y, s);
}

const char* const EYE_CLOSED[] = {"X....X", ".XXXX."};
const char* const MOUTH_FLAT[] = {"XXXX"};
const char* const MOUTH_O[] = {".XX.", "X..X", ".XX."};
const char* const EYE_FOCUS[] = {".XXXX.", "XXXXXX"};
const char* const LETTER_Z[] = {"XXXXX", "...X.", "..X..", ".X...", "XXXXX"};

void drawSleep() {
  uint32_t t = millis();
  if (t - lastSleepFrame < 80) return;   // ~12 fotogrammi al secondo bastano
  lastSleepFrame = t;
  float breath = sin(t / 1300.0);        // respiro lento
  int dy = (int)round(breath * 4);

  face.fillSprite(C_BG);
  pattern(60, 75 + dy, EYE_CLOSED, 2, BLK);
  pattern(180, 75 + dy, EYE_CLOSED, 2, BLK);
  // inspira a bocca chiusa, russa a bocca aperta
  if (breath > 0.2) pattern(130, 125 + dy, MOUTH_O, 3, BLK);
  else pattern(130, 135 + dy, MOUTH_FLAT, 1, BLK);

  // Z che compaiono una alla volta e salgono
  int phase = (t / 700) % 4;
  int rise = (t % 700) * 6 / 700;
  const int zx[] = {232, 252, 274}, zy[] = {62, 38, 10}, zs[] = {3, 4, 5};
  for (int i = 0; i < phase; i++)
    pattern(zx[i], zy[i] - (i == phase - 1 ? rise : 6), LETTER_Z, 5, zs[i]);

  face.pushSprite(SL_X, SL_Y);
}

// ---------------- schermata "stai scrivendo" ----------------

// tastiera pixel: 3 file di tasti + barra spaziatrice (id 99)
#define KEY_W   20
#define KEY_H   18
#define KEY_GAP 4
const int KB_ROWS = 3, KB_KEYS[KB_ROWS] = {11, 10, 9}, KB_X0[KB_ROWS] = {20, 30, 40};
const int KB_Y0 = 104, SPACE_X = 90, SPACE_W = 120;

int keyX(int id) {                       // centro orizzontale del tasto
  if (id == 99) return SPACE_X + SPACE_W / 2;
  int r = id / 11, k = id % 11;
  return KB_X0[r] + k * (KEY_W + KEY_GAP) + KEY_W / 2;
}

void drawKey(int x, int y, int w, bool down) {
  if (down) face.fillRect(x, y + 3, w, KEY_H - 3, C_MID);   // premuto: piu' basso e arancione
  else face.fillRect(x, y, w, KEY_H, C_EYE);
}

// manina pixel con braccio che sale dal bordo in basso
void drawHand(int cx, int top) {
  const int w = 26, h = 16;
  face.fillRect(cx - 7, top + h - 2, 14, SL_H - top, C_DARK);          // braccio (bordo)
  face.fillRect(cx - 5, top + h - 2, 10, SL_H - top, C_BG);
  face.fillRect(cx - w / 2 - 1, top - 1, w + 2, h + 2, C_DARK);         // mano (bordo)
  face.fillRect(cx - w / 2, top, w, h, C_BG);
  for (int f = 1; f < 4; f++)                                           // dita
    face.fillRect(cx - w / 2 + f * 6 + 1, top, 2, 6, C_DARK);
}

void drawTyping() {
  static int pressed = -1, lastHand = 0;
  static uint32_t nextPress = 0, pressedAt = 0;
  static int look = 0;
  static int handX[2] = {85, 215};
  uint32_t t = millis();
  if (t - lastSleepFrame < 40) return;
  lastSleepFrame = t;

  // si preme un tasto solo per ogni tasto vero arrivato dal bridge
  if (pendingPresses > 0 && t >= nextPress) {
    pendingPresses--;
    if (random(8) == 0) pressed = 99;
    else {
      int r = random(KB_ROWS);
      pressed = r * 11 + random(KB_KEYS[r]);
    }
    pressedAt = t;
    keyClick();
    nextPress = t + constrain(450 / (pendingPresses + 1), 60, 200);
  }
  if (pressed >= 0 && t - pressedAt > 110) pressed = -1;    // tasto rilasciato

  // la mano piu' vicina va sul tasto, l'altra torna a riposo
  const int rest[2] = {85, 215};
  int active = -1;
  if (pressed >= 0) {
    active = keyX(pressed) < 150 ? 0 : 1;
    lastHand = active;
  }
  for (int i = 0; i < 2; i++) {
    int target = (i == active) ? keyX(pressed) : rest[i];
    handX[i] += (target - handX[i]) / 2;
  }
  int lookTarget = constrain((handX[lastHand] - 150) / 12, -8, 8);
  look += (lookTarget > look) - (lookTarget < look);

  face.fillSprite(C_BG);
  // stessa faccia pixel delle altre schermate: occhi socchiusi verso la tastiera
  pattern(60 + look, 28, EYE_FOCUS, 2, BLK);
  pattern(180 + look, 28, EYE_FOCUS, 2, BLK);
  pattern(130 + look / 2, 66, M_SMILE_PX, 3, MBLK);

  face.fillRoundRect(10, 96, 280, 100, 6, C_DARK);
  int pressedY = KB_Y0;
  for (int r = 0; r < KB_ROWS; r++)
    for (int k = 0; k < KB_KEYS[r]; k++) {
      int id = r * 11 + k;
      int y = KB_Y0 + r * (KEY_H + KEY_GAP);
      if (id == pressed) pressedY = y;
      drawKey(KB_X0[r] + k * (KEY_W + KEY_GAP), y, KEY_W, id == pressed);
    }
  int spaceY = KB_Y0 + KB_ROWS * (KEY_H + KEY_GAP);
  if (pressed == 99) pressedY = spaceY;
  drawKey(SPACE_X, spaceY, SPACE_W, pressed == 99);

  // mani: quella che preme scende sul tasto, lasciandone vedere la parte alta
  for (int i = 0; i < 2; i++) {
    int top = (i == active) ? pressedY + 9 : 150;
    drawHand(handX[i], top);
  }

  face.pushSprite(SL_X, SL_Y);
}

void animate() {
  uint32_t t = millis();
  if (t - lastFrame < 40) return;      // ~25 fotogrammi al secondo
  lastFrame = t;
  int dx = 0, dy = 0;
  CC cc = shownCC();
  if (usage.typing) {
    dy = (int)round(MOVE_Y * sin(t / 140.0));            // annuisce mentre scrivi
  } else if (cc == CC_WORKING) {
    dx = (int)round(MOVE_X * sin(t / 600.0));            // sguardo a destra e sinistra
  } else if (cc == CC_WAITING) {
    dy = -(int)round(MOVE_Y * fabs(sin(t / 180.0)));     // saltelli
  }
  if (dx == eyeDx && dy == eyeDy) return;
  eyeDx = dx;
  eyeDy = dy;
  drawEyes();
}

void setup() {
  Serial.begin(115200);
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   // ora italiana con l'ora legale
  tzset();
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  led(false, false, true);    // blu = avvio

  tft.init();
#ifdef ILI9341_2_DRIVER
  // i CYD 2.8 escono con colori slavati: riselezionare la curva gamma li ravviva
  tft.writecommand(0x26);     // ILI9341 GAMMASET
  tft.writedata(2);
  delay(120);
  tft.writecommand(0x26);
  tft.writedata(1);
#endif
  tft.setRotation(1);         // orizzontale; prova 3 se e' capovolto
  Serial.printf("Token Buddy su %s\n", BOARD_NAME);
  loadTheme();
  tft.fillScreen(C_BG);
  eye.setColorDepth(16);
  eye.createSprite(EYE_S, EYE_S);
  row.setColorDepth(16);
  row.createSprite(ROW_W, ROW_H);
  face.setColorDepth(8);              // 8 bit: meta' memoria, i colori pieni bastano
  face.createSprite(SL_W, SL_H);
  Serial.printf("RAM libera: %u\n", ESP.getFreeHeap());

  ledcSetup(SPK_CH, 2000, 8);
  ledcAttachPin(SPK_PIN, SPK_CH);
  ledcWriteTone(SPK_CH, 0);
  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  ts.begin(touchSPI);

  usage.err = "Connecting WiFi...";
  drawEyes();
  drawFooter();

  WiFiManager wm;
  wm.setAPCallback([](WiFiManager*) {
    tft.fillScreen(C_BG);
    tft.setTextDatum(MC_DATUM);
    pixelFont(tft, 3);
    tft.setTextColor(C_DARK);
    tft.drawString("WiFi setup", 160, 80);
    tft.drawString(AP_NAME, 160, 148);
    pixelFont(tft, 2);
    tft.setTextColor(C_SOFT);
    tft.drawString("With your phone join", 160, 106);
    tft.drawString("the network", 160, 124);
    tft.drawString("and pick your WiFi", 160, 180);
  });
  wm.setConfigPortalTimeout(300);
  if (!wm.autoConnect(AP_NAME)) ESP.restart();
  WiFi.setSleep(false);       // il risparmio energetico perde pacchetti
  startWeb();
  beaconUdp.begin(BEACON_PORT);
  if (soundOn) bootChime();      // se si sente, l'altoparlante c'e'

  pollBridge();
  checkAlerts();
  drawAwakeScreen();
  updateMode();
  lastPoll = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  uint32_t t = millis();

  if (t - lastPoll >= POLL_MS) {
    lastPoll = t;
    refresh();
  }

  if (alert.active && alert.good && t - alert.since >= GOOD_ALERT_MS) {
    alert.active = false;
    if (!updateMode()) {
      drawMouth(pickMood());
      drawFooter();
    }
  }

  server.handleClient();
  listenBeacon();
  soundTick();
  handleTouch();
  updateMode();                       // es. fine della "sbirciata" col tocco
  updateLed();

  if (sleeping) {
    if (scene == SC_TYPING) drawTyping();
    else drawSleep();
    if (t - lastTick >= 1000) {         // l'orologio cambia solo al cambio minuto
      lastTick = t;
      drawFooter();
    }
  } else {
    animate();
    if (t - lastTick >= 1000) {
      lastTick = t;
      drawResets();
    }
  }
  delay(5);
}
