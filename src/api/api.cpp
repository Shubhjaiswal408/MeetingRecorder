/*
 * api.cpp
 * ─────────────────────────────────────────────────────────────────
 * All outbound HTTPS calls:
 *   • ElevenLabs Scribe v1  — speech-to-text
 *   • OpenAI GPT-4o-mini    — rolling and final meeting summaries
 *
 * Both use a 3-attempt retry loop with WiFi reconnect between tries.
 * ─────────────────────────────────────────────────────────────────
 */

#include "api.h"
#include "../core/globals.h"
#include "SD.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "../json/ShubhJson.h"

// ─── MultipartUploadStream ────────────────────────────────────────────────────
MultipartUploadStream::MultipartUploadStream(File f, const String& h, const String& ft)
    : _file(f), _header(h), _footer(ft), _pos(0) {
    _hLen    = h.length();
    _fileLen = f.size();
    _fLen    = ft.length();
    _total   = _hLen + _fileLen + _fLen;
}

int MultipartUploadStream::read() {
    if (_pos < _hLen)              return (uint8_t)_header[_pos++];
    if (_pos < _hLen + _fileLen)  { _pos++; return _file.read(); }
    if (_pos < _total)             return (uint8_t)_footer[_pos++ - _hLen - _fileLen];
    return -1;
}

size_t MultipartUploadStream::readBytes(char* buf, size_t len) {
    size_t n = 0;
    while (n < len) { int c = read(); if (c < 0) break; buf[n++] = (char)c; }
    return n;
}

int    MultipartUploadStream::available() { return (int)(_total - _pos); }
int    MultipartUploadStream::peek()      { return -1; }
void   MultipartUploadStream::flush()     {}
size_t MultipartUploadStream::write(uint8_t) { return 0; }

// ─── ensureWiFi ───────────────────────────────────────────────────────────────
// Returns true if WiFi is connected, false if reconnect fails.
bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("[WiFi] Lost connection — reconnecting...");
    WiFi.disconnect(false);   // false = keep softAP alive
    delay(200);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (millis() - start > 20000) {
            Serial.println("\n[WiFi] Reconnect TIMEOUT.");
            return false;
        }
    }
    WiFi.setSleep(false);
    Serial.println("\n[WiFi] Reconnected: " + WiFi.localIP().toString()
                   + "  RSSI: " + String(WiFi.RSSI()) + " dBm");
    return true;
}

// ─── jsonEscape ───────────────────────────────────────────────────────────────
String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 64);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char hex[8];
                    snprintf(hex, sizeof(hex), "\\u%04x", (uint8_t)c);
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ─── _sttOnce (internal, single attempt) ─────────────────────────────────────
static String _sttOnce(const String& path) {
    File file = SD.open(path.c_str(), FILE_READ);
    if (!file) return "";

    uint32_t fileSize = file.size();
    Serial.printf("[STT] File size: %u bytes\n", fileSize);

    HTTPClient http;
    http.begin(EL_STT_URL);
    http.setTimeout(90000);
    http.setReuse(false);
    http.addHeader("xi-api-key", elApiKey);

    String boundary = "----ESP32Bound";
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

    String head =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"model_id\"\r\n\r\n"
        "scribe_v1\r\n"
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    size_t totalLen = head.length() + fileSize + tail.length();
    Serial.printf("[STT] Uploading %u bytes...\n", (unsigned)totalLen);

    MultipartUploadStream stream(file, head, tail);
    int    code = http.sendRequest("POST", &stream, totalLen);
    String resp = http.getString();
    http.end();
    file.close();

    Serial.printf("[STT] HTTP %d\n", code);
    if (code == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, resp)) return "";
        String text = doc["text"].as<String>();
        text.replace("<|nb|>", "");
        text.trim();
        Serial.printf("[STT] %d chars: %s\n", text.length(), text.c_str());
        return text.length() > 0 ? text : "[no speech detected]";
    }
    Serial.println("[STT] Error: " + resp.substring(0, 200));
    return "";
}

// ─── transcribeAudio (public, with retry) ────────────────────────────────────
String transcribeAudio(const String& path) {
    if (elApiKey.length() < 10) {
        Serial.println("[STT] No ElevenLabs API key — visit Setup page.");
        return "[No ElevenLabs API key configured]";
    }
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[STT] Attempt %d/3\n", attempt);
        if (!ensureWiFi()) { delay(3000); continue; }
        String result = _sttOnce(path);
        if (result.length() > 0) return result;
        if (attempt < 3) {
            uint32_t wait = attempt * 3000;
            Serial.printf("[STT] Retrying in %us...\n", wait / 1000);
            WiFi.disconnect(false); delay(500);   // false = keep softAP alive
            WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
            delay(wait);
        }
    }
    return "[STT failed after retries]";
}

// ─── _gptCallOnce (internal, single attempt, fully parameterised) ────────────
// Generic GPT-4o-mini call.  Caller supplies the system prompt, user prompt,
// max_tokens and timeout — used by both the regular summariser and the
// chunked map-reduce flow for unbounded-length meetings.
static String _gptCallOnce(const String& sys, const String& user,
                           int maxTokens, int timeoutMs) {
    String body;
    // Pre-reserve a rough capacity to avoid many incremental reallocs while
    // we build the body String.  Saves heap churn on long transcripts.
    body.reserve(sys.length() + user.length() + 256);
    body += "{\"model\":\"gpt-4o-mini\",\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"";
    body += jsonEscape(sys);
    body += "\"},{\"role\":\"user\",\"content\":\"";
    body += jsonEscape(user);
    body += "\"}],\"temperature\":0.4,\"max_tokens\":";
    body += String(maxTokens);
    body += "}";

    HTTPClient http;
    http.begin(OPENAI_URL);
    http.setTimeout(timeoutMs);
    http.setReuse(false);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("Authorization", "Bearer " + openaiApiKey);

    int    code = http.POST(body);
    String resp = http.getString();
    http.end();

    Serial.printf("[GPT] HTTP %d\n", code);
    if (code == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, resp)) return "";
        String result = doc["choices"][0]["message"]["content"].as<String>();
        Serial.printf("[GPT] %d chars received\n", result.length());
        return result;
    }
    Serial.println("[GPT] Error: " + resp.substring(0, 200));
    return "";
}

// ─── _gptCallRetry (internal, with 3-attempt + WiFi-bounce retry) ────────────
static String _gptCallRetry(const String& sys, const String& user,
                            int maxTokens, int timeoutMs) {
    if (openaiApiKey.length() < 10) {
        Serial.println("[GPT] No OpenAI API key — visit Setup page.");
        return "[No OpenAI API key configured — visit Setup page]";
    }
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[GPT] Attempt %d/3\n", attempt);
        if (!ensureWiFi()) { delay(3000); continue; }
        String result = _gptCallOnce(sys, user, maxTokens, timeoutMs);
        if (result.length() > 0) return result;
        if (attempt < 3) {
            uint32_t wait = attempt * 3000;
            Serial.printf("[GPT] Retrying in %us...\n", wait / 1000);
            WiFi.disconnect(false); delay(500);   // false = keep softAP alive
            WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
            delay(wait);
        }
    }
    return "[GPT failed after 3 attempts]";
}

// ─── generateSummary (public, with retry) ────────────────────────────────────
// Used for:
//   - rolling summaries during a meeting (isFinal=false, lightweight)
//   - final summary on stop for SHORT meetings that fit in one GPT call
//     (the chunked/synthesised path takes over for long ones — see
//     generateSegmentSummary + synthesizeFinalSummary)
String generateSummary(const String& transcript, bool isFinal) {
    if (transcript.length() < 10) return "[Not enough transcript yet]";

    String sys = isFinal
        ? "You are a meticulous corporate meeting minutes-taker. Your job is "
          "to capture EVERY major topic discussed across the entire meeting "
          "from beginning to end — not just the most recent tactical decisions. "
          "Cover strategic planning, project assignments, brainstorming and "
          "decisions with equal thoroughness. Use clear markdown with "
          "**bold** for emphasis, ## headings, and bullet lists. "
          "Do NOT skip or condense any major topic."
        : "You are a professional meeting summarizer. Be concise and clear.";

    String user = isFinal
        ? "Below is the FULL transcript of a meeting from start to finish. "
          "Generate comprehensive meeting minutes covering EVERY section of "
          "the meeting in chronological order. Do not omit topics that were "
          "discussed earlier in favour of later ones — give each major topic "
          "its own treatment.\n\n"
          "Use this exact markdown structure:\n\n"
          "## Overview\n"
          "3-4 sentences covering the meeting's purpose and the main themes that came up.\n\n"
          "## Key Discussion Points\n"
          "Cover EVERY major topic discussed, in roughly chronological order. "
          "Use sub-bullets for specifics. Include names of people, products, "
          "vendors, projects, numbers and deadlines mentioned.\n\n"
          "## Decisions Made\n"
          "Every decision reached during the meeting, as a bulleted list.\n\n"
          "## Action Items\n"
          "Who is doing what, by when. One bullet per action.\n\n"
          "## Names, Dates & Numbers\n"
          "Compact reference list of people, dates, vendors, products, "
          "events, metrics or any other concrete data mentioned.\n\n"
          "---\n\nTRANSCRIPT:\n" + transcript
        : "Meeting in progress. Brief rolling summary (max 120 words):\n"
          "1. What's been discussed (2 sentences)\n"
          "2. Key points (3-5 bullets)\n"
          "3. Action items so far\n\nTRANSCRIPT:\n" + transcript;

    int maxTok  = isFinal ? 2500  : 512;
    int timeout = isFinal ? 90000 : 30000;
    return _gptCallRetry(sys, user, maxTok, timeout);
}

// ─── generateSegmentSummary — one segment of a long meeting ──────────────────
// Map step of the map-reduce.  Asked to be THOROUGH (not concise), because
// detail at this stage is what makes the final synthesis comprehensive.
String generateSegmentSummary(const String& transcriptSegment,
                              int segNum, int totalSeg) {
    if (transcriptSegment.length() < 10) return "";

    String sys =
        "You are a thorough meeting minutes-taker working on ONE segment of a "
        "longer meeting that is being summarised piece-by-piece. Your goal is "
        "to extract every concrete piece of information from this segment so a "
        "later synthesis pass can merge segments without losing anything. "
        "Capture EVERY topic, decision, action item, name, vendor, product, "
        "date and number. Be detailed, not concise. Use markdown.";

    String user;
    user.reserve(transcriptSegment.length() + 1024);
    user += "This is segment ";
    user += String(segNum);
    user += " of ";
    user += String(totalSeg);
    user += " from a single meeting transcript. Summarise EVERYTHING discussed "
            "in this segment with the following structure — do not skip "
            "sections even if a segment is light on one of them:\n\n"
            "## Topics Discussed\n"
            "Every topic with sub-bullets for specifics.\n\n"
            "## Decisions\n"
            "Every decision reached in this segment.\n\n"
            "## Action Items\n"
            "Who is doing what, by when.\n\n"
            "## Names, Dates & Numbers\n"
            "All concrete people, vendors, products, dates, deadlines, metrics.\n\n"
            "---\n\nSEGMENT TRANSCRIPT:\n";
    user += transcriptSegment;

    // 1500 tokens (~1100 words) per segment is plenty for a detailed extract.
    return _gptCallRetry(sys, user, 1500, 60000);
}

// ─── synthesizeFinalSummary — reduce step ────────────────────────────────────
// Takes the concatenated segment summaries and produces the single, polished
// final meeting summary that the user sees in the UI.
String synthesizeFinalSummary(const String& combinedSegmentSummaries) {
    if (combinedSegmentSummaries.length() < 30) return "";

    String sys =
        "You are synthesising the final minutes of a single meeting from a "
        "set of segment summaries that cover the meeting in chronological "
        "order. Merge them into ONE comprehensive, polished, professional "
        "meeting summary. Do not duplicate items that appear in multiple "
        "segments — merge them. Do not skip topics. Preserve chronological "
        "order in the discussion section. Use clean markdown with **bold**, "
        "## headings and bullet lists.";

    String user;
    user.reserve(combinedSegmentSummaries.length() + 1024);
    user += "Below are detailed summaries of consecutive segments of a "
            "single meeting, in chronological order. Synthesize them into "
            "ONE comprehensive final meeting summary using this exact "
            "markdown structure:\n\n"
            "## Overview\n"
            "4-5 sentences covering the meeting's purpose and the main themes.\n\n"
            "## Key Discussion Points\n"
            "Cover EVERY major topic in roughly chronological order. Use "
            "sub-bullets. Include names of people, products, vendors, "
            "projects, numbers and deadlines.\n\n"
            "## Decisions Made\n"
            "Every decision reached during the meeting, deduplicated.\n\n"
            "## Action Items\n"
            "Who is doing what, by when. Deduplicated.\n\n"
            "## Names, Dates & Numbers\n"
            "Compact deduplicated reference list.\n\n"
            "Critical rules:\n"
            "- Do NOT omit any topic that appears in any segment.\n"
            "- Do NOT duplicate; merge similar items across segments.\n"
            "- Preserve specifics: names, vendors, dates, metrics, deadlines.\n\n"
            "---\n\nSEGMENT SUMMARIES:\n";
    user += combinedSegmentSummaries;

    // 3000 tokens (~2200 words) for the final polished output.
    return _gptCallRetry(sys, user, 3000, 120000);
}
