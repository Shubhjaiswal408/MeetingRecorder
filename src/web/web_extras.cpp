/*
 * web_extras.cpp  (v3 — Bug 2 fix: /api/status field names match dashboard JS)
 * ─────────────────────────────────────────────────────────────────
 * BUG FIXED:
 *   The old /api/status returned:
 *     { "state":"recording", "chunks":3, "rollingSummary":"...", "finalSummary":"..." }
 *
 *   But the dashboard JavaScript reads:
 *     d.meeting   (boolean)
 *     d.chunk     (number)
 *     d.summary   (rolling summary string)
 *     d.final     (final summary string)
 *     d.ap_ip     (access point IP)
 *     d.sta_ip    (station IP)
 *     d.ssid      (connected WiFi name)
 *     d.rssi      (signal strength)
 *
 *   All fields were misnamed → status dot always showed idle,
 *   button always said "Start Meeting", network card was blank,
 *   summary box never updated.
 *
 *   Fix: return ALL fields the JS expects, plus the new v2
 *   fields so nothing breaks.
 * ─────────────────────────────────────────────────────────────────
 */

#include "web_extras.h"
#include "../api/api.h"       // jsonEscape()
#include "../api/api_chat.h"
#include "../time/ntp_time.h"
#include <WiFi.h>

static void addCORS() {
    server.sendHeader("Access-Control-Allow-Origin",  "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.sendHeader("Cache-Control", "no-cache");
}

// ─────────────────────────────────────────────────────────────────
//  GET /api/status
//  Returns every field the dashboard JS reads, plus extra v2 info.
// ─────────────────────────────────────────────────────────────────
void handleApiStatus() {
    addCORS();

    // ── Snapshot shared transcript/summary state under one short lock.
    //    processTask is the writer; without the lock a String += /
    //    .remove() in-flight can let us read freed memory here.
    String snapTranscript, snapRolling, snapFinal, snapFinalTrans;
    int    snapChunks, snapWords;
    bool   snapChunkReady;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    // Trim while we hold the lock — substring() reads must be atomic w/ the source
    int tStart  = max(0, (int)fullTranscript.length()      - 1200);
    int ftStart = max(0, (int)finalTranscriptText.length() - 4000);
    snapTranscript  = fullTranscript.substring(tStart);
    snapFinalTrans  = finalTranscriptText.substring(ftStart);
    snapRolling     = rollingSummary;
    snapFinal       = finalSummaryText;
    snapChunks      = chunkIndex;
    snapWords       = wordCount;
    snapChunkReady  = chunkReady;
    xSemaphoreGive(stateMutex);

    // ── Snapshots beyond the lock are fine: network + scalars are independent
    String meetingStr = meetingActive ? "true" : "false";
    String staIp      = WiFi.localIP().toString();
    String apIp       = WiFi.softAPIP().toString();
    int    rssi       = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    String ssid       = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "";
    long   elapsed    = elapsedSeconds();

    // State string for v2 consumers
    String state;
    if (meetingActive)              state = "recording";
    else if (snapChunkReady)        state = "processing";
    else if (!snapFinal.isEmpty())  state = "done";
    else                            state = "idle";

    // ── Build JSON ────────────────────────────────────────────────
    // IMPORTANT: field names here MUST match what the JS reads.
    String json = "{";
    json += "\"meeting\":"   + meetingStr + ",";
    json += "\"chunk\":"     + String(snapChunks) + ",";
    json += "\"ap_ip\":\""   + apIp  + "\",";
    json += "\"sta_ip\":\""  + staIp + "\",";
    json += "\"ssid\":\""    + jsonEscape(ssid)        + "\",";
    json += "\"rssi\":"      + String(rssi)            + ",";
    json += "\"summary\":\"" + jsonEscape(snapRolling) + "\",";
    json += "\"final\":\""   + jsonEscape(snapFinal)   + "\",";

    json += "\"state\":\""          + state                         + "\",";
    json += "\"chunks\":"           + String(snapChunks)            + ",";
    json += "\"wordCount\":"        + String(snapWords)             + ",";
    json += "\"elapsedSeconds\":"   + String(elapsed)               + ",";
    json += "\"meetingTime\":\""    + jsonEscape(meetingDisplayTime) + "\",";
    json += "\"freeRam\":"          + String(ESP.getFreeHeap())     + ",";
    json += "\"uptime\":"           + String(millis() / 1000)       + ",";
    json += "\"ntpSynced\":"        + String(ntpSynced ? "true" : "false") + ",";
    json += "\"rollingSummary\":\"" + jsonEscape(snapRolling)       + "\",";
    json += "\"finalSummary\":\""   + jsonEscape(snapFinal)         + "\",";
    json += "\"transcript\":\""     + jsonEscape(snapTranscript)    + "\",";
    json += "\"finalTranscript\":\""+ jsonEscape(snapFinalTrans)    + "\"";
    json += "}";

    server.send(200, "application/json", json);
}

// ─────────────────────────────────────────────────────────────────
//  POST /api/chat
//  Body: {"question":"...","history":[...],"context":"..."}
// ─────────────────────────────────────────────────────────────────
void handleApiChat() {
    addCORS();

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }

    String body = server.arg("plain");

    // Extract "question"
    int qi = body.indexOf("\"question\"");
    if (qi < 0) { server.send(400, "application/json", "{\"error\":\"Missing question\"}"); return; }
    int qs = body.indexOf('"', qi + 10) + 1;
    int qe = qs;
    while (qe < (int)body.length() && !(body[qe] == '"' && body[qe-1] != '\\')) qe++;
    String question = body.substring(qs, qe);
    question.replace("\\\"", "\"");
    question.replace("\\n",  "\n");

    // Extract "history" array
    String historyJson = "";
    int hi = body.indexOf("\"history\"");
    if (hi >= 0) {
        int hb = body.indexOf('[', hi);
        if (hb >= 0) {
            int depth = 0, he = hb;
            do {
                if (body[he]=='[') depth++;
                else if (body[he]==']') depth--;
                he++;
            } while (depth > 0 && he < (int)body.length());
            historyJson = body.substring(hb, he);
        }
    }

    // Extract "context" override
    String contextOverride = "";
    int cxi = body.indexOf("\"context\"");
    if (cxi >= 0) {
        int cs = body.indexOf('"', cxi + 9) + 1;
        if (cs > 0) {
            int ce = cs;
            while (ce < (int)body.length()) {
                if (body[ce] == '\\') { ce += 2; continue; }
                if (body[ce] == '"') break;
                ce++;
            }
            contextOverride = body.substring(cs, ce);
            contextOverride.replace("\\n",  "\n");
            contextOverride.replace("\\\"", "\"");
            contextOverride.replace("\\\\", "\\");
        }
    }

    if (question.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Empty question\"}");
        return;
    }

    Serial.printf("[CHAT] Question: %s  (context: %d chars)\n",
                  question.c_str(), contextOverride.length());

    String answer = askAboutSummary(question, historyJson, contextOverride);
    server.send(200, "application/json",
                "{\"answer\":\"" + jsonEscape(answer) + "\"}");
}

// ─────────────────────────────────────────────────────────────────
//  POST /api/settime
//  Body: {"ts":1705312200000}  (JavaScript Date.now())
// ─────────────────────────────────────────────────────────────────
void handleApiSetTime() {
    addCORS();

    if (!server.hasArg("plain")) { server.send(400); return; }
    String body = server.arg("plain");

    int ti = body.indexOf("\"ts\"");
    if (ti < 0) { server.send(400, "application/json", "{\"error\":\"Missing ts\"}"); return; }

    int ns = ti + 4;
    while (ns < (int)body.length() && !isDigit(body[ns])) ns++;
    int ne = ns;
    while (ne < (int)body.length() && isDigit(body[ne])) ne++;

    long long epochMs = body.substring(ns, ne).toInt();
    setBrowserTime(epochMs);

    server.send(200, "application/json", "{\"ok\":true}");
}