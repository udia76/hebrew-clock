#include <GxEPD2_3C.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PNGdec.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <RTClib.h>
#include <esp_system.h>

// Waveshare e-Paper ESP32 Driver Board (Rev 3) -> HINK-E075A07-A0 7.5" 880x528 B/W/R (SSD1677, GxEPD2_750c_Z90)
// The board wires the panel to fixed, non-default SPI pins, so SPI is re-mapped in setup().
#define EPD_SCK  13
#define EPD_MISO 12   // not connected on the board, only needed by SPI.begin()
#define EPD_MOSI 14   // "DIN"
#define EPD_CS   15
#define EPD_DC   27
#define EPD_RST  26
#define EPD_BUSY 25

#define PANEL_W 880
#define PANEL_H 528
#define ROW_BYTES (PANEL_W / 8)   // 110

// Page buffer kept small (HEIGHT/16) so static DRAM fits on a classic ESP32.
GxEPD2_3C<GxEPD2_750c_Z90, GxEPD2_750c_Z90::HEIGHT / 16> display(
    GxEPD2_750c_Z90(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

#define PNG_BUF_SIZE 16000
static uint8_t *pngBuf = nullptr;   // allocated per fetch
// Rows of the current display page; the PNG callback only draws rows inside this window.
static int gPageY0 = 0, gPageY1 = 0;

PNG         png;
Preferences prefs;
WebServer   server(80);

String   imageUrl;
uint32_t refreshInterval  = 60;
uint32_t lastRefresh      = 0;
uint8_t  refreshCount     = 0;
int      failCount        = 0;   // consecutive failed fetches
String   otaPassword      = "";  // empty = wireless (OTA) updates disabled
bool     otaEnabled       = false;
// Last fetch result, shown on http://<device-ip>/status for debugging without USB.
static String   gLastResult  = "no fetch yet";
static uint32_t gLastOkMs    = 0;
static uint32_t gOkCount     = 0;
static uint32_t gFailTotal   = 0;
static const char* gResetWhy = "?";
String   selectedFont     = "Heebo-Bold";
String   location         = "Tel Aviv";

// Sleep schedule (ESP-side only — not sent to server as URL params)
bool     sleepEnabled = false;
String   sleepStart   = "22:00";  // HH:MM 24h
String   sleepEnd     = "06:00";

// Calendar
String calendarType = "gregorian";  // "gregorian" | "jewish"

// RTC
bool       rtcEnabled = false;
RTC_DS3231 rtc;

// ── Time helpers ──────────────────────────────────────

// Returns minutes-from-midnight parsed from "HH:MM"
static int toMinutes(const String& hhmm) {
    return hhmm.substring(0, 2).toInt() * 60 + hhmm.substring(3, 5).toInt();
}

// True when the current local time falls inside the sleep window.
// Handles overnight windows (e.g. 22:00 – 06:00).
bool isInSleepWindow() {
    if (!sleepEnabled) return false;
    struct tm ti;
    if (!getLocalTime(&ti, 0)) return false;
    int now   = ti.tm_hour * 60 + ti.tm_min;
    int start = toMinutes(sleepStart);
    int end   = toMinutes(sleepEnd);
    return (start <= end) ? (now >= start && now < end)
                          : (now >= start || now < end);
}

// Active interval in seconds: 5 min during sleep window, configured otherwise.
uint32_t activeInterval() {
    return isInSleepWindow() ? 300 : refreshInterval;
}

// ── URL helpers ───────────────────────────────────────

String urlEncode(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            sprintf(buf, "%%%02X", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

String buildFetchUrl() {
    String url = imageUrl;
    // Ensure a path exists before the query string ("https://host" → "https://host/")
    int schemeEnd = url.indexOf("://");
    if (schemeEnd >= 0 && url.indexOf('/', schemeEnd + 3) < 0)
        url += "/";
    url += (url.indexOf('?') >= 0) ? "&" : "?";
    url += "font="       + urlEncode(selectedFont);
    url += "&sleeptime=" + String(isInSleepWindow() ? "1" : "0");
    url += "&location="  + urlEncode(location.length() > 0 ? location : "Tel Aviv");
    url += "&calendar="  + calendarType;
    return url;
}

// ── Web config ────────────────────────────────────────

static const char CONFIG_HTML[] PROGMEM = R"(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ePaper Config</title>
  <style>
    body{font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 16px}
    h2{margin-bottom:24px}
    label{display:block;margin-top:16px;font-size:.9em;color:#555}
    input[type=text],input[type=number],input[type=time],select{
      width:100%;padding:8px;box-sizing:border-box;
      border:1px solid #ccc;border-radius:4px;font-size:1em}
    .row{display:flex;align-items:center;margin-top:16px;gap:10px}
    .row input[type=checkbox]{width:18px;height:18px;margin:0;flex-shrink:0}
    .row span{font-size:.9em;color:#555}
    .sleep-fields{margin-top:0;padding:12px 0 0 28px;display:none}
    .sleep-fields label{margin-top:10px}
    .time-row{display:flex;gap:12px}
    .time-row > div{flex:1}
    button{margin-top:24px;width:100%;padding:10px;background:#333;color:#fff;
      border:none;border-radius:4px;font-size:1em;cursor:pointer}
    button:hover{background:#555}
    .note{font-size:.8em;color:#999;margin-top:4px}
    .saved{color:green;margin-top:12px;display:none}
    hr{margin:24px 0;border:none;border-top:1px solid #eee}
  </style>
</head>
<body>
  <h2>ePaper Configuration</h2>
  <form method="POST" action="/config">

    <label>Image URL</label>
    <input type="text" name="url" value="{{URL}}" placeholder="http://...">

    <label>Font</label>
    <select name="font">{{FONT_OPTIONS}}</select>

    <label>Location</label>
    <input type="text" name="location" value="{{LOCATION}}" placeholder="Tel Aviv">

    <label>Calendar type</label>
    <select name="calendar">
      <option value="gregorian" {{CALENDAR_GREGORIAN}}>Gregorian</option>
      <option value="jewish"    {{CALENDAR_JEWISH}}>Jewish (Hebrew)</option>
    </select>

    <hr>

    <div class="row">
      <input type="checkbox" name="sleep_en" id="sleep_en" value="1" {{SLEEP_EN_CHECKED}}
             onchange="document.getElementById('sf').style.display=this.checked?'block':'none'">
      <span>Enable sleep schedule</span>
    </div>
    <div class="sleep-fields" id="sf">
      <p class="note">During this window the refresh rate is fixed at 5 minutes and the server is told to show the sleep image.</p>
      <div class="time-row">
        <div>
          <label>Sleep start</label>
          <input type="time" name="sleep_start" value="{{SLEEP_START}}">
        </div>
        <div>
          <label>Sleep end</label>
          <input type="time" name="sleep_end" value="{{SLEEP_END}}">
        </div>
      </div>
    </div>

    <hr>

    <label>Refresh interval (seconds, outside sleep window)</label>
    <input type="number" name="interval" value="{{INTERVAL}}" min="1">

    <label>Wireless update (OTA) password</label>
    <input type="password" name="ota_pw" value="" placeholder="{{OTA_STATE}}" autocomplete="new-password">
    <p class="note">Min. 6 characters. Leave empty to keep the current setting, type <b>-</b> to disable OTA. Takes effect after restart.</p>

    <hr>

    <div class="row">
      <input type="checkbox" name="rtc_en" id="rtc_en" value="1" {{RTC_EN_CHECKED}}>
      <span>Enable DS3231 RTC module</span>
    </div>
    <p class="note">Wire SDA&rarr;GPIO21, SCL&rarr;GPIO22. On NTP success the RTC is updated; if NTP is unavailable the system clock is restored from the RTC so the sleep schedule keeps working.</p>

    <button type="submit">Save &amp; restart</button>
  </form>
  <p class="saved" id="sv">Saved! Device is restarting...</p>
  <script>
    var cb = document.getElementById('sleep_en');
    document.getElementById('sf').style.display = cb.checked ? 'block' : 'none';
    if(location.search.includes('saved')) document.getElementById('sv').style.display='block';
  </script>
</body>
</html>)";

static const char* FONTS[] = {
    "DavidLibre-Bold", "FrankRuhlLibre-Bold", "FrankRuhlLibre",
    "Heebo-Bold", "NotoSansHebrew-Bold"
};

void handleStatus() {
    auto ago = [](uint32_t t) { return t ? String((millis() - t) / 1000) + " s ago" : String("never"); };
    String s = "Last fetch: " + gLastResult + "\n";
    s += "Last OK: " + ago(gLastOkMs) + "\n";
    s += "OK count: " + String(gOkCount) + "   total failures: " + String(gFailTotal) +
         "   failures in a row: " + String(failCount) + "\n";
    s += "Uptime: " + String(millis() / 1000) + " s\n";
    s += "Reset reason: " + String(gResetWhy) + "\n";
    s += "Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "connected" : "DOWN") +
         "  RSSI " + String(WiFi.RSSI()) + " dBm\n";
    s += "Free heap: " + String(ESP.getFreeHeap()) + "  largest block: " + String(ESP.getMaxAllocHeap()) + "\n";
    s += "Interval: " + String(refreshInterval) + " s\n";
    server.send(200, "text/plain; charset=utf-8", s);
}

void handleRoot() {
    String fontOptions;
    for (const char* f : FONTS) {
        fontOptions += "<option value=\"";
        fontOptions += f;
        fontOptions += "\"";
        if (selectedFont == f) fontOptions += " selected";
        fontOptions += ">";
        fontOptions += f;
        fontOptions += "</option>";
    }
    String html = FPSTR(CONFIG_HTML);
    html.replace("{{URL}}",              imageUrl);
    html.replace("{{FONT_OPTIONS}}",     fontOptions);
    html.replace("{{LOCATION}}",           location);
    html.replace("{{CALENDAR_GREGORIAN}}", calendarType == "gregorian" ? "selected" : "");
    html.replace("{{CALENDAR_JEWISH}}",    calendarType == "jewish"    ? "selected" : "");
    html.replace("{{SLEEP_EN_CHECKED}}",   sleepEnabled ? "checked" : "");
    html.replace("{{SLEEP_START}}",      sleepStart);
    html.replace("{{SLEEP_END}}",        sleepEnd);
    html.replace("{{INTERVAL}}",         String(refreshInterval));
    html.replace("{{OTA_STATE}}",        otaPassword.length() ? "OTA enabled (password set)" : "OTA disabled");
    html.replace("{{RTC_EN_CHECKED}}",   rtcEnabled ? "checked" : "");
    server.send(200, "text/html; charset=utf-8", html);
}

void handleConfig() {
    if (server.hasArg("url") && server.arg("url").length() > 0)
        { imageUrl = server.arg("url"); imageUrl.trim(); }
    if (server.hasArg("font"))
        selectedFont = server.arg("font");
    if (server.hasArg("location"))
        location = server.arg("location");
    if (server.hasArg("calendar") &&
        (server.arg("calendar") == "gregorian" || server.arg("calendar") == "jewish"))
        calendarType = server.arg("calendar");
    sleepEnabled = server.hasArg("sleep_en");
    rtcEnabled   = server.hasArg("rtc_en");
    if (server.hasArg("sleep_start") && server.arg("sleep_start").length() == 5)
        sleepStart = server.arg("sleep_start");
    if (server.hasArg("sleep_end") && server.arg("sleep_end").length() == 5)
        sleepEnd = server.arg("sleep_end");
    if (server.hasArg("interval") && server.arg("interval").toInt() > 0)
        refreshInterval = (uint32_t)server.arg("interval").toInt();
    if (server.hasArg("ota_pw")) {
        String pw = server.arg("ota_pw");
        pw.trim();
        if (pw == "-") otaPassword = "";
        else if (pw.length() >= 6) otaPassword = pw;
    }

    prefs.begin("epaper", false);
    prefs.putString("url",         imageUrl);
    prefs.putString("font",        selectedFont);
    prefs.putString("location",    location);
    prefs.putString("calendar",    calendarType);
    prefs.putBool("sleep_en",      sleepEnabled);
    prefs.putString("sleep_start", sleepStart);
    prefs.putString("sleep_end",   sleepEnd);
    prefs.putUInt("interval",      refreshInterval);
    prefs.putString("ota_pw",      otaPassword);
    prefs.putBool("rtc_en",        rtcEnabled);
    prefs.end();

    Serial.println("Config saved — restarting");
    server.sendHeader("Location", "/?saved");
    server.send(302);
    server.client().stop();
    delay(300);
    ESP.restart();
}

// ── PNG decode callback ───────────────────────────────

int pngDrawCallback(PNGDRAW *pDraw) {
    int y = pDraw->y;
    if (y < gPageY0 || y >= gPageY1) return 1;     // not in the page being drawn
    uint16_t lineRGB[PANEL_W];
    png.getLineAsRGB565(pDraw, lineRGB, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);
    for (int x = 0; x < PANEL_W; x++) {
        uint16_t p = lineRGB[x];
        uint8_t r = p >> 11, g = (p >> 5) & 0x3F, b = p & 0x1F;
        if (r >= 20 && g < 24 && b < 12)  display.drawPixel(x, y, GxEPD_RED);
        else if (p < 0x8000)              display.drawPixel(x, y, GxEPD_BLACK);
    }
    return 1;
}

// ── Image fetch + display ─────────────────────────────

static bool fail(const String& why) {
    gLastResult = why;
    gFailTotal++;
    Serial.println("FAIL: " + why);
    return false;
}

static bool fetchAndDisplayImpl() {
    String url = buildFetchUrl();
    Serial.printf("Fetching: %s\n", url.c_str());

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(10000);         // never block forever on a stalled connection
    http.setReuse(false);           // do not keep the connection (and its TLS memory) alive
    if (!http.begin(secureClient, url)) {
        return fail("HTTP begin failed (bad URL?)");
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return fail("HTTP " + String(code) + (code < 0 ? " " + HTTPClient::errorToString(code) : ""));
    }

    int contentLen = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    int pngLen = 0;

    if (contentLen > 0 && contentLen <= (int)PNG_BUF_SIZE) {
        // readBytes() gives up after 1 s of silence, which truncates the image on a slow or
        // weak Wi-Fi link. Keep reading until everything arrived or the link really stalls.
        uint32_t deadline = millis() + 15000;
        while (pngLen < contentLen && millis() < deadline) {
            int avail = stream->available();
            if (avail > 0) {
                int toRead = min(avail, contentLen - pngLen);
                pngLen  += stream->readBytes(pngBuf + pngLen, toRead);
                deadline = millis() + 5000;      // progress -> extend the stall timeout
            } else if (!http.connected()) {
                break;
            } else {
                delay(5);
            }
        }
    } else {
        uint32_t deadline = millis() + 10000;
        while ((http.connected() || stream->available()) && pngLen < (int)PNG_BUF_SIZE) {
            int avail = stream->available();
            if (avail > 0) {
                int toRead = min(avail, (int)PNG_BUF_SIZE - pngLen);
                pngLen   += stream->readBytes(pngBuf + pngLen, toRead);
                deadline  = millis() + 3000;
            } else if (millis() > deadline) {
                break;
            }
            delay(1);
        }
    }
    http.end();
    // Release the TLS session now so its buffers (~40 KB) are free again.
    secureClient.stop();
    Serial.printf("Downloaded %d bytes  (free heap %u, largest block %u)\n",
                  pngLen, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (pngLen == 0) return fail("empty download");
    if (contentLen > 0 && pngLen != contentLen) return fail("truncated download " + String(pngLen) + "/" + String(contentLen));

    int rc = png.openRAM(pngBuf, pngLen, pngDrawCallback);
    if (rc != PNG_SUCCESS) return fail("PNG open error " + String(rc) + " (server sent a non-PNG?)");
    if (png.getWidth() != PANEL_W || png.getHeight() != PANEL_H) {
        String w = "wrong size " + String(png.getWidth()) + "x" + String(png.getHeight());
        png.close(); return fail(w);
    }

    png.close();

    // Dry run: decode the whole PNG without drawing. If it is damaged we keep the old
    // picture on the panel instead of refreshing it with blank pages.
    gPageY0 = gPageY1 = -1;
    rc = png.openRAM(pngBuf, pngLen, pngDrawCallback);
    if (rc == PNG_SUCCESS) { rc = png.decode(NULL, 0); png.close(); }
    if (rc != PNG_SUCCESS) return fail("PNG decode error " + String(rc) + " (panel left unchanged)");

    // Paged drawing: the PNG stays in RAM (~5 KB) and is decoded once per display page
    // (33 rows each), so no 58 KB frame buffers are needed.
    const int pageH = display.pageHeight();
    int page = 0;
    bool decodeOk = true;
    display.setFullWindow();
    display.firstPage();
    do {
        gPageY0 = page * pageH;
        gPageY1 = gPageY0 + pageH;
        display.fillScreen(GxEPD_WHITE);
        rc = png.openRAM(pngBuf, pngLen, pngDrawCallback);
        if (rc == PNG_SUCCESS) { rc = png.decode(NULL, 0); png.close(); }
        if (rc != PNG_SUCCESS && decodeOk) {
            Serial.printf("PNG decode error: %d (page %d)\n", rc, page);
            decodeOk = false;
        }
        page++;
    } while (display.nextPage());     // last page triggers the full refresh + power off

    refreshCount++;
    Serial.printf("Display updated (full refresh, %d pages)\n", page);
    if (!decodeOk) return fail("PNG decode error while drawing");
    gLastResult = "ok";
    gLastOkMs   = millis();
    gOkCount++;
    return true;
}

bool fetchAndDisplay() {
    pngBuf = (uint8_t*)malloc(PNG_BUF_SIZE);
    if (!pngBuf) { Serial.println("Out of memory for PNG buffer"); return false; }
    // A flaky download or a server that is still waking up is usually fine on the next try,
    // so retry a few times before counting it as a failure.
    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        ok = fetchAndDisplayImpl();
        if (!ok && attempt < 3) { Serial.printf("Retry %d/2 in 3 s\n", attempt); delay(3000); }
    }
    free(pngBuf);
    pngBuf = nullptr;
    return ok;
}

// ── Self-recovery ─────────────────────────────────────
// A watchdog task restarts the board if loop() stops running (e.g. a hung connection),
// and loop() restarts it after repeated failures or a long Wi-Fi outage.
static volatile uint32_t gHeartbeat = 0;
static const uint32_t WATCHDOG_MS   = 300000;   // loop must tick at least every 5 min
static const int      MAX_FAILURES  = 5;        // consecutive failed fetches before restart
static const uint32_t WIFI_DOWN_MAX = 300000;   // restart after 5 min without Wi-Fi
static uint32_t wifiDownSince = 0;
static uint32_t lastReconnect = 0;

static void watchdogTask(void*) {
    for (;;) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        if (millis() - gHeartbeat > WATCHDOG_MS) {
            Serial.println("Watchdog: loop stuck -> restarting");
            delay(100);
            ESP.restart();
        }
    }
}

// ── Info screens ─────────────────────────────────────

static void showTextScreen(const String& title, const String& sub, const String& detail = "") {
    int16_t x1, y1;
    uint16_t w, h;
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.drawRect(8, 8, PANEL_W - 16, PANEL_H - 16, GxEPD_BLACK);

        display.setFont(&FreeSansBold18pt7b);
        display.getTextBounds(title.c_str(), 0, 0, &x1, &y1, &w, &h);
        display.setCursor((PANEL_W - w) / 2, 170);
        display.print(title);

        display.setFont(&FreeSans12pt7b);
        display.getTextBounds(sub.c_str(), 0, 0, &x1, &y1, &w, &h);
        display.setCursor((PANEL_W - w) / 2, 265);
        display.print(sub);

        if (detail.length()) {
            display.getTextBounds(detail.c_str(), 0, 0, &x1, &y1, &w, &h);
            display.setCursor((PANEL_W - w) / 2, 325);
            display.print(detail);
        }
    } while (display.nextPage());
}

// ── Setup ─────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting...");

    SPI.end();                                          // release the default SPI pins
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);     // map SPI to the driver board's pins
    display.init(115200, true, 2, false);
    display.setRotation(0);
    // After a software restart (config saved, watchdog) keep the current picture on the panel
    // instead of flashing it white and showing status screens.
    const bool quickBoot = (esp_reset_reason() == ESP_RST_SW);
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  gResetWhy = "power on";                         break;
        case ESP_RST_SW:       gResetWhy = "software restart (watchdog/config)"; break;
        case ESP_RST_BROWNOUT: gResetWhy = "BROWNOUT (power supply too weak)";   break;
        case ESP_RST_PANIC:    gResetWhy = "crash (panic)";                    break;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      gResetWhy = "hardware watchdog (stuck)";        break;
        default:               gResetWhy = "other";                            break;
    }
    Serial.printf("Reset reason: %s\n", gResetWhy);
    if (!quickBoot) {
        display.setFullWindow();
        display.firstPage();
        do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
    }
    Serial.println("ePaper initialized");

    WiFiManagerParameter paramUrl("image_url", "Image URL", "", 256);
    WiFiManagerParameter paramMin("refresh_min", "Refresh (seconds)", "60", 5);

    WiFiManager wm;
    wm.addParameter(&paramUrl);
    wm.addParameter(&paramMin);

    wm.setSaveParamsCallback([&]() {
        prefs.begin("epaper", false);
        String u = String(paramUrl.getValue());
        u.trim();                       // pasted URLs often carry stray spaces
        prefs.putString("url",    u);
        prefs.putUInt("interval", (uint32_t)atoi(paramMin.getValue()));
        prefs.end();
    });

    wm.setAPCallback([](WiFiManager *wm) {
        String ssid = wm->getConfigPortalSSID();
        String ip   = WiFi.softAPIP().toString();
        Serial.printf("AP started — SSID: %s  IP: %s\n", ssid.c_str(), ip.c_str());
        showTextScreen("Wi-Fi Setup",
                       "Connect to:  " + ssid,
                       "Then open:   " + ip);
    });

    if (!wm.autoConnect("EPaper-Setup")) {
        Serial.println("WiFi failed — restarting");
        ESP.restart();
    }
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    if (!quickBoot) {
        showTextScreen("Connected", WiFi.localIP().toString());
        delay(2000);
    }

    prefs.begin("epaper", true);
    imageUrl         = prefs.getString("url",         "");
    imageUrl.trim();
    refreshInterval  = prefs.getUInt("interval",      60);
    otaPassword      = prefs.getString("ota_pw",      "");
    selectedFont     = prefs.getString("font",        "Heebo-Bold");
    location         = prefs.getString("location",    "Tel Aviv");
    calendarType     = prefs.getString("calendar",    "gregorian");
    sleepEnabled     = prefs.getBool("sleep_en",      false);
    sleepStart       = prefs.getString("sleep_start", "22:00");
    sleepEnd         = prefs.getString("sleep_end",   "06:00");
    rtcEnabled       = prefs.getBool("rtc_en",        false);
    prefs.end();

    // Init RTC if enabled
    bool rtcAvailable = false;
    if (rtcEnabled) {
        if (rtc.begin()) {
            rtcAvailable = true;
            Serial.println("RTC found");
        } else {
            Serial.println("RTC not found — check wiring (SDA→GPIO21, SCL→GPIO22)");
        }
    }

    // Sync time via NTP (Israel timezone with automatic DST)
    configTzTime("IST-2IDT,M3.4.4/26,M10.5.0", "pool.ntp.org");
    Serial.print("NTP sync");
    struct tm ti;
    for (int i = 0; i < 10 && !getLocalTime(&ti, 1000); i++) Serial.print(".");
    bool ntpOk = getLocalTime(&ti, 0);
    Serial.println(ntpOk ? " OK" : " failed");

    if (rtcAvailable) {
        if (ntpOk) {
            // Keep RTC accurate by writing the fresh NTP time to it
            rtc.adjust(DateTime(time(nullptr)));
            Serial.println("RTC synced from NTP");
        } else if (!rtc.lostPower()) {
            // NTP unavailable — restore system clock from RTC
            struct timeval tv = { .tv_sec = (time_t)rtc.now().unixtime(), .tv_usec = 0 };
            settimeofday(&tv, nullptr);
            Serial.println("System time restored from RTC");
        } else {
            Serial.println("RTC lost power — time unreliable");
        }
    } else if (!ntpOk) {
        Serial.println("NTP failed and no RTC — sleep window unavailable");
    }

    Serial.printf("Font: %s  Location: %s\n", selectedFont.c_str(), location.c_str());
    Serial.printf("Sleep: %s  %s – %s\n",
                  sleepEnabled ? "on" : "off", sleepStart.c_str(), sleepEnd.c_str());
    Serial.printf("RTC: %s\n", rtcEnabled ? (rtcAvailable ? "enabled" : "enabled (not found)") : "disabled");

    server.on("/",       HTTP_GET,  handleRoot);
    server.on("/config", HTTP_POST, handleConfig);
    server.on("/status", HTTP_GET,  handleStatus);
    if (otaPassword.length() >= 6) {
        ArduinoOTA.setHostname("hebrew-clock");
        ArduinoOTA.setPassword(otaPassword.c_str());
        ArduinoOTA.onStart([]()  { Serial.println("OTA update starting"); });
        ArduinoOTA.onEnd([]()    { Serial.println("OTA update done, restarting"); });
        ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", (unsigned)e); });
        ArduinoOTA.begin();
        otaEnabled = true;
        Serial.println("OTA enabled: hostname hebrew-clock");
    }
    server.begin();
    Serial.printf("Config UI: http://%s/\n", WiFi.localIP().toString().c_str());

    gHeartbeat = millis();
    xTaskCreate(watchdogTask, "watchdog", 2048, nullptr, 1, nullptr);

    if (imageUrl.length() > 0) {
        if (!fetchAndDisplay()) failCount++;
    } else {
        Serial.println("No URL configured — open the config UI to set one");
    }
    lastRefresh = millis();
}

// ── Loop ──────────────────────────────────────────────

void loop() {
    gHeartbeat = millis();
    server.handleClient();
    if (otaEnabled) ArduinoOTA.handle();

    // Wi-Fi supervision: try to reconnect, restart if it stays down.
    if (WiFi.status() != WL_CONNECTED) {
        if (wifiDownSince == 0) wifiDownSince = millis();
        if (millis() - lastReconnect > 20000) {
            Serial.println("Wi-Fi down -> reconnecting");
            WiFi.reconnect();
            lastReconnect = millis();
        }
        if (millis() - wifiDownSince > WIFI_DOWN_MAX) {
            Serial.println("Wi-Fi down too long -> restarting");
            delay(100);
            ESP.restart();
        }
        delay(100);
        return;
    }
    wifiDownSince = 0;

    if (imageUrl.length() > 0 &&
        (millis() - lastRefresh) >= activeInterval() * 1000UL) {
        if (fetchAndDisplay()) {
            failCount = 0;
        } else if (++failCount >= MAX_FAILURES) {
            Serial.printf("%d failed fetches in a row -> restarting\n", failCount);
            delay(100);
            ESP.restart();
        }
        lastRefresh = millis();
    }
    delay(10);
}
