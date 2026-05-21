/*
 * api_chat.cpp
 * ─────────────────────────────────────────────────────────────────
 * Sends a user question to OpenAI GPT with the meeting summary
 * and (up to 3 KB of) transcript as grounding context.
 * Uses gpt-4o-mini for fast, affordable responses.
 *
 * Resilience fixes:
 *   • Added http.setReuse(false) — prevents connection-state
 *     corruption that caused HTTP -1 on back-to-back requests.
 *   • Raised timeout to 30 s (matches api.cpp) — reduces -1 on
 *     slow links.
 *   • Added 2-attempt retry loop with WiFi reconnect between
 *     tries — matches the resilience of transcribeAudio /
 *     generateSummary and eliminates single-hiccup failures.
 *   • Error sentinel prefix changed from ⚠ to [ERR] so the
 *     dashboard JS can detect errors and render themed icons
 *     without relying on Unicode emoji (which vary by OS/font).
 * ─────────────────────────────────────────────────────────────────
 */

#include "api_chat.h"
#include "api.h"          // ensureWiFi(), jsonEscape()
#include <HTTPClient.h>
#include <Arduino.h>

// Max characters of transcript sent as context (keep request small)
#define CHAT_TRANSCRIPT_CTX  2500
// Max tokens in the GPT reply
#define CHAT_MAX_TOKENS      700
// Retry attempts for the HTTP POST
#define CHAT_MAX_ATTEMPTS    3

// Internal sentinel used to signal an error to the JS renderer.
// The dashboard intercepts lines starting with this prefix and
// renders a themed warning card instead of a plain chat bubble.
#define CHAT_ERR "[ERR] "

// ─────────────────────────────────────────────────────────────────
// _chatOnce — single attempt, no retry logic
// ─────────────────────────────────────────────────────────────────
static String _chatOnce(const String& body) {
    HTTPClient http;
    http.begin(OPENAI_URL);
    http.setReuse(false);          // ← was missing; caused HTTP -1 on reuse
    http.setTimeout(30000);        // 30 s, matches api.cpp
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("Authorization", "Bearer " + openaiApiKey);

    Serial.printf("[CHAT] POST %d chars\n", body.length());
    int    code = http.POST(body);
    String resp = http.getString();
    http.end();

    Serial.printf("[CHAT] HTTP %d\n", code);
    if (code != 200) {
        Serial.println("[CHAT] Error body: " + resp.substring(0, 120));
        return "";   // caller retries
    }

    // Extract content from {"choices":[{"message":{"content":"…"}}]}
    int ci = resp.indexOf("\"content\":");
    if (ci < 0) return "";

    int qs = resp.indexOf('"', ci + 10);
    if (qs < 0) return "";
    int qe = qs + 1;
    while (qe < (int)resp.length()) {
        if (resp[qe] == '\\') { qe += 2; continue; }
        if (resp[qe] == '"')  break;
        qe++;
    }

    String answer = resp.substring(qs + 1, qe);
    answer.replace("\\n",  "\n");
    answer.replace("\\\"", "\"");
    answer.replace("\\'",  "'");
    answer.replace("\\\\", "\\");

    Serial.printf("[CHAT] Answer: %d chars\n", answer.length());
    return answer;
}

// ─────────────────────────────────────────────────────────────────
String askAboutSummary(const String& question, const String& historyJson, const String& overrideContext) {

    // ── 1. Guards ──────────────────────────────────────────────────
    if (openaiApiKey.isEmpty())
        return CHAT_ERR "OpenAI API key not configured — visit Settings.";

    // Snapshot shared state under lock so processTask can't realloc
    // these Strings while we read them.
    String summaryLocal, transcriptLocal;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    summaryLocal    = finalSummaryText;
    transcriptLocal = fullTranscript;
    xSemaphoreGive(stateMutex);

    String summaryToUse = overrideContext.length() > 10 ? overrideContext : summaryLocal;
    if (summaryToUse.isEmpty())
        return CHAT_ERR "No meeting summary available. Complete a meeting first.";

    // ── 2. Build system prompt ─────────────────────────────────────
    String sys = "You are an expert meeting assistant. Answer questions concisely and accurately "
                 "based ONLY on the meeting content provided below. "
                 "If the answer is not in the provided content, say so clearly. "
                 "Format lists with dashes. Keep answers under 200 words unless more detail is requested.\n\n"
                 "=== MEETING SUMMARY ===\n" + summaryToUse + "\n\n";

    if (transcriptLocal.length() > 0) {
        int start = max(0, (int)transcriptLocal.length() - CHAT_TRANSCRIPT_CTX);
        sys += "=== TRANSCRIPT EXCERPT (last ~" + String(CHAT_TRANSCRIPT_CTX) + " chars) ===\n";
        sys += transcriptLocal.substring(start);
    }

    // ── 3. Build request body ──────────────────────────────────────
    String body = "{\"model\":\"gpt-4o-mini\",\"max_tokens\":" + String(CHAT_MAX_TOKENS) + ","
                  "\"temperature\":0.3,\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys) + "\"}";

    if (historyJson.length() > 4) {
        String inner = historyJson.substring(1, historyJson.length() - 1);
        inner.trim();
        if (inner.length() > 0) body += "," + inner;
    }

    body += ",{\"role\":\"user\",\"content\":\"" + jsonEscape(question) + "\"}";
    body += "]}";

    // ── 4. POST with retry ─────────────────────────────────────────
    for (int attempt = 1; attempt <= CHAT_MAX_ATTEMPTS; attempt++) {
        Serial.printf("[CHAT] Attempt %d/%d\n", attempt, CHAT_MAX_ATTEMPTS);

        if (!ensureWiFi()) {
            delay(2000);
            continue;
        }

        String result = _chatOnce(body);
        if (result.length() > 0) return result;

        if (attempt < CHAT_MAX_ATTEMPTS) {
            Serial.println("[CHAT] Retrying after WiFi bounce...");
            WiFi.disconnect(false);   // false = keep softAP alive
            delay(500);
            WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
            delay(attempt * 2000);
        }
    }

    return CHAT_ERR "Could not reach OpenAI after " + String(CHAT_MAX_ATTEMPTS) + " attempts. Check WiFi and API key.";
}