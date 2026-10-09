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

#include <Preferences.h>
#include <ESPmDNS.h>
#include <WiFiMulti.h>
#include <stdarg.h>       // va_list / vsnprintf — see _htmlAppend(). Arduino.h happens to
                          // pull this in transitively today; an undeclared dependency on a
                          // transitive include is exactly what breaks on a core upgrade.
// <esp_mac.h> removed 2026-10-09: its only user was the AP SSID's MAC suffix, which the
// owner reverted. Dead includes are how a comment outlives the code it described.

bool wifiActive = false;                // true whenever the dashboard is reachable (STA or AP)
static WebServer _srv(80);
static char      _csrfToken[9] = {0};   // HIGH-1: per-boot random token
extern bool fsReady;

// ── Station mode state ────────────────────────────────────────────────────────
// See Config.h § "Station mode" for why this exists and why it never runs while riding.
// Credentials are in NVS only; they are never written to source and they survive a flash.
enum WifiState : uint8_t {
    WST_OFF = 0,
    WST_STATION,   // on the home network
    WST_AP         // SoftAP fallback, or no credentials stored
};
static WifiState _wst          = WST_OFF;
static uint32_t  _wstSince     = 0;      // when we entered JOINING
static uint32_t  _staRetryAt   = 0;      // do not re-attempt a failed join before this
static uint32_t  _staLostSince = 0;      // S-7: link first seen down — debounce, not a hair trigger
static bool      _apNoEvidence = false;  // F-1: this AP is a fallback, not a decision — retry STA
                                         // once pack frames finally arrive
static bool      _hadStation   = false;  // a station link succeeded at least once this boot
static uint8_t   _staRelostTries = 0;    // consecutive no-charger re-join attempts
static uint32_t  _lastReqMs    = 0;      // G-3: last HTTP request served — tells an idle
                                         // auto-joined phone from someone actually working
static bool      _userForcedOff = false; // a manual BTN1 off must not be overridden by auto-on
static bool      _mdnsUp       = false;
// 2026-10-06 - A LIST of networks, not one, and WiFiMulti to choose between them.
// Ported from foilIQ's WifiXfer.ino, which already solves this on the owner's own hardware.
// WiFiMulti scans and joins the STRONGEST stored network actually in range and carries
// per-network failover itself — which is exactly the garage-is-far-from-the-house case.
typedef struct { char ssid[33]; char pass[65]; } WifiNet;
static WifiNet      _nets[WIFI_MAX_NETS];
static uint8_t      _netCount = 0;
static WiFiMulti    _multi;
static char         _apSsid[24] = {0};

static void wifiNetsLoad() {
    Preferences p;
    _netCount = 0;
    if (!p.begin("okaiwifi", true)) return;        // absent namespace is normal on a new board
    for (uint8_t i = 0; i < WIFI_MAX_NETS; i++) {
        char k[8];
        snprintf(k, sizeof(k), "ssid%u", i);
        String s = p.getString(k, "");
        if (!s.length()) continue;
        snprintf(k, sizeof(k), "pass%u", i);
        String q = p.getString(k, "");
        snprintf(_nets[_netCount].ssid, sizeof(_nets[0].ssid), "%s", s.c_str());
        snprintf(_nets[_netCount].pass, sizeof(_nets[0].pass), "%s", q.c_str());
        _netCount++;
    }
    p.end();
}

static bool wifiNetsSave() {
    Preferences p;
    if (!p.begin("okaiwifi", false)) return false;
    for (uint8_t i = 0; i < WIFI_MAX_NETS; i++) {
        char k[8];
        snprintf(k, sizeof(k), "ssid%u", i);
        if (i < _netCount) p.putString(k, _nets[i].ssid); else p.remove(k);
        snprintf(k, sizeof(k), "pass%u", i);
        if (i < _netCount) p.putString(k, _nets[i].pass); else p.remove(k);
    }
    p.end();
    return true;
}

// Add, or update the password of an SSID already stored. Oldest entry is dropped when full.
static bool wifiNetAdd(const char* ssid, const char* pass) {
    uint8_t slot = _netCount;
    for (uint8_t i = 0; i < _netCount; i++)
        if (strcmp(_nets[i].ssid, ssid) == 0) { slot = i; break; }
    if (slot == WIFI_MAX_NETS) {                   // full: drop the oldest
        memmove(&_nets[0], &_nets[1], (WIFI_MAX_NETS - 1) * sizeof(WifiNet));
        slot = WIFI_MAX_NETS - 1;
        _netCount = WIFI_MAX_NETS;
    } else if (slot == _netCount) {
        _netCount++;
    }
    snprintf(_nets[slot].ssid, sizeof(_nets[0].ssid), "%s", ssid);
    snprintf(_nets[slot].pass, sizeof(_nets[0].pass), "%s", pass);
    return wifiNetsSave();
}

// ── Clamped HTML append ──────────────────────────────────────────────────────
// 2026-10-09 - WHY THIS EXISTS. snprintf returns the length it WOULD have written, not
// the length it wrote. So `used += snprintf(buf + used, cap - used, ...)` walks the cursor
// PAST the end of the buffer the moment anything truncates — and because the size argument
// is unsigned, `cap - used` then underflows to an enormous value. The next call gets an
// out-of-bounds pointer and an effectively unbounded limit: stack corruption, on a page
// served over HTTP.
//
// This is R-5 from the 2026-10-06 audit ("clamp the snprintf return before the memcpy",
// in packRegistryList). I wrote the same bug again three days later in the /wifi list
// builder. So it is a helper now rather than an idiom to re-type: the only cursor
// arithmetic in this file happens in here, clamped to what was ACTUALLY written.
static void _htmlAppend(char* buf, size_t cap, size_t* used, const char* fmt, ...) {
    if (!buf || !used || cap == 0 || *used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[*used] = '\0'; return; }  // encoding error: terminate, cursor still
    const size_t room = cap - *used - 1;       // -1 preserves room for the NUL
    *used += ((size_t)n > room) ? room : (size_t)n;
}

// Did the last build truncate? The OTHER half of R-5 was silent truncation, and the fix
// there was a visible banner (packRegistryList's red <tr>). Same treatment here: a short
// page is only safe if it admits to being short.
static inline bool _htmlTruncated(size_t used, size_t cap) { return used + 1 >= cap; }

// NOT ESCAPING SSIDs INTO THE /wifi ATTRIBUTES — a deliberate omission, 2026-10-09.
// The name goes into value='...' and comes back as the form value identifying which network
// to forget, so an apostrophe would end the attribute early and the delete would silently
// do nothing (audit MEDIUM, graded post-flash). iOS does default its hotspot to
// "<Name>'s iPhone", so I started writing the escape — then the owner said plainly that his
// networks contain no apostrophes. Taking him at his word: escaping would have grown the
// row budget by ~2x and the page buffers with it, on the very commit that ships, which is
// the pattern that produced today's two regressions. The failure mode if a name like that
// ever IS added is narrow and non-destructive: add, save and join all work; only `forget`
// on that one entry no-ops, with "not stored, nothing done" on serial. Escape it then.

bool wifiHasCreds() { return _netCount > 0; }

// Delete one stored network, BY NAME. Needed because the setup page used to be write-only:
// there was no way to see what was saved, let alone remove a typo, short of a full reflash.
//
// 2026-10-09 (G-2) - by name, NOT by slot index, and the difference is a silent wrong
// delete. Forgetting shifts every later entry down, so an index rendered into the page goes
// stale the instant anything is removed: with three saved, tapping forget on slot 0 and then
// tapping slot 1 from a page that has not reloaded deletes a DIFFERENT network than the one
// the button was next to. A double-tap on a slow phone does the same. It also failed quietly
// about half the time, because a stale index >= _netCount is simply a no-op - worse than
// failing loudly. And String::toInt() returns 0 for anything unparseable, which is
// indistinguishable from a legitimate "forget slot 0", so a garbled form value deleted the
// first network. Matching on the SSID closes all three at once: the name identifies the
// same thing before and after any shift, and an unknown name deletes nothing.
static bool wifiNetForget(const char* ssid) {
    if (!ssid || !ssid[0]) return false;
    uint8_t slot = _netCount;
    for (uint8_t i = 0; i < _netCount; i++)
        if (strcmp(_nets[i].ssid, ssid) == 0) { slot = i; break; }
    if (slot >= _netCount) {
        Serial.printf("[WiFi] forget \"%s\" - not stored, nothing done\n", ssid);
        return false;
    }
    for (uint8_t i = slot; i + 1 < _netCount; i++) _nets[i] = _nets[i + 1];
    _netCount--;
    memset(&_nets[_netCount], 0, sizeof(WifiNet));
    Serial.printf("[WiFi] forgot \"%s\" (%u left)\n", ssid, _netCount);
    return wifiNetsSave();
}

// What the header and the diag line should say.
const char* wifiStateStr() {
    switch (_wst) {
        case WST_STATION: return "STA";
        case WST_AP:      return "AP";
        default:          return "off";
    }
}

static void _mdnsStart() {
    if (_mdnsUp) return;
    if (MDNS.begin(WIFI_MDNS_NAME)) {
        MDNS.addService("http", "tcp", 80);
        _mdnsUp = true;
        Serial.printf("[WiFi] mDNS up — http://%s.local\n", WIFI_MDNS_NAME);
    }
}

static void _serverUp() {
    if (!wifiActive) { _srv.begin(); wifiActive = true; }
    _mdnsStart();
}

static void _wifiAllDown() {
    if (wifiActive) _srv.stop();
    MDNS.end();
    _mdnsUp = false;
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, false);        // drop the association, keep NVS creds
    WiFi.mode(WIFI_OFF);
    wifiActive = false;
    _wst = WST_OFF;
    // Hygiene: clear the debounce timer here rather than leaving it stale across a
    // teardown. Benign either way (a stale value only skips one debounce), but a timer
    // that outlives the state it describes is how the G-4 class of bug starts.
    _staLostSince = 0;
}

static void _wifiStartAP() {
    WiFi.mode(WIFI_AP);
    // 2026-10-09 - OWNER'S DECISION: the AP name and password stay EXACTLY as they have
    // always been - "OkaiBMS" / "12345678". A MAC suffix was briefly added on audit advice
    // (so his phone would not auto-join it); he has overruled that, and he is right about
    // the priority: this AP is his guaranteed way back into the box when nothing else
    // works. A name he already knows beats a name he has to look up, and the password has
    // to stay predictable for the same reason. Accepted trade: his phone may auto-join,
    // and a second board (the planned dock) would clash on the name - deal with that when
    // a second board exists, by changing THAT board.
    snprintf(_apSsid, sizeof(_apSsid), "%s", WIFI_AP_SSID);
    WiFi.softAP(_apSsid, WIFI_AP_PASSWORD);        // WPA2, never open — the AP exposes every log
    _wst = WST_AP;
    // 2026-10-09 - S-2: _wstSince was set only by _wifiStartAuto(), so every path that
    // reaches the AP directly left a STALE timestamp and the window read as already
    // expired. The next loop pass tore the AP down. That killed the one route to /wifi,
    // which is the ONLY way to enter home credentials on a board that has none — so the
    // documented first-boot setup could never have worked. Set it where the AP comes up.
    _wstSince = millis();
    _serverUp();
    Serial.printf("[WiFi] AP on  SSID=%s  IP=%s\n",
                  _apSsid, WiFi.softAPIP().toString().c_str());
}

// ── Station attempt — SEQUENTIAL, never concurrent with the AP ────────────────
// Why not AP+STA at the same time, which was the obvious-looking design: WIFI_MODE_APSTA
// allocates both control blocks, and Espressif's own docs and issue tracker put station
// mode alone at ~45 kB of heap with APSTA "a lot of RAM". This board has NO PSRAM, so the
// ~106 kB framebuffer already sits in internal DRAM, and an out-of-memory reboot is exactly
// Rex's K-7 — a reboot stops the keep-alive. foilIQ avoids APSTA for the same reason and has
// been proven in the field. One radio at a time.
//
// `_multi.run()` BLOCKS for up to the budget. That is acceptable and deliberate: the
// keep-alive is a Core-1 priority-20 task so it keeps beating throughout, and the only cost
// is that pack reads and the display pause. At boot there is nothing to log yet; on a
// charge-start attempt the sample interval is 30 s, so at worst one row is late.
static bool _wifiTryStation() {
    if (_netCount == 0) {
        Serial.println("[WiFi] no stored networks — going straight to AP");
        return false;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // TWO naming mechanisms, because one is not enough — foilIQ's note, and it is right:
    //   setHostname() is the DHCP client name, so the ROUTER learns it. That makes a bare
    //     "okai" resolve (or okai.lan / okai.home depending on the router) and shows a
    //     readable name in the client list instead of an anonymous MAC. It MUST be set
    //     before the association, which is why it is here and not after run().
    //   mDNS (started in _mdnsStart) answers okai.local, which Windows and Apple resolve
    //     natively and Android frequently does not.
    WiFi.setHostname(WIFI_MDNS_NAME);
    _multi.APlistClean();
    for (uint8_t i = 0; i < _netCount; i++)
        _multi.addAP(_nets[i].ssid, _nets[i].pass[0] ? _nets[i].pass : nullptr);

    Serial.printf("[WiFi] scanning for %u known network(s), %lu ms budget...\n",
                  _netCount, (unsigned long)WIFI_STA_CONNECT_MS);
    if (_multi.run(WIFI_STA_CONNECT_MS) == WL_CONNECTED) {
        _wst = WST_STATION;
        // A link has existed this boot, so a later drop is worth retrying even with no
        // charger attached — see the re-join branch in wifiServerLoop(). Reset the attempt
        // counter: this join worked, so the next drop gets a full allowance again.
        _hadStation      = true;
        _staRelostTries  = 0;
        _serverUp();
        Serial.printf("[WiFi] joined \"%s\"  IP=%s  http://%s.local  RSSI=%d dBm\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                      WIFI_MDNS_NAME, WiFi.RSSI());
        return true;
    }
    Serial.println("[WiFi] no known network answered — falling back to AP");
    WiFi.disconnect(true);
    return false;
}

// Declared above its first use: _wifiStartAuto() retires it for every caller (G-1).
static bool _bootJoinPending = false;

// Try the home network, fall back to the AP. Used at boot, at charge start, and by BTN1.
static void _wifiStartAuto() {
    // 2026-10-09 (G-1) - RETIRE THE BOOT JOIN HERE, for every caller.
    // Without this: power up, press Join (or BTN1) at t=20 s, get a working station link,
    // and then at t=30 s the still-armed boot-join deadline fires _wifiStartAP(), whose
    // WiFi.mode(WIFI_AP) drops the station interface. The owner pressed Join, the page said
    // "joining", and okai.local never answers. It belongs in here rather than in each
    // caller because all three paths - the boot join itself, wifiToggle() and the /wifi
    // Join button - mean the same thing: a join decision has now been made, so the deferred
    // one is spent.
    _bootJoinPending = false;
    _wstSince = millis();
    if (!_wifiTryStation()) _wifiStartAP();
}

// Called once from setup(). Separate from wifiToggle() so a boot attempt is never mistaken
// for a manual one: _userForcedOff stays clear, so the charge-time retry still works even if
// this attempt finds nothing.
// R-2: do NOT join from setup(). A reset mid-ride would raise the radio exactly where
// SOP-038 forbids it, and mid-session reboots are PROVEN on this unit. Arm a request here
// and let wifiServerLoop() honour it once it can see whether a ride is in progress.
void wifiStartBoot() {
    _userForcedOff    = false;
    _staRetryAt       = 0;
    _bootJoinPending  = true;
}

// OkaiBMS instances are in UART.ino
#include "OkaiBMS.h"
extern OkaiBMS pack[NUM_PACKS];

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
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    String h;
    h.reserve(4096);

    // ── head
    h += F("<!DOCTYPE html><html><head>"
           "<meta charset='utf-8'><title>Okai BMS</title>"
           "<meta http-equiv='refresh' content='5'>");
    h += "<script>fetch('/settime?t='+Date.now()+'&_t=";
    h += _csrfToken;
    h += "');</script>";
    h += F("<style>"
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

    char rtcStr[40];
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

    // 2026-07-26 - surface the balance phase here too, so the dashboard agrees with
    // the device screen. "CHARGE" alone did not distinguish bulk from the trickle
    // that lets the balancer bleed the high cells.
    bool anyBalancing = false;
    for (uint8_t i = 0; i < NUM_PACKS; i++)
        if (packs[i].valid && packs[i].isBalancing) { anyBalancing = true; break; }

    const char *modeStr = (logCurrentMode() == LOG_RIDE)   ? "&#128694; RIDE" :
                          (logCurrentMode() == LOG_CHARGE) ? (anyBalancing ? "&#9889; CHARGE &mdash; balancing"
                                                                           : "&#9889; CHARGE")
                                                           : "IDLE";
    h += " &nbsp; Log: <b>"; h += modeStr; h += "</b></p>";

    // ── pack table
    h += F("<h3>Pack status</h3>"
           "<table><tr>"
           "<th>Pack</th><th>SOC</th><th>Voltage</th><th>Current</th><th>Power</th>"
           "<th>Avail.Wh</th><th>Cell&Delta;</th><th>Temp</th><th>CYC ID</th><th>Health</th>"
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
        // 2026-07-26 - Verdict comes from the rest-gated helper, never from the
        // live delta. Under load the spread is an internal-resistance reading,
        // not a health reading. See Config.h CELL_DELTA_WARN_V.
        const char *htag = healthTag(packs[i]);
        const char *cls  = (strcmp(htag, "POOR") == 0) ? "poor" :
                           (strcmp(htag, "WARN") == 0) ? "warn" : "good";
        delta = healthDelta(packs[i]);   // show the number actually being judged
        const char *cycStr = packRec[i].known ? packRec[i].cycID : "---";
        char row[320];
        // 2026-07-26 - current preformatted so a balancing trickle shows "<0.00A"
        // instead of "+0.00A"; same helper the device screen uses (Config.h).
        char ampsCell[12];
        fmtAmps(ampsCell, sizeof(ampsCell), packs[i].current, 2, "A");
        snprintf(row, sizeof(row),
            "<td>%s</td><td>%u%%</td><td>%.2fV</td><td>%s</td>"
            "<td>%+.0fW</td><td>%.0f Wh</td><td>%u mV</td>"
            "<td>%u&#176;C</td><td>%s</td><td class='%s'>%s</td></tr>",
            lbl, (unsigned)packs[i].soc, packs[i].voltage, ampsCell,
            powerW, availWh,
            (unsigned)(delta * 1000.0f + 0.5f),
            (unsigned)packs[i].maxTemp, cycStr,
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
                char row[280];
                // f.name() returns just the base name without leading /
                snprintf(row, sizeof(row),
                    "<tr><td>%s</td><td>%u KB</td>"
                    "<td><a class='btn' href='/csv?f=/%s'>&#128229;</a></td>"
                    "<td><a class='btn' href='/delete?f=/%s&_t=%s'>&#128465;</a></td></tr>",
                    f.name(), (unsigned)(f.size() / 1024),
                    f.name(), f.name(), _csrfToken);
                h += row;
            }
            File next = root.openNextFile();
            f.close();
            f = next;
        }
        root.close();
        if (!any) h += F("<tr><td colspan='4' class='dim'>No log files yet</td></tr>");
        h += F("</table>");
        // 2026-07-26 - Big red target with a confirm step. It used to be a small
        // link identical to the other two, one tap from wiping everything, and it
        // did not work anyway. Individual delete buttons above are deliberately
        // left exactly as they are — the owner confirmed they work well and the
        // tiny page refresh keeps the phone view from jumping.
        h += F("<p style='margin:18px 0'>"
               "<a href='/clearall?_t=");
        h += _csrfToken;
        h += F("' onclick=\"return confirm('Delete ALL log files?\\n\\n"
               "The log currently being written is kept.\\nThis cannot be undone.')\" "
               "style='display:block;padding:16px;background:#b3261e;color:#fff;"
               "border-radius:8px;text-decoration:none;font-size:1.15em;"
               "font-weight:bold;text-align:center'>"
               "&#128465; Delete all logs</a></p>"
               "<p><a class='btn' href='/rawdump'>&#128270; Raw frame dump</a>"
               " &nbsp; <a class='btn' href='/packs'>&#128230; Pack registry</a></p>");

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
           "Cell&Delta; judged AT REST only &mdash; warn=100mV poor=180mV</p>"
           "</body></html>");

    _srv.send(200, "text/html", h);
}

// ── Route: /settime ───────────────────────────────────────────────────────────
static void handleSetTime() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken)) {
        _srv.send(403, "text/plain", "Forbidden");
        return;
    }
    if (!_srv.hasArg("t")) { _srv.send(400, "text/plain", "Missing t"); return; }
    int64_t epochMs = (int64_t)_srv.arg("t").toDouble();
    // Clamp to 2020-01-01 .. 2100-01-01 to reject bogus values (HIGH-4)
    if (epochMs < 1577836800000LL || epochMs > 4102444800000LL) {
        _srv.send(400, "text/plain", "Bad epoch");
        return;
    }
    timeSyncSet(epochMs);
    _srv.send(200, "text/plain", "OK");
}

// ── Route: /csv?f=/NAME.csv ───────────────────────────────────────────────────
static void handleCsv() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    String fname = _srv.hasArg("f") ? _srv.arg("f") : String("/bms_log.csv");
    if (!safePath(fname) || !isCsvFile(fname.c_str()) || !fsReady || !LittleFS.exists(fname)) {
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
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken)) {
        _srv.send(403, "text/plain", "Forbidden");
        return;
    }
    String fname = _srv.hasArg("f") ? _srv.arg("f") : String();
    if (!safePath(fname) || !isCsvFile(fname.c_str()) || !fsReady) {
        _srv.send(400, "text/plain", "Bad request");
        return;
    }
    LittleFS.remove(fname);
    _srv.sendHeader("Location", "/");
    _srv.send(302, "text/plain", "Deleted");
}

// ── Route: /clearall ──────────────────────────────────────────────────────────
static void handleClearAll() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken)) {
        _srv.send(403, "text/plain", "Forbidden");
        return;
    }
    // 2026-07-26 - REWRITTEN. This button silently did nothing.
    //
    // WHAT WAS WRONG: the old loop called LittleFS.remove() *while walking the
    // directory* with openNextFile(). Removing an entry invalidates the open
    // directory handle, so the walk ended right after the first delete and the
    // rest of the files were never touched. Single-file delete was unaffected
    // because it never iterates — which is why that one always worked.
    //
    // THE FIX: two passes. Collect every name first, close the directory, then
    // delete. The active log is skipped (owner request) so an in-progress session
    // is never destroyed by a tidy-up.
    uint16_t removed = 0, skipped = 0;
    if (fsReady) {
        const char *activeFile = loggerActiveFile();   // "/Okai_RIDE_….csv" or ""

        // Pass 1 — collect. Nothing is modified while the directory is open.
        static const uint8_t kMaxDel = 64;
        String names[kMaxDel];
        uint8_t n = 0;
        File root = LittleFS.open("/");
        File f    = root.openNextFile();
        while (f && n < kMaxDel) {
            if (isCsvFile(f.name())) names[n++] = String("/") + f.name();
            File next = root.openNextFile();
            f.close();
            f = next;
        }
        if (f) f.close();
        root.close();

        // Pass 2 — delete, directory handle now closed.
        for (uint8_t i = 0; i < n; i++) {
            if (activeFile[0] && names[i] == activeFile) { skipped++; continue; }
            if (LittleFS.remove(names[i])) removed++;
        }
        Serial.printf("[WiFi] cleared %u logs (%u active kept)\n", removed, skipped);
    }

    // Report the count instead of a silent redirect — a silent 302 is precisely
    // why a broken delete looked like a dead button for so long.
    char body[320];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='2;url=/'>"
        "<style>body{background:#0d1117;color:#e0e0e0;font-family:sans-serif;"
        "padding:24px;text-align:center}b{color:#4af;font-size:1.4em}</style>"
        "</head><body><p><b>%u</b> log%s deleted.</p>%s"
        "<p><a style='color:#4af' href='/'>Back</a></p></body></html>",
        (unsigned)removed, removed == 1 ? "" : "s",
        skipped ? "<p>Active log kept — it is still being written.</p>" : "");
    _srv.send(200, "text/html", body);
}

// ── Route: /rawdump — hex dump of all pack frames (bench test / UID hunt) ─────
static void handleRawDump() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
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

// ── Route: /packs — lifetime registry for all known packs ────────────────────
static void handlePacks() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    String h;
    h.reserve(2048);
    h += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<title>Pack Registry</title>"
           "<style>body{background:#0d1117;color:#e0e0e0;font-family:monospace;"
           "padding:16px}h2{color:#4af;margin:8px 0 4px}"
           "table{border-collapse:collapse;width:100%;margin:8px 0}"
           "th{background:#161b22;padding:6px;border:1px solid #333;color:#aaa}"
           "td{padding:6px;border:1px solid #222;text-align:center}"
           ".dim{color:#555}a{color:#4af}"
           "</style></head><body>");
    h += F("<h2>Pack Registry</h2>");
    h += F("<table><tr>"
           "<th>Port</th><th>CYC ID</th><th>First Seen</th><th>Cycles</th>"
           "<th>Sessions</th><th>Wh In</th><th>Wh Out</th>"
           "<th>SoH %</th><th>Spread mV</th>"
           "</tr>");
    for (uint8_t i = 0; i < NUM_PACKS; i++) {
        const PackRecord *r = packRegGet(i);
        char row[256];
        if (!r || !r->known) {
            snprintf(row, sizeof(row),
                "<tr><td>%u</td><td colspan='8' class='dim'>not identified</td></tr>", i+1);
        } else {
            uint16_t lastSpread = r->historyLen ? r->spreadHistory[r->historyLen - 1] : 0;
            uint8_t  lastSoh    = r->historyLen ? r->sohHistory   [r->historyLen - 1] : 0;
            snprintf(row, sizeof(row),
                "<tr><td>%u</td><td>%s</td><td>%s</td><td>%u&rarr;%u</td>"
                "<td>%u</td><td>%.0f Wh</td><td>%.0f Wh</td>"
                "<td>%u%%</td><td>%u</td></tr>",
                i+1, r->cycID, r->firstSeen,
                (unsigned)r->regCYC, (unsigned)r->currentCycles,
                (unsigned)r->sessions,
                r->totalWhCharged, r->totalWhDischarged,
                (unsigned)lastSoh, (unsigned)lastSpread);
        }
        h += row;
    }
    h += F("</table>");

    // ── EVERY STORED RECORD, including packs that are not plugged in.
    // The table above only ever showed the four live ports (packRegGet is port-indexed),
    // which is why the registry had to be read by decoding raw flash to find out that four
    // records all claimed 55 cycles.
    h += F("<h2>Stored records</h2>"
           "<table><tr><th>CYC ID</th><th>regCYC</th><th>last seen</th><th>first seen</th>"
           "<th>auto</th><th>label</th><th>Wh in</th><th>Wh out</th><th>port</th>"
           "<th>number / delete</th></tr>");
    {
        // 2026-10-09 - 3600 → 8192. A row runs ~463 B, so 3600 held SEVEN records. The
        // fleet is 8 numbered packs (NUM_LABELS 8) and the registry rebuild creates all 8,
        // so the page that drives the rebuild was guaranteed to drop one. 8192 holds 17,
        // which leaves room for the duplicate-name records (_2.._9) a cycle-count
        // collision can create. static, so it costs flash-time RAM not stack.
        static char rows[8192];
        uint8_t n = packRegistryList(rows, sizeof(rows), _csrfToken);
        h += rows;
        h += F("</table>");
        char note[200];
        snprintf(note, sizeof(note),
                 "<p class='dim'>%u stored record(s). \"IN USE\" rows are held by a live "
                 "port and cannot be deleted - unplug the pack first.</p>", (unsigned)n);
        h += note;
    }

    // Registry bankruptcy. Deliberately a typed confirmation rather than a one-tap button.
    h += F("<form method='POST' action='/packwipe' style='margin:18px 0'>"
           "<input type='hidden' name='_t' value='");
    h += _csrfToken;
    h += F("'><p class='dim'>Wipe every stored record and rebuild from the markers on the "
           "batteries. Use this when the stored history can no longer be trusted - type "
           "WIPE to confirm.</p>"
           "<input name='confirm' placeholder='type WIPE' style='width:9em'> "
           "<button style='background:#8b1a1a;color:#fff;border:0;padding:8px 14px;"
           "border-radius:4px'>Wipe registry</button></form>");

    h += F("<p><a href='/'>&#8592; Back to dashboard</a></p></body></html>");
    _srv.send(200, "text/html", h);
}

static void handleNotFound() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    _srv.send(404, "text/plain", "Not found");
}

// ── Public API ────────────────────────────────────────────────────────────────
// ── Route: /wifi ──────────────────────────────────────────────────────────────
// Enter the home network ONCE, from the AP. Stored in NVS, never in source control,
// and it survives a firmware flash. The password field is write-only: the form shows
// whether one is stored, never what it is.
static void handleWifiSetup() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    if (_srv.hasArg("ssid")) {
        if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken)) {
            _srv.send(403, "text/plain", "Forbidden");
            return;
        }
        String ssid = _srv.arg("ssid");
        String pass = _srv.arg("pass");
        if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
            _srv.send(400, "text/plain", "SSID 1-32 chars, password up to 64");
            return;
        }
        if (!wifiNetAdd(ssid.c_str(), pass.c_str())) {
            _srv.send(500, "text/plain", "Could not write to NVS");
            return;
        }
        // 2026-10-09 - SAVE NO LONGER TEARS THE AP DOWN, and that was the whole problem
        // with this page. It used to save ONE network and immediately drop the AP to go
        // join it, so there was no way to enter a second or third network in one sitting -
        // which is exactly the owner's ask: home, plus two phone hotspots. Now saving just
        // saves, the page comes back with the list, and joining is a separate button he
        // presses when he has finished entering them.
        Serial.printf("[WiFi] credentials stored for \"%s\" (%u/%u saved)\n",
                      ssid.c_str(), _netCount, (unsigned)WIFI_MAX_NETS);
        _srv.sendHeader("Location", "/wifi");
        _srv.send(302, "text/plain", "saved");
        return;
    }

    // Delete one stored network.
    if (_srv.hasArg("forget")) {
        if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken)) {
            _srv.send(403, "text/plain", "Forbidden");
            return;
        }
        wifiNetForget(_srv.arg("forget").c_str());
        _srv.sendHeader("Location", "/wifi");
        _srv.send(302, "text/plain", "forgotten");
        return;
    }

    // Join now — the explicit action, separated from saving.
    if (_srv.hasArg("join")) {
        if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken)) {
            _srv.send(403, "text/plain", "Forbidden");
            return;
        }
        if (rideSuspected()) {            // SOP-038: never from a web button either
            _srv.send(409, "text/plain", "Refused - packs are discharging");
            return;
        }
        _srv.send(200, "text/html",
                  "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Joining</title>"
                  "<style>body{background:#0d1117;color:#e0e0e0;font-family:sans-serif;"
                  "padding:20px}a{color:#4af}.dim{color:#888;font-size:13px}</style></head>"
                  "<body><h2>Joining now</h2>"
                  "<p>The access point is going away &mdash; that is the point. Reconnect "
                  "your phone to your normal network and open "
                  "<b>http://" WIFI_MDNS_NAME ".local</b></p>"
                  "<p class='dim'>If no stored network answers within 15 seconds it falls "
                  "back to this access point, same name, same password.</p></body></html>");
        _srv.client().flush();
        delay(250);                      // let the response leave before the radio flips
        _wifiAllDown();
        _userForcedOff = false;
        _staRetryAt    = 0;
        _apNoEvidence  = false;
        _wifiStartAuto();
        return;
    }

    // The stored list. SSIDs only — a saved password is never rendered back, not even
    // masked, because this page is reachable over plain HTTP on whatever network the board
    // happens to be on.
    // static, not stack: this runs inside _srv.handleClient() on the 8 KB loopTask stack,
    // and list+body together are ~5.5 kB. Single call site, single-threaded loopTask, so
    // static costs nothing and shows up honestly in the RAM figure instead of hiding in
    // the stack high-water mark. Same reasoning as victims[] in packRegistryForgetAll.
    // Sized FROM the constant, not from a measurement of today's value of it. A row is
    // ~273 B measured (232 literal + substitutions) and carrying the SSID in the forget
    // form instead of an index adds up to 31 more, so 340 is the per-entry worst case with
    // headroom; +96 covers the heading and the <ul>. The static_assert is the point: at
    // WIFI_MAX_NETS 5 the old hand-picked 1400 would have truncated SILENTLY, and a
    // constant two files away is exactly the kind of change nobody re-checks a buffer for.
    // 2026-10-09 - the assert that was here was `X >= X` and could never fire. Worse than
    // absent: it read as protection in the exact spot the last silent-truncation bug lived.
    // These two check real invariants. The first catches someone adding markup to the row -
    // which is precisely what THIS commit did, +31 B for the SSID in the forget field.
    #define WIFI_ROW_BUDGET 340
    #define WIFI_LIST_CAP   (96 + WIFI_MAX_NETS * WIFI_ROW_BUDGET)
    // Literal bytes in the row format below, measured: 231. Plus two SSIDs (<=32 each) and
    // the 8-char CSRF token = 303 worst case.
    static_assert(231 + 2 * 32 + 8 <= WIFI_ROW_BUDGET,
                  "/wifi row markup grew past its per-entry budget - raise WIFI_ROW_BUDGET");
    static char list[WIFI_LIST_CAP];
    size_t lu = 0;
    list[0] = '\0';
    _htmlAppend(list, sizeof(list), &lu,
                "<h3>Saved networks (%u of %u)</h3>", _netCount, (unsigned)WIFI_MAX_NETS);
    if (_netCount == 0) {
        _htmlAppend(list, sizeof(list), &lu,
                    "<p class='dim'>None yet. Add your home network below.</p>");
    } else {
        _htmlAppend(list, sizeof(list), &lu, "<ul>");
        for (uint8_t i = 0; i < _netCount; i++) {
            _htmlAppend(list, sizeof(list), &lu,
                "<li><b>%s</b> "
                "<form method='POST' action='/wifi' style='display:inline'>"
                "<input type='hidden' name='_t' value='%s'>"
                "<input type='hidden' name='forget' value='%s'>"
                "<button style='padding:2px 8px;background:#5a1f1f'>forget</button>"
                "</form></li>", _nets[i].ssid, _csrfToken, _nets[i].ssid);
        }
        _htmlAppend(list, sizeof(list), &lu, "</ul>");
    }
    if (_htmlTruncated(lu, sizeof(list))) {
        Serial.println("[WiFi] /wifi list TRUNCATED - the page is incomplete");
        // Overwrite the tail rather than appending, since by definition there is no room.
        const char* warn = "<p style='color:#ff5555'><b>LIST TRUNCATED &mdash; more "
                           "networks are stored than fit here.</b></p>";
        const size_t wl = strlen(warn);
        if (sizeof(list) > wl + 1) {
            memcpy(list + (sizeof(list) - wl - 1), warn, wl);
            list[sizeof(list) - 1] = '\0';
        }
    }

    // The second invariant, and the one that will actually bite: `list` is substituted INTO
    // `body`, so body must hold the template plus a worst-case list. Template measured at
    // ~1931 literal bytes + ~55 of other substitutions; 2100 is that with headroom. At
    // WIFI_MAX_NETS 6 the old 4096 would have overflowed and `body`'s snprintf return is
    // discarded, so it would have truncated the page with no banner at all.
    #define WIFI_BODY_TEMPLATE_MAX 2100
    static char body[4096];
    static_assert(WIFI_LIST_CAP + WIFI_BODY_TEMPLATE_MAX <= sizeof(body),
                  "the /wifi network list no longer fits inside body[] - raise body");
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi Setup</title><style>body{background:#0d1117;color:#e0e0e0;"
        "font-family:sans-serif;padding:16px;max-width:520px}h2{color:#4af}"
        "input{width:100%%;padding:10px;margin:6px 0 14px;background:#161b22;color:#e0e0e0;"
        "border:1px solid #333;border-radius:4px;font-size:16px}"
        "button{padding:12px 20px;background:#1f6feb;color:#fff;border:0;border-radius:4px;"
        "font-size:16px}ul{padding-left:20px}li{margin:6px 0}"
        "h3{color:#4af;margin-bottom:4px}.dim{color:#888;font-size:13px}a{color:#4af}"
        "</style></head><body>"
        "<h2>WiFi networks</h2>"
        "<p class='dim'>Current state: <b>%s</b>%s%s</p>"
        "%s"
        "<h3>Add a network</h3>"
        "<form method='POST' action='/wifi'>"
        "<input type='hidden' name='_t' value='%s'>"
        "<label>Network name (SSID)</label>"
        "<input name='ssid' maxlength='32' required>"
        "<label>Password</label>"
        "<input name='pass' type='password' maxlength='64'>"
        "<button type='submit'>Save</button></form>"
        "<p class='dim'>Saving does <b>not</b> disconnect anything &mdash; add your home "
        "network and both phone hotspots one after another, then press Join below.</p>"
        "<h3>Join now</h3>"
        "<form method='POST' action='/wifi'>"
        "<input type='hidden' name='_t' value='%s'>"
        "<input type='hidden' name='join' value='1'>"
        "<button type='submit'%s>Join the strongest saved network</button></form>"
        "<p class='dim'>Which one it picks is decided by <b>signal strength</b>, not by the "
        "order in this list &mdash; whichever saved network is strongest where the display "
        "is standing wins. Credentials live in the chip's NVS, never in the firmware, so "
        "they are not in source control and they survive a reflash. Afterwards it joins by "
        "itself at boot and whenever a charge starts, and answers at "
        "<b>http://" WIFI_MDNS_NAME ".local</b>. It never joins while you are riding, and "
        "if nothing answers it falls back to this access point &mdash; <b>" WIFI_AP_SSID
        "</b>, same password as always.</p>"
        "<p><a href='/'>&larr; dashboard</a></p></body></html>",
        wifiStateStr(),
        (_wst == WST_STATION) ? " &mdash; IP " : "",
        (_wst == WST_STATION) ? WiFi.localIP().toString().c_str() : "",
        list,
        _csrfToken,
        _csrfToken,
        _netCount ? "" : " disabled");
    _srv.send(200, "text/html", body);
    // Function-local #defines leak into every later .ino in the concatenated sketch TU.
    #undef WIFI_ROW_BUDGET
    #undef WIFI_LIST_CAP
    #undef WIFI_BODY_TEMPLATE_MAX
}

// ── Route: /packedit ─────────────────────────────────────────────────────────
// Set a label on, or delete, ONE stored record. This is what the registry rebuild runs on:
// before it existed the only way to see stored records was decoding raw flash, and the only
// way to remove one was writing a flash image back.
static void handlePackEdit() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken)) {
        _srv.send(403, "text/plain", "Forbidden");
        return;
    }
    String f = _srv.arg("f");
    String a = _srv.arg("a");
    bool ok = false;
    if (a == "del") {
        ok = packRegistryForget(f.c_str());
    } else if (a == "set") {
        ok = packRegistrySetLabelByFile(f.c_str(), (uint8_t)_srv.arg("l").toInt());
    }
    _srv.sendHeader("Location", "/packs");
    _srv.send(302, "text/plain", ok ? "OK" : "Refused (in use, or bad request)");
}

// ── Route: /packwipe ─────────────────────────────────────────────────────────
// Registry bankruptcy in one button. Needed because currentCycles was re-anchored across
// packs by the old matcher, so stored wear history is BLENDED between batteries rather than
// merely mislabelled — repairing it would leave numbers that look authoritative and are not.
static void handlePackWipe() {
    _lastReqMs = millis();   // G-3: somebody is actually using this radio
    if (!_srv.hasArg("_t") || _srv.arg("_t") != String(_csrfToken) ||
        _srv.arg("confirm") != "WIPE") {
        _srv.send(403, "text/plain",
                  "Forbidden - needs the CSRF token and confirm=WIPE");
        return;
    }
    uint8_t gone = packRegistryForgetAll();
    uint8_t left = packRegistryCount();          // B-5: say whether it actually finished
    char body[760];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Registry wiped</title>"
        "<style>body{background:#0d1117;color:#e0e0e0;font-family:sans-serif;padding:20px}"
        "a{color:#4af}.bad{color:#ff5555;font-weight:bold}</style></head><body>"
        "<h2>%u record(s) deleted</h2>%s"
        "<p>Now plug the packs in ONE AT A TIME and set each number from the marker on the "
        "battery. Records held by a live port were skipped.</p>"
        "<p><a href='/packs'>&larr; registry</a></p></body></html>",
        (unsigned)gone,
        left ? "<p class='bad'>WARNING: records still remain. Press Wipe again until this "
               "says 0 remain. Rebuilding on top of leftovers re-blends the wear history "
               "this wipe exists to destroy.</p>"
             : "<p>0 remain &mdash; the registry is empty.</p>");
    _srv.send(200, "text/html", body);
}

void wifiServerInit() {
    snprintf(_csrfToken, sizeof(_csrfToken), "%08x", (unsigned)esp_random());
    wifiNetsLoad();
    if (wifiHasCreds()) {
        Serial.printf("[WiFi] %u stored network(s):", _netCount);
        for (uint8_t i = 0; i < _netCount; i++) Serial.printf(" \"%s\"", _nets[i].ssid);
        Serial.println();
    } else {
        Serial.println("[WiFi] no stored network — AP will come up; add one at /wifi");
    }
    _srv.on("/wifi",     handleWifiSetup);
    _srv.on("/",         handleRoot);
    _srv.on("/settime",  handleSetTime);
    _srv.on("/csv",      handleCsv);
    _srv.on("/delete",   handleDelete);
    _srv.on("/clearall", handleClearAll);
    _srv.on("/rawdump",  handleRawDump);
    _srv.on("/packs",    handlePacks);
    _srv.on("/packedit", handlePackEdit);
    _srv.on("/packwipe", handlePackWipe);
    _srv.onNotFound(handleNotFound);
}

void wifiServerLoop() {
    const uint32_t now = millis();
    // 2026-10-09 - BOTH of these used to come from logCurrentMode(), i.e. from the logging
    // subsystem, which SOP-038 ranks BELOW WiFi. See RideWatch.ino for the full reasoning
    // and the two concrete failures that produced (a failed mount pinning the mode at IDLE
    // forever, and pack #1's zero-amp charger fault making a whole ride report as CHARGE).
    // The radio-holding charge test is the charger BIT, not real current. chargeActive()
    // (current > 0.150 A) is the complement of chargeDone, so it is false for the whole
    // taper — and the owner's rule is that WiFi stays up for the WHOLE charge, because the
    // dock is where he pulls logs. See chargerPresent() in RideWatch.ino for why the bit
    // is safe here now that the `!charging` veto over the ride shutdown is gone.
    const bool charging = chargerPresent();

    // Lost the home network. RETRY it rather than give up: the owner's case is a garage at
    // the edge of coverage, where a drop is normal and a reconnect is what he expects.
    //
    // 2026-10-09 - M-3: this used to stop the server and set _wst = WST_OFF but NEVER TAKE
    // THE RADIO DOWN. The station interface stayed powered with auto-reconnect running, and
    // because BOTH shutdown paths below require _wst != WST_OFF, nothing could ever turn it
    // off again - a transient beacon miss left the radio scanning for the rest of the ride.
    // _wifiAllDown() is the whole teardown and it is idempotent, so use it.
    // 2026-10-09 - S-7: DEBOUNCE before tearing down. M-3 (above) is right, but reacting to
    // a SINGLE missed beacon means a full deinit/re-init plus a 60 s wait — up to ~60 cycles
    // an hour in a garage at the edge of coverage, which is precisely the owner's case. That
    // is also the heap-churn path R-11 is about, so a hair trigger here makes the unmeasured
    // leak risk worse. Require the link to stay down for STA_LOST_DEBOUNCE_MS.
    if (_wst == WST_STATION && WiFi.status() != WL_CONNECTED) {
        if (_staLostSince == 0) {
            _staLostSince = now;
        } else if (now - _staLostSince >= STA_LOST_DEBOUNCE_MS) {
            Serial.println("[WiFi] station link LOST - radio down, will retry");
            _wifiAllDown();
            _staLostSince = 0;
            _staRetryAt   = now + WIFI_STA_RETRY_MS;
        }
    } else if (_wst == WST_STATION) {
        _staLostSince = 0;            // link came back inside the debounce — no teardown
    }

    // Keep trying for as long as a charge is running. Charging is stationary, mains-powered
    // and off the water, so WiFi stays up for the whole charge - the owner's rule. Riding is
    // the opposite case and is excluded on purpose (SOP-038). A manual BTN1 off is respected.
    // 2026-10-09 - the old test was the logger's LOG_CHARGE. Pack #1's fault (charger
    // detected, +0.000 A, 2 h 12 min) satisfied it, so this branch re-raised the radio
    // every 60 s, each time blocking the loop for up to 15 s in _multi.run(). The 30 s
    // dwell below is what fixes that, NOT a current test: a current test would also go
    // false through the whole taper and strand the owner at the dock with no link.
    // INITIATING a join is gated harder than HOLDING one: chargeJoinWorthy() requires the
    // bit, REAL charge current at some point in this run, AND a 30 s dwell. The current
    // requirement is the one that matters - a dwell alone separates a chattering bit from a
    // steady one, and pack #1's fault is a STEADY bit whose dwell matured hours ago, so
    // without it a long coast could still fire a 15 s scan mid-session. Holding the radio up
    // uses the bare bit, so the CV taper never drops the link.
    // 2026-10-09 - RE-JOIN AFTER A DROP, WITH NO CHARGER. Observed on the bench the day
    // this shipped: the owner's phone hotspot dropped (it slept), the radio came down, and
    // it never came back — even though his HOME network was saved and in range. His spec is
    // the opposite: "if it finds a network, it will connect to the network and keep itself
    // connected... I don't have to manually open the box". Only chargeJoinWorthy() could
    // re-raise it, so no charger meant no reconnect until the next boot.
    //
    // Why it is BOUNDED rather than unlimited: each attempt blocks the main loop for up to
    // WIFI_STA_CONNECT_MS, stalling pack reads and the display. Retrying forever would mean
    // an 8 s stall every 60 s for as long as the board is out of range — e.g. parked on the
    // beach between rides, packs idle, nothing charging. So a drop buys STA_RELOST_MAX_TRIES
    // attempts; a success resets the allowance. A charger still gets unlimited retries,
    // because that is the dock and he wants it reachable there indefinitely.
    const bool chargeWantsJoin = chargeJoinWorthy(CHG_WIFI_DWELL_MS);
    const bool relostWantsJoin = _hadStation && _staRelostTries < STA_RELOST_MAX_TRIES;
    if (_wst == WST_OFF && !_userForcedOff && !rideSuspected() &&
        (chargeWantsJoin || relostWantsJoin) &&
        wifiHasCreds() && (int32_t)(now - _staRetryAt) >= 0) {
        if (!chargeWantsJoin) {
            _staRelostTries++;
            Serial.printf("[WiFi] link was lost - re-join attempt %u of %u (no charger)\n",
                          (unsigned)_staRelostTries, (unsigned)STA_RELOST_MAX_TRIES);
        } else {
            Serial.println("[WiFi] charging - (re)trying the home network");
        }
        _staRetryAt = now + WIFI_STA_RETRY_MS;
        _wifiStartAuto();
    }

    // The window closes. The ONLY outcome that shuts WiFi down is "no network and no
    // charge" - i.e. presumed riding, which is exactly when the keep-alive matters and a
    // radio does not. Every success re-arms the window, so a connected board stays up.
    // R-2: a station link must NOT hold the radio up through a whole ride. SOP-038 says
    // never while riding, and that is the one condition the rule exists for.
    // 2026-10-09 - riding now comes from RideWatch (pack current), not the logger, and the
    // `!charging` escape is GONE. It was there so a charge could hold the radio up, but
    // charging and riding are mutually exclusive in reality and the clause only ever
    // mattered when something had MISREPORTED a ride as a charge - which is exactly pack
    // #1's fault. An unconditional shutdown is what the comment below always claimed.
    const bool riding = rideSuspected();
    if (riding && _wst != WST_OFF) {
        Serial.println("[WiFi] RIDE detected - shutting WiFi down (SOP-038)");
        _wifiAllDown();
    }
    // 2026-10-09 - AP CLIENT RE-ARM. The window used to be re-armed by a station link or a
    // charge only, so an AP session died after 60 s with a phone still associated. That is
    // precisely the registry rebuild: wipe, then plug 8 packs in ONE AT A TIME and set each
    // number from the marker through /packs. That job is minutes long, at the bench, with no
    // charger necessarily attached - the AP would have dropped out from under it repeatedly.
    // Safe against SOP-038 because the ride shutdown above is unconditional and runs FIRST:
    // an associated client cannot hold the radio up into a ride.
    // S-10: the re-arm also requires that this board has NEVER seen the vehicle discharge.
    // With zero telemetry (no packs, or all packs silent) a telemetry-derived interlock has
    // nothing to go on, and an associated phone would otherwise hold the AP up indefinitely.
    // This does not make that case safe — it cannot be made safe from telemetry alone — it
    // bounds it to a board that has not moved since power-on.
    const bool apClient = (_wst == WST_AP && !rideEverSeen() &&
                           WiFi.softAPgetStationNum() > 0);
    if (_wst != WST_OFF && (now - _wstSince) > WIFI_ON_WINDOW_MS) {
        if ((_wst == WST_STATION && !riding) || charging || (apClient && !riding)) {
            _wstSince = now;
        } else {
            Serial.printf("[WiFi] %lu s window closed, no network and no charge - "
                          "shutting down (presumed riding)\n",
                          (unsigned long)(WIFI_ON_WINDOW_MS / 1000UL));
            _wifiAllDown();
        }
    }

    // The deferred boot join.
    //
    // 2026-10-09 - M-4: this used to fire at a fixed t=4 s and test `riding`, which at 4 s
    // the logger could not possibly know - the ride hysteresis has had no chance to arm, so
    // a board rebooted mid-ride looked identical to one sitting on the bench. Mid-session
    // reboots are PROVEN on this unit, so that was not a hypothetical. The cost of getting
    // it wrong is a 15 s blocking _multi.run() stalling pack reads, the display and logging
    // - telemetry and display both outrank WiFi in SOP-038.
    //
    // Now it waits for POSITIVE evidence instead: real pack frames arriving AND no
    // discharge seen since boot (bootJoinSafe()). If that evidence never comes the join is
    // abandoned rather than attempted blind - the charge-start retry below still picks it
    // up at the dock, which is where the owner wants it anyway.
    if (_bootJoinPending) {
        if (now > BOOT_JOIN_DEADLINE_MS) {
            _bootJoinPending = false;
            // 2026-10-09 - B-4. Abandoning outright was a hole: bootJoinSafe() needs at
            // least one pack frame, so powering up with NO PACK ATTACHED failed the gate,
            // abandoned at 30 s, and then nothing retried — station-lost needs WST_STATION,
            // the charge retry needs the charger bit, the window close needs _wst != OFF.
            // WiFi was dead for the entire power cycle and plugging a pack in afterwards
            // did not help. That defeats "WiFi up at boot, no button" outright and strands
            // the web-driven registry rebuild, which is bench work with no pack in yet.
            //
            // The AP is the right fallback: no scan, no 15 s block, nothing to starve. And
            // the ride shutdown above is now unconditional, so the instant any discharge
            // appears this comes straight back down.
            // F-4: gate the fallback on rideEverSeen(). Without it, a mid-ride reboot with
            // PERFECTLY HEALTHY telemetry also raised an AP — bootJoinSafe() fails on the
            // discharge latch for the whole 5-30 s window and routes straight here. On the
            // throttle that was killed on the next pass; coasting it was up to 60 s of AP
            // mid-ride. With the test, the with-telemetry case is gone entirely. The
            // zero-telemetry case stays, and is irreducible for a telemetry-derived gate.
            if (rideEverSeen()) {
                Serial.println("[WiFi] boot join ABANDONED - this board has seen the "
                               "vehicle discharge (SOP-038)");
            } else {
                Serial.println("[WiFi] boot join: no ride evidence either way - starting AP "
                               "(no scan, no blocking join)");
                _apNoEvidence = true;
                _wifiStartAP();
            }
        } else if (now > BOOT_JOIN_EARLIEST_MS && bootJoinSafe()) {
            _bootJoinPending = false;
            _wifiStartAuto();
        }
    }

    // F-1 - THE NO-EVIDENCE AP IS A FALLBACK, NOT A DECISION.
    //
    // Booting with no pack attached cannot satisfy bootJoinSafe(), so the deadline above
    // raises the AP. But _wifiStartAP() sets _wst = WST_AP, and the charge-start station
    // join requires WST_OFF — and with a charger attached the bare bit re-arms the window
    // forever, so the AP never closes to free _wst. Net effect: the board never tried
    // okai.local again for the whole power cycle, stored credentials and a healthy router
    // notwithstanding. That is B-4's own complaint ("plugging a pack in afterwards did not
    // help") left unfixed, and it would have looked like success while home WiFi — the
    // owner's headline requirement — silently failed.
    //
    // So: the moment real pack frames arrive we can finally judge, and we judge. Not while
    // someone is actually USING the AP, because that is the registry rebuild and pulling
    // the network out from under it would be worse than the thing being fixed.
    // 2026-10-09 (G-3) - "no client" is not enough, now that the AP name is predictable
    // again. The owner's phone will AUTO-JOIN `OkaiBMS`, and an idle associated phone would
    // then block this recovery for the whole power cycle: boot with no pack -> fallback AP
    // -> phone auto-joins in his pocket -> no home WiFi, which is F-1 wearing a different
    // hat. The Join button happens to rescue it, but relying on him to press a button to
    // undo a thing the firmware did by itself is not a design.
    //
    // So an associated client only defers the recovery while it is actually BEING USED. A
    // phone sitting idle in a pocket issues no requests; the registry rebuild issues them
    // constantly (the dashboard self-refreshes every 5 s). After AP_IDLE_RECOVER_MS of
    // silence we judge, which is the behaviour the owner asked for: no button, no box.
    const bool apBusy = (WiFi.softAPgetStationNum() > 0) &&
                        _lastReqMs && (now - _lastReqMs) < AP_IDLE_RECOVER_MS;
    if (_apNoEvidence && _wst == WST_AP && packFramesSeen() && !rideEverSeen() &&
        !_userForcedOff && wifiHasCreds() && !apBusy) {
        _apNoEvidence = false;
        Serial.println("[WiFi] pack frames arrived - the fallback AP can judge now, "
                       "trying the home network");
        _wifiAllDown();
        _wifiStartAuto();
    }

    if (wifiActive) _srv.handleClient();
}

// BTN1. Cycles: off → station (or AP if no credentials stored) → off.
void wifiToggle() {
    if (_wst != WST_OFF) {
        _wifiAllDown();
        _userForcedOff = true;          // do not let auto-on undo a deliberate off
        Serial.println("[WiFi] off (manual)");
    } else {
        // S-12: the ON path was not ride-gated at all. R-3's 1.5 s hold stops a brief
        // glitch, but a water bridge lasting 1.5-4.0 s lands exactly in the window between
        // that hold and the 4 s sleep arm — and would have raised the radio mid-ride.
        if (rideSuspected()) {
            Serial.println("[WiFi] toggle REFUSED - packs are discharging (SOP-038)");
            return;
        }
        _userForcedOff = false;
        if (wifiHasCreds()) {
            _wifiStartAuto();
        } else {
            Serial.println("[WiFi] no stored network — starting AP. "
                           "Open http://192.168.4.1/wifi to add one.");
            _wifiStartAP();
        }
    }
}
