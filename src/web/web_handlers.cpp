/*
 * web_handlers.cpp
 * ─────────────────────────────────────────────────────────────────
 * BUGS FIXED (in addition to v3 fixes):
 *
 *   Bug A — handleApiHistory SD iterator corruption:
 *     Old code opened summary files WHILE the root directory iterator
 *     was still active. On ESP32's SD/FatFS layer this corrupts the
 *     iterator → entries get skipped → history shows empty even when
 *     meeting dirs exist on the card.
 *     Fix: two-pass approach — collect all dir names (close root),
 *     then open each summary file separately.
 *
 *   Bug B — handleApiHistoryDelete missing dir arg:
 *     On some ESP32 WebServer builds, server.arg() for a POST with
 *     a URL query param + body arg would return empty.
 *     Fix: explicit body fallback parse if server.arg() returns "".
 *
 *   Bug C — handleApiStart missing finalSummaryText reset:
 *     finalSummaryText was cleared but process.cpp now preserves
 *     finalTranscriptText / finalSummaryText after meeting end so
 *     the UI can still display them. Both must be cleared here when
 *     a NEW meeting starts.
 * ─────────────────────────────────────────────────────────────────
 */

#include "web_handlers.h"
#include "../core/globals.h"
#include "html_pages.h"              // defines PAGE_MAIN and PAGE_SETUP
#include "../config/config.h"
#include "../api/api.h"              // jsonEscape()
#include "../process/process.h"      // deleteDirRecursive()
#include "web_extras.h"              // handleApiStatus(), handleApiChat(), handleApiSetTime()
#include "../time/ntp_time.h"        // stampNow()
#include "FS.h"
#include "SD.h"
#include <WiFi.h>

// ─── GET / ────────────────────────────────────────────────────────────────────
static void handleRoot() {
    server.send_P(200, "text/html", DASHBOARD_HTML);
}

static void handleSetup() {
    String page = String(FPSTR(SETUP_HTML));
    page.replace("%%SSID%%", wifiSSID);
    server.send(200, "text/html", page);
}

// ─── POST /api/start ─────────────────────────────────────────────────────────
static void handleApiStart() {
    if (!meetingActive) {
        // Full-speed CPU for I2S DMA + STT/GPT TLS work.
        // Dropped back to 80 MHz at the end of the final-summary block in
        // processTask (see process.cpp).
        setCpuFrequencyMhz(240);
        Serial.println("[Power] Active: CPU @ 240 MHz");
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        fullTranscript      = "";
        finalTranscriptText = "";   // clear previous meeting's transcript
        finalSummaryText    = "";   // clear previous meeting's summary
        chunkIndex          = 0;
        wordCount           = 0;
        rollingSummary      = "Meeting started — summary will appear after the first chunk.";
        xSemaphoreGive(stateMutex);
        xQueueReset(chunkQueue);   // discard any leftover chunk paths
        finalStop           = false;
        createMeetingDir();
        meetingActive       = true;
        digitalWrite(LED_BUILTIN, LOW);
        Serial.println("[Web] ● Meeting STARTED via web UI");
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/stop ──────────────────────────────────────────────────────────
static void handleApiStop() {
    if (meetingActive) {
        meetingActive = false;
        finalStop     = true;
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.println("[Web] ■ Meeting STOPPED via web UI");
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/config ────────────────────────────────────────────────────────
static void handleApiConfig() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }
    String ssid   = server.arg("ssid");
    String pass   = server.arg("pass");
    String el     = server.arg("el_key");
    String oai    = server.arg("openai_key");
    String apSsid = server.arg("ap_ssid");
    String apPas  = server.arg("ap_pass");

    if (ssid.length() > 0) wifiSSID     = ssid;
    if (pass.length() > 0) wifiPass     = pass;
    if (el.length()  > 4)  elApiKey     = el;
    if (oai.length() > 4)  openaiApiKey = oai;

    // AP settings: name needs ≥2 chars, password needs ≥8 (WPA2 minimum)
    bool apChanged = false;
    if (apSsid.length() >= 2 && apSsid != apSSID) { apSSID = apSsid; apChanged = true; }
    if (apPas.length()  >= 8 && apPas  != apPass)  { apPass  = apPas;  apChanged = true; }

    saveConfig();

    // Build response BEFORE scheduling AP restart (restart will drop the connection)
    String resp = "{\"ok\":true,\"apChanged\":" + String(apChanged ? "true" : "false");
    if (apChanged) resp += ",\"apSSID\":\"" + jsonEscape(apSSID) + "\"";
    resp += "}";
    server.send(200, "application/json", resp);

    // Flags handled in loop() — safely after HTTP response is sent
    if (apChanged)           needApRestart     = true;
    if (wifiSSID.length() > 0) needWifiReconnect = true;

    Serial.printf("[Config] Saved — AP changed: %s\n", apChanged ? "YES" : "no");
}

// ─── GET /api/history ────────────────────────────────────────────────────────
// Scans SD root for meeting_* directories and returns their final summaries.
//
// BUG FIX: The original implementation opened summary files WHILE the root
// directory iterator was still active. On ESP32's SD/FatFS layer this can
// corrupt the iterator mid-loop (entries get skipped, or the function silently
// returns an empty array even though meetings exist).
//
// Fix: TWO-PASS approach:
//   Pass 1 — collect directory names, then CLOSE root.
//   Pass 2 — open each summary file independently (root is closed).
//
// Also caps each summary at 1 200 chars to keep the JSON response small
// (the full summary is re-fetched via /api/chat context anyway).
static void handleApiHistory() {

    // ── Pass 1: collect meeting directory names ────────────────────
    const int MAX_MEETINGS = 30;
    String    dirs[MAX_MEETINGS];
    int       dirCount = 0;

    File root = SD.open("/");
    if (!root) {
        server.send(200, "application/json", "[]");
        return;
    }

    File entry = root.openNextFile();
    while (entry && dirCount < MAX_MEETINGS) {
        String name = String(entry.name());
        if (name.startsWith("/")) name = name.substring(1);   // strip leading /
        bool isDir  = entry.isDirectory();
        entry.close();   // close BEFORE opening any other file

        if (isDir && name.startsWith("meeting_")) {
            dirs[dirCount++] = name;
        }
        entry = root.openNextFile();
    }
    // Also close any remaining entry (loop exited early via count limit)
    if (entry) entry.close();
    root.close();   // ← root is now fully closed before any sub-file open

    // Sort descending (newest timestamp first — lexicographic sort works
    // because directory names contain ISO-style timestamps).
    for (int i = 0; i < dirCount - 1; i++) {
        for (int j = i + 1; j < dirCount; j++) {
            if (dirs[j] > dirs[i]) { String tmp = dirs[i]; dirs[i] = dirs[j]; dirs[j] = tmp; }
        }
    }

    // ── Pass 2: read each summary (root is closed — no iterator conflict) ─
    String json  = "[";
    bool   first = true;

    for (int i = 0; i < dirCount; i++) {
        const String& name = dirs[i];
        String summary = "";

        // Primary: summary_final.txt (what process.cpp always writes)
        File sf = SD.open(("/" + name + "/summary_final.txt").c_str(), FILE_READ);

        // Fallback: timestamped name from older firmware versions
        if (!sf) {
            String pastTs = name.substring(8);   // strip "meeting_"
            if (pastTs.length() > 0) {
                sf = SD.open(("/" + name + "/summary_" + pastTs + ".txt").c_str(), FILE_READ);
            }
        }

        if (sf) {
            while (sf.available() && (int)summary.length() < 1200)
                summary += (char)sf.read();
            sf.close();
            summary.trim();
        }

        if (!first) json += ",";
        json += "{\"dir\":\""     + jsonEscape(name)    + "\","
              +  "\"summary\":\"" + jsonEscape(summary) + "\"}";
        first = false;
    }

    json += "]";
    server.send(200, "application/json", json);
}

// ─── POST /api/history/delete ────────────────────────────────────────────────
static void handleApiHistoryDelete() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    // The JS sends the dir name as BOTH a URL query param and a POST body arg.
    // ESP32 WebServer::arg() searches _getArgs (URL) then _postArgs (body), so
    // one call is sufficient — but on some builds only one source is populated.
    // Read both and prefer whichever is non-empty.
    String dir = server.arg("dir");
    if (dir.isEmpty()) {
        // Manual parse from raw body as last resort
        if (server.hasArg("plain")) {
            String body = server.arg("plain");
            int di = body.indexOf("dir=");
            if (di >= 0) {
                int de = body.indexOf('&', di);
                dir = de < 0 ? body.substring(di + 4) : body.substring(di + 4, de);
                // URL-decode the '+' and percent-encoded chars (basic)
                dir.replace("+", " ");
            }
        }
    }

    // Safety guard — must start with "meeting_", no slashes, no parent traversal
    if (dir.isEmpty()
     || !dir.startsWith("meeting_")
     || dir.indexOf('/') >= 0
     || dir.indexOf('\\') >= 0
     || dir.indexOf("..") >= 0) {
        Serial.printf("[History] Refused delete (bad dir): '%s'\n", dir.c_str());
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid dir\"}");
        return;
    }

    String fullPath = "/" + dir;
    Serial.printf("[History] Deleting: %s\n", fullPath.c_str());
    deleteDirRecursive(fullPath);
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── 404 ─────────────────────────────────────────────────────────────────────
static void handle404() {
    server.send(404, "text/plain", "Not found");
}

// ─── startWebServer ───────────────────────────────────────────────────────────
void startWebServer() {
    server.on("/",                   HTTP_GET,  handleRoot);
    server.on("/setup",              HTTP_GET,  handleSetup);
    server.on("/api/start",          HTTP_POST, handleApiStart);
    server.on("/api/stop",           HTTP_POST, handleApiStop);
    server.on("/api/config",         HTTP_POST, handleApiConfig);
    server.on("/api/history",        HTTP_GET,  handleApiHistory);
    server.on("/api/history/delete", HTTP_POST, handleApiHistoryDelete);

    // v2 routes from web_extras
    server.on("/api/status",  HTTP_GET,  handleApiStatus);
    server.on("/api/chat",    HTTP_POST, handleApiChat);
    server.on("/api/settime", HTTP_POST, handleApiSetTime);

    server.onNotFound(handle404);
    server.begin();
    Serial.println("[Web] Server started on port 80.");
}