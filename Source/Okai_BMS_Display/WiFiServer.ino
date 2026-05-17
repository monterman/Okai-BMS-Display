// WiFiServer.ino — WiFi AP + live web dashboard + CSV download
//
// BTN1 (GPIO0) toggles the AP on/off.
// Connect to "OkaiBMS" / 12345678 then open http://192.168.4.1
//
// Routes:
//   GET /       — live pack data table (auto-refreshes every 5 s)
//   GET /csv    — download log file as CSV
//   GET /clear  — wipe log file and redirect to /

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

bool wifiActive = false;

static WebServer _srv(80);
extern bool fsReady;    // Logger.ino

// ── Route handlers ────────────────────────────────────────────────
static void handleRoot() {
    // Build HTML — kept in one pass to avoid large String appends
    String h;
    h.reserve(3200);

    h += F("<!DOCTYPE html><html><head>"
           "<meta charset='utf-8'><title>Okai BMS</title>"
           "<meta http-equiv='refresh' content='5'>"
           "<style>"
           "body{background:#0d1117;color:#e0e0e0;font-family:monospace;padding:16px}"
           "h2{color:#4af;margin:0 0 8px}"
           "p{color:#666;margin:4px 0}"
           "table{border-collapse:collapse;width:100%;margin:12px 0}"
           "th{background:#161b22;padding:8px;border:1px solid #333;color:#aaa}"
           "td{padding:8px;border:1px solid #222;text-align:center}"
           ".good{color:#00e676}.warn{color:#ffee00}.poor{color:#ff4444}"
           ".dim{color:#555} a{color:#4af}"
           "</style></head><body>");

    h += "<h2>Okai BMS Display " FW_VERSION "</h2>";

    uint32_t s = millis() / 1000;
    char up[12];
    snprintf(up, sizeof(up), "%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);
    h += "<p>Uptime: "; h += up;
    h += " &nbsp; WiFi IP: "; h += WiFi.softAPIP().toString(); h += "</p>";

    h += F("<table><tr>"
           "<th>Pack</th><th>SOC</th><th>Voltage</th><th>Current</th><th>Power</th>"
           "<th>Avail.Wh</th><th>Cell&Delta;</th><th>Temp</th><th>Cycles</th><th>Health</th>"
           "</tr>");

    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        h += "<tr>";
        if (!packs[i].valid) {
            h += "<td>P"; h += (i+1); h += "</td>";
            h += "<td colspan='9' class='dim'>no data</td></tr>";
            continue;
        }
        float delta  = packs[i].cellHigh - packs[i].cellLow;
        float powerW = packs[i].voltage * packs[i].current;
        float availWh = (packs[i].soc / 100.0f) * PACK_DESIGN_WH;
        const char *cls  = (delta >= CELL_DELTA_POOR_V) ? "poor" :
                           (delta >= CELL_DELTA_WARN_V) ? "warn" : "good";
        const char *htag = (delta >= CELL_DELTA_POOR_V) ? "POOR" :
                           (delta >= CELL_DELTA_WARN_V) ? "WARN" : "GOOD";
        char row[320];
        snprintf(row, sizeof(row),
            "<td>P%u</td>"
            "<td>%u%%</td>"
            "<td>%.2fV</td>"
            "<td>%+.2fA</td>"
            "<td>%+.0fW</td>"
            "<td>%.0f Wh</td>"
            "<td>%u mV</td>"
            "<td>%u&#176;C</td>"
            "<td>%u</td>"
            "<td class='%s'>%s</td></tr>",
            (unsigned)(i+1),
            (unsigned)packs[i].soc,
            packs[i].voltage,
            packs[i].current,
            powerW,
            availWh,
            (unsigned)(delta * 1000.0f + 0.5f),
            (unsigned)packs[i].maxTemp,
            (unsigned)packs[i].cycles,
            cls, htag);
        h += row;
    }

    h += F("</table>");

    // Session energy summary
    h += F("<p><b>Session energy (since boot):</b> ");
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        if (!packs[i].valid) continue;
        char e[48];
        snprintf(e, sizeof(e), "P%u +%.1f/&#8722;%.1f Wh &nbsp; ",
                 i+1, packs[i].whIn, packs[i].whOut);
        h += e;
    }
    h += F("</p>");

    h += F("<p><a href='/csv'>&#128229; Download log CSV</a>"
           " &nbsp; <a href='/clear'>&#128465; Clear log</a></p>"
           "<p class='dim'>BTN2/BTN3 = screens &nbsp; BTN1 = WiFi toggle &nbsp;"
           "Design: 460.8 Wh (NCR18650BD 10S4P) &nbsp;"
           "Delta thresholds: warn=50mV poor=100mV</p>"
           "</body></html>");

    _srv.send(200, "text/html", h);
}

static void handleCsv() {
    if (!fsReady || !LittleFS.exists(LOG_FILENAME)) {
        _srv.send(404, "text/plain", "No log file yet");
        return;
    }
    File f = LittleFS.open(LOG_FILENAME, "r");
    if (!f) { _srv.send(500, "text/plain", "Open failed"); return; }
    _srv.sendHeader("Content-Disposition",
                    "attachment; filename=\"bms_log.csv\"");
    _srv.streamFile(f, "text/csv");
    f.close();
}

static void handleClear() {
    if (fsReady) {
        LittleFS.remove(LOG_FILENAME);
        File f = LittleFS.open(LOG_FILENAME, "w", true);
        if (f) { f.print(LOG_CSV_HEADER); f.close(); }
        Serial.println("[WiFi] log cleared");
    }
    _srv.sendHeader("Location", "/");
    _srv.send(302, "text/plain", "Cleared");
}

static void handleNotFound() {
    _srv.send(404, "text/plain", "Not found");
}

// ── Public API ────────────────────────────────────────────────────
void wifiServerInit() {
    _srv.on("/",      handleRoot);
    _srv.on("/csv",   handleCsv);
    _srv.on("/clear", handleClear);
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
                      WIFI_AP_SSID,
                      WiFi.softAPIP().toString().c_str());
    }
}
