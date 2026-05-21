/*
 * web_handlers.cpp
 * ─────────────────────────────────────────────────────────────────
 * BUGS FIXED:
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
        // LED is managed by the blink state machine in loop().
        Serial.println("[Web] ● Meeting STARTED via web UI");
    }
    server.send(200, "application/json", "{\"ok\":true}");
}

// ─── POST /api/stop ──────────────────────────────────────────────────────────
static void handleApiStop() {
    if (meetingActive) {
        meetingActive = false;
        finalStop     = true;
        // LED keeps blinking while processTask finishes the final summary.
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
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String el   = server.arg("el_key");
    String oai  = server.arg("openai_key");
    String tz   = server.arg("tz_min");

    bool wifiChanged = false;
    if (ssid.length() > 0 && ssid != wifiSSID) { wifiSSID = ssid; wifiChanged = true; }
    if (pass.length() > 0)                     { wifiPass = pass; wifiChanged = true; }
    if (el.length()  > 4)                      { elApiKey     = el;  }
    if (oai.length() > 4)                      { openaiApiKey = oai; }

    // Timezone — minutes from UTC; clamp to a sane range (−12 h … +14 h)
    bool tzChanged = false;
    if (tz.length() > 0) {
        int v = tz.toInt();
        if (v >= -720 && v <= 840 && v != tzOffsetMin) {
            tzOffsetMin = v;
            tzChanged   = true;
        }
    }

    saveConfig();

    server.send(200, "application/json", "{\"ok\":true}");

    // If timezone changed and we're connected, re-init NTP so the clock
    // jumps to the new offset immediately instead of waiting for the
    // next reboot.
    if (tzChanged && WiFi.status() == WL_CONNECTED) {
        ntpInit();
    }

    // WiFi reconnect is handled in loop() — safely after HTTP response
    if (wifiChanged && wifiSSID.length() > 0) needWifiReconnect = true;

    Serial.printf("[Config] Saved — wifi changed: %s, tz changed: %s (now %+d min)\n",
                  wifiChanged ? "YES" : "no", tzChanged ? "YES" : "no", tzOffsetMin);
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
            // Read the FULL summary file.  Comprehensive final summaries
            // (especially from the map-reduce path on long meetings) are
            // routinely 2-4 KB; the old 1200-char cap was cutting them off
            // mid-word in the History tab even though the Summary tab
            // showed them complete.  We keep a generous safety cap at 8 KB
            // per meeting so the /api/history response can't blow up the
            // HTTP buffer if some file is unexpectedly huge.
            char buf[256];
            while (sf.available() && (int)summary.length() < 8000) {
                int n = sf.readBytes(buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = '\0';
                summary += buf;
            }
            sf.close();
            summary.trim();
        }

        // Skip meetings with no usable summary so the History tab doesn't
        // get cluttered with empty entries.  Two ways a meeting can end up
        // useless: (a) user pressed start/stop without speaking, so no
        // summary_final.txt was ever written (summary stays "") — or
        // (b) GPT failed and we wrote the fallback "Meeting recorded but
        // summary failed..." marker.  Hide both — the underlying
        // directories stay on the SD card and can still be removed
        // manually if desired.
        bool isUseful = summary.length() >= 50
                     && !summary.startsWith("Meeting recorded but summary failed")
                     && !summary.startsWith("Not enough speech");
        if (!isUseful) {
            Serial.printf("[/api/history] Skipping empty meeting: %s\n", name.c_str());
            continue;
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

// ─── POST /api/history/regenerate ────────────────────────────────────────────
// Re-run the final-summary GPT pipeline on a past meeting's saved
// full_transcript.txt and overwrite summary_final.txt with the result.
// Useful when the original final-summary call failed and we fell back to
// the (much shorter) rolling summary.
static void handleApiHistoryRegenerate() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    // Same dir-parsing strategy as the delete endpoint
    String dir = server.arg("dir");
    if (dir.isEmpty() && server.hasArg("plain")) {
        String body = server.arg("plain");
        int di = body.indexOf("dir=");
        if (di >= 0) {
            int de = body.indexOf('&', di);
            dir = de < 0 ? body.substring(di + 4) : body.substring(di + 4, de);
            dir.replace("+", " ");
        }
    }

    if (dir.isEmpty()
     || !dir.startsWith("meeting_")
     || dir.indexOf('/') >= 0
     || dir.indexOf('\\') >= 0
     || dir.indexOf("..") >= 0) {
        Serial.printf("[History] Refused regenerate (bad dir): '%s'\n", dir.c_str());
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid dir\"}");
        return;
    }

    String fullPath = "/" + dir;
    Serial.printf("[History] Regenerating summary for: %s\n", fullPath.c_str());

    // This is the slow path — runs GPT, can take 30 s for a short meeting
    // or several minutes for a long map-reduce.  The browser fetch can
    // wait; we keep handleClient() responsive by virtue of webTask being
    // on its own pinned core.
    String newSum = regenerateSummaryForMeeting(fullPath);

    if (newSum.length() < 80) {
        server.send(500, "application/json",
                    "{\"ok\":false,\"error\":\"regeneration failed — check OpenAI key / network\"}");
        return;
    }

    String resp = "{\"ok\":true,\"summary\":\"" + jsonEscape(newSum) + "\"}";
    server.send(200, "application/json", resp);
}

// ─── POST /api/factory-reset ─────────────────────────────────────────────────
// Sets a flag for processTask (which has the 20 KB stack we need for
// recursively walking and deleting every meeting directory) and returns
// immediately.  Doing the actual work in this handler crashed the webTask
// with a stack-canary watchpoint when the device had many stored meetings.
static void handleApiFactoryReset() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST,OPTIONS");
    server.sendHeader("Cache-Control", "no-cache");

    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "POST only");
        return;
    }

    if (meetingActive) {
        server.send(409, "application/json",
                    "{\"ok\":false,\"error\":\"stop the active meeting first\"}");
        return;
    }

    Serial.println("[FactoryReset] Request received — scheduled in processTask");
    needFactoryReset = true;

    // Send the response now; processTask will do the SD work and reboot.
    // The browser's fetch will succeed; the JS then renders its
    // "device reset complete" screen.  If the reboot happens before the
    // browser has fully received the response, the fetch error branch
    // still shows the same screen.
    server.send(200, "application/json",
                "{\"ok\":true,\"msg\":\"factory reset scheduled — device will reboot in a few seconds\"}");
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
    server.on("/api/history",            HTTP_GET,  handleApiHistory);
    server.on("/api/history/delete",     HTTP_POST, handleApiHistoryDelete);
    server.on("/api/history/regenerate", HTTP_POST, handleApiHistoryRegenerate);
    server.on("/api/factory-reset",      HTTP_POST, handleApiFactoryReset);

    // Extra routes from web_extras
    server.on("/api/status",  HTTP_GET,  handleApiStatus);
    server.on("/api/chat",    HTTP_POST, handleApiChat);
    server.on("/api/settime", HTTP_POST, handleApiSetTime);

    server.onNotFound(handle404);
    server.begin();
    Serial.println("[Web] Server started on port 80.");
}