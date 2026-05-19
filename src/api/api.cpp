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

// ─── _gptOnce (internal, single attempt) ─────────────────────────────────────
static String _gptOnce(const String& transcript, bool isFinal) {
    String sys  = "You are a professional meeting summarizer. Be concise and clear.";
    String user = isFinal
        ? "Meeting ended. Provide FINAL summary:\n"
          "1. Overview (2-3 sentences)\n"
          "2. Key Discussion Points\n"
          "3. Decisions Made\n"
          "4. Action Items\n"
          "5. Important names/dates/numbers\n\nTRANSCRIPT:\n" + transcript
        : "Meeting in progress. Brief rolling summary (max 120 words):\n"
          "1. What's been discussed (2 sentences)\n"
          "2. Key points (3-5 bullets)\n"
          "3. Action items so far\n\nTRANSCRIPT:\n" + transcript;

    String body =
        "{\"model\":\"gpt-4o-mini\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + jsonEscape(sys)  + "\"},"
        "{\"role\":\"user\",\"content\":\""   + jsonEscape(user) + "\"}"
        "],\"temperature\":0.4,\"max_tokens\":512}";

    HTTPClient http;
    http.begin(OPENAI_URL);
    http.setTimeout(30000);
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

// ─── generateSummary (public, with retry) ────────────────────────────────────
String generateSummary(const String& transcript, bool isFinal) {
    if (transcript.length() < 10) return "[Not enough transcript yet]";
    if (openaiApiKey.length() < 10) {
        Serial.println("[GPT] No OpenAI API key — visit Setup page.");
        return "[No OpenAI API key configured — visit Setup page]";
    }
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf("[GPT] Attempt %d/3\n", attempt);
        if (!ensureWiFi()) { delay(3000); continue; }
        String result = _gptOnce(transcript, isFinal);
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
