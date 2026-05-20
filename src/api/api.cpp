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
    WiFi.setSleep(true);   // WiFi modem sleep for power saving
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
        ? "You are a meticulous meeting minutes-taker. Your job is to "
          "summarise EXACTLY what was discussed in the transcript provided — "
          "nothing else.\n\n"
          "ABSOLUTE RULES:\n"
          "1. NEVER invent placeholders like [Insert Date], [Project Lead's "
          "Name], [List items]. If something wasn't mentioned, omit it.\n"
          "2. NEVER use generic meeting-minutes templates with sections like "
          "'Opening Remarks', 'Review of Previous Minutes', 'Open Forum', "
          "'Closing Remarks' unless those were actually discussed.\n"
          "3. Only include content the transcript explicitly mentions.\n"
          "4. Use ## (two hash marks) for ALL section headings — never use "
          "'1.', '2.', '3.' for section titles. Numbers are for actual "
          "ordered lists only.\n"
          "5. Use - (dash) for bullet items.\n"
          "6. Use **bold** for emphasis within text only — not for headings."
        : "You are a meeting summariser. Summarise ONLY what was said in the "
          "transcript provided. Never invent placeholders or template "
          "sections. Use ## for headings, - for bullets.";

    String user = isFinal
        ? "Below is the FULL transcript of a meeting from start to finish. "
          "Summarise EVERY topic that was actually discussed — in "
          "chronological order, without skipping earlier topics in favour of "
          "later ones.\n\n"
          "Output EXACTLY in this format (## for headings, - for bullets):\n\n"
          "## Overview\n"
          "3-4 sentences on what the meeting was actually about, based on the "
          "transcript. Do not invent a purpose if none was stated.\n\n"
          "## Key Discussion Points\n"
          "- Every major topic that was discussed, in chronological order\n"
          "- Use sub-bullets (indented with 2 spaces) for specifics\n"
          "- Include real names of people, products, vendors, numbers and "
          "deadlines from the transcript\n\n"
          "## Decisions Made\n"
          "- Every decision actually reached during the meeting\n"
          "- If none, write: 'No formal decisions recorded in this meeting.'\n\n"
          "## Action Items\n"
          "- Who is doing what, by when (only if mentioned)\n"
          "- If none, write: 'No specific action items recorded.'\n\n"
          "## Names, Dates & Numbers\n"
          "- Real people, vendors, products, dates, metrics from the transcript\n"
          "- If none mentioned, write: 'None mentioned.'\n\n"
          "REMINDERS:\n"
          "- Do NOT use [Insert ...] placeholders.\n"
          "- Do NOT invent template sections like 'Opening Remarks' or 'SWOT "
          "Analysis' unless they were actually discussed.\n"
          "- Do NOT use 1./2./3. as section headings. Only ##.\n\n"
          "---\n\nTRANSCRIPT:\n" + transcript
        : "Generate a brief rolling summary (max 120 words) of what has been "
          "discussed in the meeting so far, based ONLY on the transcript "
          "below. Use this format (do NOT use 1./2./3. as headings):\n\n"
          "## So Far\n"
          "2 sentences on the main topic discussed.\n\n"
          "## Key Points\n"
          "- 3-5 bullet points of what was said\n\n"
          "## Action Items So Far\n"
          "- Any action items mentioned, or write 'None yet'\n\n"
          "Do NOT invent placeholders. Only summarise actual content.\n\n"
          "TRANSCRIPT:\n" + transcript;

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
        "You are extracting facts from ONE segment of a longer meeting "
        "transcript. The goal is detail — every topic, name, vendor, number, "
        "decision and action that actually appears in this segment.\n\n"
        "ABSOLUTE RULES:\n"
        "1. NEVER invent placeholders like [Insert Date] or [Project Lead]. "
        "Only include what the transcript actually says.\n"
        "2. NEVER use generic template sections like 'Opening Remarks' that "
        "are not in the transcript.\n"
        "3. Use ## for headings — never '1.', '2.', '3.' for section titles.\n"
        "4. Use - (dash) for bullet items.\n"
        "5. If a section has nothing in this segment, write 'None in this "
        "segment.' rather than inventing content.";

    String user;
    user.reserve(transcriptSegment.length() + 1024);
    user += "This is segment ";
    user += String(segNum);
    user += " of ";
    user += String(totalSeg);
    user += " from a meeting transcript. Extract everything that was "
            "actually said in this segment using EXACTLY this format:\n\n"
            "## Topics Discussed\n"
            "- Every topic with sub-bullets for specifics\n\n"
            "## Decisions\n"
            "- Every decision reached, or 'None in this segment.'\n\n"
            "## Action Items\n"
            "- Who/what/when, or 'None in this segment.'\n\n"
            "## Names, Dates & Numbers\n"
            "- Real names, vendors, products, dates, metrics actually said\n\n"
            "Do NOT use placeholders. Do NOT use 1./2. as headings.\n\n"
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
        "You are merging multiple segment summaries of ONE meeting into a "
        "single polished final summary. Use ONLY the information present in "
        "the segment summaries below.\n\n"
        "ABSOLUTE RULES:\n"
        "1. NEVER invent placeholders like [Insert Name], [Insert Date]. "
        "Only use information that appears in the segments.\n"
        "2. NEVER use generic meeting-minutes templates with sections like "
        "'Opening Remarks', 'Review of Previous Minutes', 'Open Forum', "
        "'Closing Remarks' unless those were actually mentioned.\n"
        "3. Use ## for ALL section headings — never '1.', '2.', '3.'.\n"
        "4. Use - (dash) for bullets.\n"
        "5. Merge duplicate items across segments, but do NOT drop any "
        "topic that appears in any segment.";

    String user;
    user.reserve(combinedSegmentSummaries.length() + 1024);
    user += "Below are detailed summaries of consecutive segments of a "
            "single meeting in chronological order. Merge them into ONE "
            "final summary using EXACTLY this format:\n\n"
            "## Overview\n"
            "4-5 sentences on what the meeting was actually about, based on "
            "the segment summaries.\n\n"
            "## Key Discussion Points\n"
            "- Every major topic discussed across all segments, in "
            "chronological order\n"
            "- Use sub-bullets for specifics\n"
            "- Include real names, products, vendors, numbers, deadlines\n\n"
            "## Decisions Made\n"
            "- Every decision, deduplicated\n"
            "- If none, write 'No formal decisions recorded in this meeting.'\n\n"
            "## Action Items\n"
            "- Who/what/when, deduplicated\n"
            "- If none, write 'No specific action items recorded.'\n\n"
            "## Names, Dates & Numbers\n"
            "- Deduplicated list of real names, vendors, products, dates, "
            "metrics from the segments\n"
            "- If none, write 'None mentioned.'\n\n"
            "REMINDERS:\n"
            "- Do NOT use [Insert ...] placeholders.\n"
            "- Do NOT invent template sections that aren't in the segments.\n"
            "- Do NOT use 1./2./3. as section headings — only ##.\n\n"
            "---\n\nSEGMENT SUMMARIES:\n";
    user += combinedSegmentSummaries;

    // 3000 tokens (~2200 words) for the final polished output.
    return _gptCallRetry(sys, user, 3000, 120000);
}
