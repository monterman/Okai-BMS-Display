// WiFiServer.ino — WiFi AP + live web dashboard + log management
//
// BTN1 (GPIO0) toggles the AP on/off.
// Connect to "OkaiBMS" / 12345678 then open http://192.168.4.1
//
// Routes:
//   GET /            — live pack data + log file list (auto-refreshes 5 s)
//   GET /settime?t=N — set RTC from browser Date.now() (epoch ms) — called by JS
//   GET /csv?f=NAME  — download a specific log file
//   GET /delete?f=NAME — delete a log file
//   GET /clearall    — delete ALL log files
//   GET /rawdump     — hex dump of raw 36-byte BMS frame (bench test / UID hunt)

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

bool wifiActive = false;
static WebServer _srv(80);
extern bool fsReady;

// Forward declaration — RuipuBattery instances are in UART.ino
#include "RuipuBattery.h"
extern RuipuBattery pack[NUM_PACKS];

// ── Helpers ───────────────────────────────────────────────────────────────────
static bool isCsvFile(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".csv") == 0;
}

// Basic path sanity — must start with / and contain no ".."
static bool safePath(const String &name) {
    return name.length() > 0 &&
           name.charAt(0) == '/' &&
           name.indexOf("..") < 0;
}

// ── Route: / ─────────────────────────────────────────────────────────────────
static void handleRoot() {
    String h;
    h.reserve(4096);

    // ── head
    h += F("<!DOCTYPE html><html><head>"
           "<meta charset='utf-8'><title>Okai BMS</title>"
           "<meta http-equiv='refresh' content='5'>"
           "<script>fetch('/settime?t='+Date.now());</script>"
           "<style>"
           "body{background:#0d1117;color:#e0e0e0;font-family:monospace;padding:16px}"
           "h2,h3{color:#4af;margin:8px 0 4px}"
           "p{color:#888;margin:4px 0}"
           "table{border-collapse:collapse;width:100%;margin:8px 0}"
           "th{background:#161b22;padding:6px;border:1px solid #333;color:#aaa}"
           "td{padding:6px;border:1px solid #222;text-align:center}"
           ".good{color:#00e676}.warn{color:#ffee00}.poor{color:#ff4444}"
           ".chrg{color:#00bcd4}.dim{color:#555}"
           "a{color:#4af} .btn{background:#161b22;border:1px solid #333;"
           "color:#aaa;padding:4px 10px;text-decoration:none;border-radius:4px}"
           "</style></head><body>");

    // ── header
    h += "<h2>Okai BMS " FW_VERSION "</h2>";

    uint32_t s = millis() / 1000;
    char up[12];
    snprintf(up, sizeof(up), "%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);

    char rtcStr[32];
    if (timeIsSynced()) {
        time_t t = timeNowSec();
        struct tm tm_info;
        gmtime_r(&t, &tm_info);
        snprintf(rtcStr, sizeof(rtcStr), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                 tm_info.tm_year+1900, tm_info.tm_mon+1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else {
        strcpy(rtcStr, "NOT SET — open this page to sync");
    }

    h += "<p>Uptime: "; h += up;
    h += " &nbsp; IP: "; h += WiFi.softAPIP().toString();
    h += " &nbsp; RTC: "; h += rtcStr;

    const char *modeStr = (logCurrentMode() == LOG_RIDE)   ? "&#128694; RIDE" :
                          (logCurrentMode() == LOG_CHARGE) ? "&#9889; CHARGE" : "IDLE";
    h += " &nbsp; Log: <b>"; h += modeStr; h += "</b></p>";

    // ── pack table
    h += F("<h3>Pack status</h3>"
           "<table><tr>"
           "<th>Pack</th><th>SOC</th><th>Voltage</th><th>Current</th><th>Power</th>"
           "<th>Avail.Wh</th><th>Cell&Delta;</th><th>Temp</th><th>Cycles</th><th>Health</th>"
           "</tr>");

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        h += "<tr>";
        if (!packs[i].valid) {
            h += "<td>"; h += lbl; h += "</td>";
            h += "<td colspan='9' class='dim'>no data</td></tr>";
            continue;
        }
        float delta   = packs[i].cellHigh - packs[i].cellLow;
        float powerW  = packs[i].voltage * packs[i].current;
        float availWh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
        const char *cls  = (delta >= CELL_DELTA_POOR_V) ? "poor" :
                           (delta >= CELL_DELTA_WARN_V) ? "warn" : "good";
        const char *htag = (delta >= CELL_DELTA_POOR_V) ? "POOR" :
                           (delta >= CELL_DELTA_WARN_V) ? "WARN" : "GOOD";
        char row[320];
        snprintf(row, sizeof(row),
            "<td>%s</td><td>%u%%</td><td>%.2fV</td><td>%+.2fA</td>"
            "<td>%+.0fW</td><td>%.0f Wh</td><td>%u mV</td>"
            "<td>%u&#176;C</td><td>%u</td><td class='%s'>%s</td></tr>",
            lbl, (unsigned)packs[i].soc, packs[i].voltage, packs[i].current,
            powerW, availWh,
            (unsigned)(delta * 1000.0f + 0.5f),
            (unsigned)packs[i].maxTemp, (unsigned)packs[i].cycles,
            cls, htag);
        h += row;
    }
    h += F("</table>");

    // Session Wh summary
    h += F("<p><b>Session energy:</b> ");
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        char e[40];
        snprintf(e, sizeof(e), "L%s +%.1f/&#8722;%.1f Wh &nbsp; ",
                 lbl, packs[i].whIn, packs[i].whOut);
        h += e;
    }
    h += F("</p>");

    // ── log file list
    h += F("<h3>Log files</h3>");
    if (!fsReady) {
        h += F("<p class='poor'>Filesystem not ready</p>");
    } else {
        h += F("<table><tr><th>File</th><th>Size</th><th>Download</th><th>Delete</th></tr>");
        File root = LittleFS.open("/");
        File f    = root.openNextFile();
        bool any  = false;
        while (f) {
            if (isCsvFile(f.name())) {
                any = true;
                char row[256];
                // f.name() returns just the base name without leading /
                snprintf(row, sizeof(row),
                    "<tr><td>%s</td><td>%u KB</td>"
                    "<td><a class='btn' href='/csv?f=/%s'>&#128229;</a></td>"
                    "<td><a class='btn' href='/delete?f=/%s'>&#128465;</a></td></tr>",
                    f.name(), (unsigned)(f.size() / 1024),
                    f.name(), f.name());
                h += row;
            }
            f = root.openNextFile();
        }
        root.close();
        if (!any) h += F("<tr><td colspan='4' class='dim'>No log files yet</td></tr>");
        h += F("</table>");
        h += F("<p><a class='btn' href='/clearall'>&#9888; Delete all logs</a>"
               " &nbsp; <a class='btn' href='/rawdump'>&#128270; Raw frame dump</a></p>");

        // Filesystem usage
        char fs[64];
        snprintf(fs, sizeof(fs), "Storage: %u KB used / %u KB total",
                 (unsigned)(LittleFS.usedBytes()/1024),
                 (unsigned)(LittleFS.totalBytes()/1024));
        h += "<p class='dim'>"; h += fs; h += "</p>";
    }

    h += F("<p class='dim'>BTN2/BTN3=screens &nbsp; BTN1=WiFi toggle &nbsp;"
           "Hold BTN3 on screen&nbsp;0 to assign pack labels<br>"
           "Design ref: 460.8 Wh (NCR18650BD 10S4P) &nbsp;"
           "Delta thresholds: warn=50mV poor=100mV</p>"
           "</body></html>");

    _srv.send(200, "text/html", h);
}

// ── Route: /settime ───────────────────────────────────────────────────────────
static void handleSetTime() {
    if (_srv.hasArg("t")) {
        int64_t epochMs = (int64_t)_srv.arg("t").toDouble();
        timeSyncSet(epochMs);
        _srv.send(200, "text/plain", "OK");
    } else {
        _srv.send(400, "text/plain", "Missing t");
    }
}

// ── Route: /csv?f=/NAME.csv ───────────────────────────────────────────────────
static void handleCsv() {
    String fname = _srv.hasArg("f") ? _srv.arg("f") : String("/bms_log.csv");
    if (!safePath(fname) || !fsReady || !LittleFS.exists(fname)) {
        _srv.send(404, "text/plain", "Not found");
        return;
    }
    File f = LittleFS.open(fname, "r");
    if (!f) { _srv.send(500, "text/plain", "Open failed"); return; }
    String disp = "attachment; filename=\"" + fname.substring(1) + "\"";
    _srv.sendHeader("Content-Disposition", disp);
    _srv.streamFile(f, "text/csv");
    f.close();
}

// ── Route: /delete?f=/NAME.csv ────────────────────────────────────────────────
static void handleDelete() {
    String fname = _srv.hasArg("f") ? _srv.arg("f") : String();
    if (!safePath(fname) || !fsReady) {
        _srv.send(400, "text/plain", "Bad request");
        return;
    }
    LittleFS.remove(fname);
    _srv.sendHeader("Location", "/");
    _srv.send(302, "text/plain", "Deleted");
}

// ── Route: /clearall ──────────────────────────────────────────────────────────
static void handleClearAll() {
    if (fsReady) {
        File root = LittleFS.open("/");
        File f    = root.openNextFile();
        while (f) {
            if (isCsvFile(f.name())) {
                String p = String("/") + f.name();
                LittleFS.remove(p);
            }
            f = root.openNextFile();
        }
        root.close();
        Serial.println("[WiFi] all logs cleared");
    }
    _srv.sendHeader("Location", "/");
    _srv.send(302, "text/plain", "Cleared");
}

// ── Route: /rawdump — hex dump of all pack frames (bench test / UID hunt) ─────
static void handleRawDump() {
    String h;
    h.reserve(1024);
    h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta http-equiv='refresh' content='3'>"
           "<title>Raw Frame Dump</title>"
           "<style>body{background:#0d1117;color:#e0e0e0;font-family:monospace;"
           "padding:16px}h2{color:#4af}pre{background:#161b22;padding:12px;"
           "border-radius:4px;overflow-x:auto}</style></head><body>");
    h += F("<h2>Raw 36-byte BMS frames (refreshes every 3 s)</h2>");
    h += F("<p>Compare frames from different packs in the same port to find UID bytes.</p><pre>");

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        char lbl[6]; labelStr(i, lbl, sizeof(lbl));
        char line[128];
        snprintf(line, sizeof(line), "Port %u (L%s) — %s\n",
                 i+1, lbl, packs[i].valid ? "valid" : "NO DATA");
        h += line;
        if (packs[i].valid) {
            const byte *buf = pack[i].buf();
            for (uint8_t b = 0; b < 36; b++) {
                char hex[8];
                snprintf(hex, sizeof(hex), "%02X ", buf[b]);
                h += hex;
                if (b == 17) h += "\n          ";   // wrap at byte 18
            }
            h += "\n     byte: ";
            for (uint8_t b = 0; b < 36; b++) {
                char idx[8];
                snprintf(idx, sizeof(idx), "%02u ", b);
                h += idx;
                if (b == 17) h += "\n           ";
            }
            h += "\n\n";
        }
    }

    h += F("</pre><p><a href='/'>&#8592; Back to dashboard</a></p>"
           "</body></html>");
    _srv.send(200, "text/html", h);
}

static void handleNotFound() {
    _srv.send(404, "text/plain", "Not found");
}

// ── Public API ────────────────────────────────────────────────────────────────
void wifiServerInit() {
    _srv.on("/",         handleRoot);
    _srv.on("/settime",  handleSetTime);
    _srv.on("/csv",      handleCsv);
    _srv.on("/delete",   handleDelete);
    _srv.on("/clearall", handleClearAll);
    _srv.on("/rawdump",  handleRawDump);
    _srv.onNotFound(handleNotFound);
}

void wifiServerLoop() {
    if (wifiActive) _srv.handleClient();
}

void wifiToggle() {
    if (wifiActive) {
        _srv.stop();
        WiFi.softAPdisconnect(true);
        wifiActive = false;
        Serial.println("[WiFi] AP off");
    } else {
        WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
        _srv.begin();
        wifiActive = true;
        Serial.printf("[WiFi] AP on  SSID=%s  IP=%s\n",
                      WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    }
}
