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
#include <SD.h>
#include "FS.h"

// With OPI PSRAM enabled (XIAO ESP32-S3 Sense = 8 MB PSRAM), we have
// enormous headroom for the transcript buffer + HTTP body assembly.
// 380 KB transcript covers ~12 hours of speech in one chat request.
// gpt-4o-mini's input context (128 K tokens ≈ 500 KB) is the model
// ceiling, and we sit just below it once prompt + JSON-escape are
// added.  Long-form chat now has the same quality as the summary
// path — no head/tail tricks, never truncates the meeting.
#define CHAT_TRANSCRIPT_CTX  380000
// 16 K tokens in the GPT reply — gpt-4o-mini's hard output cap.
#define CHAT_MAX_TOKENS      16000
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
    http.setTimeout(150000);       // 2.5 min — large bodies + 16 K-token reply
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
String askAboutSummary(const String& question,
                       const String& historyJson,
                       const String& overrideContext,
                       const String& meetingDir) {

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

    // ── 1b. Load full transcript from SD (for history meetings) ────
    // ALWAYS read from the start of the file — no head/tail splitting,
    // no middle elision.  If the meeting is larger than CHAT_TRANSCRIPT_CTX,
    // we stop reading at the limit but never skip content from inside
    // the file.  This guarantees the chat answers from the actual flow
    // of the meeting from minute 0 onward.
    String fullTranscriptFromSD;
    if (meetingDir.length() > 0) {
        String txPath = (meetingDir.startsWith("/") ? "" : "/") + meetingDir + "/full_transcript.txt";
        File ft = SD.open(txPath.c_str(), FILE_READ);
        if (ft) {
            size_t sz   = ft.size();
            size_t want = sz <= (size_t)CHAT_TRANSCRIPT_CTX ? sz : (size_t)CHAT_TRANSCRIPT_CTX;
            fullTranscriptFromSD.reserve(want + 16);

            ft.seek(0);
            char buf[256];
            while (ft.available() && fullTranscriptFromSD.length() < want) {
                int n = sizeof(buf) - 1;
                if (fullTranscriptFromSD.length() + (size_t)n > want)
                    n = want - fullTranscriptFromSD.length();
                int got = ft.readBytes(buf, n);
                if (got <= 0) break;
                buf[got] = '\0';
                fullTranscriptFromSD += buf;
            }
            ft.close();

            if (sz <= (size_t)CHAT_TRANSCRIPT_CTX) {
                Serial.printf("[CHAT] Loaded FULL transcript (%u chars) from %s\n",
                              fullTranscriptFromSD.length(), txPath.c_str());
            } else {
                Serial.printf("[CHAT] Long transcript (%u B) — sent first %u chars (no head+tail split)\n",
                              (unsigned)sz, fullTranscriptFromSD.length());
            }
        } else {
            Serial.printf("[CHAT] Could not open %s — falling back to RAM transcript\n",
                          txPath.c_str());
        }
    }

    // ── 2. Build system prompt ─────────────────────────────────────
    String sys = "You are an expert meeting assistant. Answer the user's "
                 "question PRECISELY based on the meeting content below.  "
                 "Prefer the TRANSCRIPT for factual specifics (who said "
                 "what, exact numbers, names, decisions) — the SUMMARY is "
                 "just an overview and may have skipped details.\n\n"
                 "TRANSCRIPTION CLEAN-UP: the transcript comes from a "
                 "speech-to-text model and contains misheard words — "
                 "especially proper nouns (companies, products, places, "
                 "events, people, brands, technologies, libraries, "
                 "websites, frameworks).\n"
                 "For every proper noun, silently ask yourself: does "
                 "this name actually exist?  Does it match the meeting's "
                 "context?  Would the speakers — given who they are and "
                 "what they're discussing — plausibly use this exact "
                 "term?  If the answer is no on any count, the transcript "
                 "is wrong: use the entity the speakers actually meant.\n"
                 "Note that the transcript reinforcing a wrong phrase "
                 "repeatedly is NOT evidence it's right — ASR makes the "
                 "same error consistently.  When you're confident of a "
                 "corrected form, use the corrected form in your answer.  "
                 "When two interpretations are both plausible, keep the "
                 "transcript as-is.\n\n"
                 "If the answer truly isn't in the content, say so "
                 "clearly.  Format lists with dashes.  Keep answers "
                 "focused — under 250 words unless the user explicitly "
                 "asks for detail.\n\n"
                 "=== MEETING SUMMARY ===\n" + summaryToUse + "\n\n";

    // Prefer SD transcript (full / head+tail) over RAM transcript (live-only).
    // SD path already chose what to send above — just append it verbatim.
    if (fullTranscriptFromSD.length() > 0) {
        sys += "=== TRANSCRIPT (raw spoken content, from SD) ===\n";
        sys += fullTranscriptFromSD;
    } else if (transcriptLocal.length() > 0) {
        sys += "=== TRANSCRIPT (raw spoken content, live tail) ===\n";
        if ((int)transcriptLocal.length() > CHAT_TRANSCRIPT_CTX) {
            int start = (int)transcriptLocal.length() - CHAT_TRANSCRIPT_CTX;
            sys += "[…earlier transcript omitted to fit context window…]\n";
            sys += transcriptLocal.substring(start);
        } else {
            sys += transcriptLocal;
        }
    }

    // ── 3. Build request body ──────────────────────────────────────
    // gpt-4o-mini — fast + cheap for interactive chat.
    String body = "{\"model\":\"gpt-4o-mini\",\"max_tokens\":" + String(CHAT_MAX_TOKENS) + ","
                  "\"temperature\":0.2,\"messages\":[";
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