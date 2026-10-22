#include <GxEPD2_3C.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PNGdec.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <time.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <RTClib.h>

// ESP32 (classic) -> HINK-E075A07-A0 7.5" 880x528 B/W/R panel (SSD1677, GxEPD2_750c_Z90)
// Default hardware SPI pins: CLK=GPIO18, DIN/MOSI=GPIO23. Change the four below to match your wiring.
#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

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

    prefs.begin("epaper", false);
    prefs.putString("url",         imageUrl);
    prefs.putString("font",        selectedFont);
    prefs.putString("location",    location);
    prefs.putString("calendar",    calendarType);
    prefs.putBool("sleep_en",      sleepEnabled);
    prefs.putString("sleep_start", sleepStart);
    prefs.putString("sleep_end",   sleepEnd);
    prefs.putUInt("interval",      refreshInterval);
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

static void fetchAndDisplayImpl() {
    String url = buildFetchUrl();
    Serial.printf("Fetching: %s\n", url.c_str());

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    http.setReuse(false);           // do not keep the connection (and its TLS memory) alive
    if (!http.begin(secureClient, url)) {
        Serial.println("HTTP begin failed");
        return;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", code);
        http.end();
        return;
    }

    int contentLen = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    int pngLen = 0;

    if (contentLen > 0 && contentLen <= (int)PNG_BUF_SIZE) {
        pngLen = stream->readBytes(pngBuf, contentLen);
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
    if (pngLen == 0) return;

    int rc = png.openRAM(pngBuf, pngLen, pngDrawCallback);
    if (rc != PNG_SUCCESS) { Serial.printf("PNG open error: %d\n", rc); return; }
    if (png.getWidth() != PANEL_W || png.getHeight() != PANEL_H) {
        Serial.printf("Wrong dimensions: %dx%d\n", png.getWidth(), png.getHeight());
        png.close(); return;
    }

    png.close();

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
}

void fetchAndDisplay() {
    pngBuf = (uint8_t*)malloc(PNG_BUF_SIZE);
    if (!pngBuf) { Serial.println("Out of memory for PNG buffer"); return; }
    fetchAndDisplayImpl();
    free(pngBuf);
    pngBuf = nullptr;
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

    display.init(115200, true, 2, false);
    display.setRotation(0);
    display.setFullWindow();
    display.firstPage();
    do { display.fillScreen(GxEPD_WHITE); } while (display.nextPage());
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

    showTextScreen("Connected", WiFi.localIP().toString());
    delay(2000);

    prefs.begin("epaper", true);
    imageUrl         = prefs.getString("url",         "");
    imageUrl.trim();
    refreshInterval  = prefs.getUInt("interval",      60);
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
    server.begin();
    Serial.printf("Config UI: http://%s/\n", WiFi.localIP().toString().c_str());

    if (imageUrl.length() > 0) {
        fetchAndDisplay();
    } else {
        Serial.println("No URL configured — open the config UI to set one");
    }
    lastRefresh = millis();
}

// ── Loop ──────────────────────────────────────────────

void loop() {
    server.handleClient();

    if (imageUrl.length() > 0 &&
        (millis() - lastRefresh) >= activeInterval() * 1000UL) {
        fetchAndDisplay();
        lastRefresh = millis();
    }
    delay(10);
}
