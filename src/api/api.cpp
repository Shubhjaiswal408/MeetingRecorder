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
    // 1.3× growth + headroom for control-char escapes (\u00XX = 6 chars).
    // Pre-reserving large slabs lets the ESP32 allocator hand them off to
    // PSRAM in one shot, avoiding reallocation churn on huge transcripts.
    out.reserve((s.length() * 130) / 100 + 256);
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
    http.setTimeout(180000);   // 3 min — large bodies + long thinking
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
    // Pre-reserve generously — JSON escape can add ~30 % to size, plus
    // we have model/temperature/max_tokens prefix and message wrappers.
    // A single big reservation lets the ESP32 allocator place the buffer
    // in PSRAM in one shot (needs OPI PSRAM enabled in Tools menu).
    body.reserve((sys.length() + user.length()) * 140 / 100 + 4096);
    // gpt-4o (full) — significantly stronger reasoning + factual recovery
    // than gpt-4o-mini.  Higher cost (~$0.05/regen) but per-call quality
    // matters more than per-call cost for this product.
    // gpt-4o-mini — cheaper + faster.  With our 3-pass pipeline
    // (write → review → fact-check) we get production quality without
    // the gpt-4o premium.
    body += "{\"model\":\"gpt-4o-mini\",\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"";
    body += jsonEscape(sys);
    body += "\"},{\"role\":\"user\",\"content\":\"";
    body += jsonEscape(user);
    // Lower temperature → more focused / deterministic / fewer hallucinations.
    body += "\"}],\"temperature\":0.2,\"max_tokens\":";
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
        ? "You are a meeting summariser.  Write a detailed summary "
          "covering every meaningful topic from the transcript.  Use "
          "2-6 content-driven '##' headings named after the actual "
          "topics (not generic 'Overview' / 'Introduction' / 'Conclusion').  "
          "After the topic sections, add '## Decisions Made', "
          "'## Action Items', and/or '## People & Key Details' only if "
          "they have real content.\n\n"
          "The transcript is from speech-to-text — use context and your "
          "world knowledge to silently correct misheard proper nouns "
          "(places, brands, products, events).  Don't preserve obvious "
          "ASR errors even if the transcript repeats them."
        : "Write a brief plain-prose rolling summary of the transcript "
          "below.  No headings, no templates.";

    String user = isFinal
        ? "Below is the FULL transcript of a meeting.\n\n"
          "Write a thorough summary that covers every meaningful topic.  "
          "Use 2-6 '##' headings named after the ACTUAL topics from this "
          "meeting (NOT generic 'Introduction', 'Overview', 'Conclusion', "
          "'Closing Remarks').  Under each heading, write a few "
          "sentences or bullets describing what was said.\n\n"
          "AFTER the topic sections, include '## Decisions Made', "
          "'## Action Items', and/or '## People & Key Details' — but "
          "ONLY if they have real content.  Skip empty sections.\n\n"
          "Use your world knowledge to silently correct mis-heard proper "
          "nouns from the speech-to-text (places, brands, products, "
          "events that don't exist or don't fit the meeting's context).  "
          "Use '-' for bullets.\n\n"
          "---\n\nTRANSCRIPT:\n" + transcript
        : "Write a brief rolling summary (max 120 words) of what has been "
          "discussed in the meeting so far, based ONLY on the transcript "
          "below.  Use plain paragraphs — no '##' headings, no category "
          "labels.  Just say naturally what's been talked about, the way "
          "you'd brief someone joining the meeting late.\n\n"
          "TRANSCRIPT:\n" + transcript;

    // User wants UNLIMITED — set to gpt-4o-mini's hard ceiling (16 K
    // tokens ≈ 12 K words).  The model won't exceed this no matter what
    // we ask for, but giving it the full ceiling lets it write as much
    // as the meeting warrants.
    int maxTok  = isFinal ? 16000 : 512;
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
        "1. NEVER use square brackets [ ] for placeholders. No [Insert Date], "
        "no [Team Member Name], no [List items]. If something wasn't said, "
        "write 'someone' / 'a team member' / 'TBD' — never square brackets.\n"
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
    // Bumped from 1500 to 4000 — each segment extraction can capture
    // every detail without truncation.
    return _gptCallRetry(sys, user, 4000, 90000);
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
        "1. NEVER use square brackets [ ] in your output. No [Insert Name], "
        "no [Team Member Name], no [Counselor's Name]. If a name wasn't "
        "specified in the segments, write 'someone' / 'a team member' / "
        "'TBD' — never square brackets.\n"
        "2. NEVER use generic meeting-minutes templates with sections like "
        "'Opening Remarks', 'Review of Previous Minutes', 'Open Forum', "
        "'Closing Remarks' unless those were actually mentioned.\n"
        "3. Use ## for ALL section headings — never '1.', '2.', '3.'.\n"
        "4. Use - (dash) for bullets.\n"
        "5. Merge duplicate items across segments, but do NOT drop any "
        "topic that appears in any segment.";

    String user;
    user.reserve(combinedSegmentSummaries.length() + 1024);
    user += "Below are detailed extractions from consecutive segments of a "
            "single meeting in chronological order.  Merge them into ONE "
            "clear final summary that covers the entire flow of the meeting "
            "from start to finish.\n\n"
            "STRUCTURE:\n"
            "1) Main body — 2-6 headings (use '## Heading Name') where "
            "each heading is the NAME OF AN ACTUAL TOPIC from THIS meeting "
            "(e.g. '## Friday attendance trend', '## John Smith's "
            "situation').  NEVER use generic boilerplate headings like "
            "'Overview', 'Core Objective', 'Contest Details', 'Factual "
            "Audit', 'Opening Remarks', 'Closing Remarks', 'Conclusion', "
            "'Wrap-Up', 'Other Discussion', 'Miscellaneous'.\n\n"
            "2) Tail sections — AFTER the topic headings, include any of "
            "these THREE that actually have content (skip a section if it "
            "would be empty — do NOT write 'None mentioned' filler):\n\n"
            "    ## Decisions Made\n"
            "    - Concrete decisions actually reached, one per bullet.\n\n"
            "    ## Action Items\n"
            "    - Who is doing what, by when (if mentioned).\n\n"
            "    ## People & Key Details\n"
            "    - Real people with their role (host / presenter / "
            "audience / participant / student / etc.)\n"
            "    - Specific numbers, dates, deadlines, products, places.\n\n"
            "If a tail section has nothing in this meeting, OMIT the "
            "heading entirely.  No filler.\n\n"
            "Cover every meaningful topic from every segment — opening, "
            "middle and end.  Deduplicate items that appear in more than "
            "one segment.  Weight coverage by time spent discussing.\n\n"
            "Preserve any unusual names exactly as they appear when "
            "the transcription is clearly correct.  Keep distinct roles "
            "distinct.  Use - (dash) for bullets.  No 1./2./3.  No "
            "[Insert ...] placeholders.\n\n"
            "---\n\nSEGMENT SUMMARIES:\n";
    user += combinedSegmentSummaries;

    // 3000 tokens (~2200 words) for the final polished output.
    // 16 K tokens — model maximum.  Long meetings get fully detailed
    // synthesis with no truncation.
    return _gptCallRetry(sys, user, 16000, 180000);
}

// ─── reviewAndFixSummary — second-pass quality control ──────────────────────
// The first GPT pass writes a draft.  This second pass acts as a critic:
// it has the transcript and the draft side-by-side and must find every
// mistake, ASR mishearing, boilerplate slip and contradiction — then
// output the fully corrected version.  Two-pass adds ~50-90 s but
// catches the issues that a single pass keeps missing (boilerplate
// headings that slip through, garbled proper nouns that the writer
// preserved out of fidelity, hallucinated details).
String reviewAndFixSummary(const String& transcript, const String& draftSummary) {
    if (draftSummary.length() < 50 || transcript.length() < 50) return draftSummary;

    String sys =
        "You are a STRICT editor doing forensic review of a draft meeting "
        "summary.  Your default assumption is that the draft has "
        "multiple errors — your job is to find every one of them and "
        "produce a corrected version.  Outputting the draft unchanged is "
        "a failure of your task.  Producing minimal cosmetic edits when "
        "real errors exist is also a failure.  Be aggressive.";

    String user;
    user.reserve(transcript.length() + draftSummary.length() + 4096);
    user += "You will receive a meeting TRANSCRIPT (from speech-to-text, "
            "with ASR errors) and a DRAFT SUMMARY of that transcript.\n\n"
            "The draft was written by a model that defers too much to "
            "transcript fidelity.  It almost certainly contains errors "
            "from the following categories — find ALL of them and fix.\n\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "MANDATORY CHECKLIST  (work through every item):\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
            "[1] EVERY PROPER NOUN in the draft (every event name, "
            "company, product, place, person, framework, website).  For "
            "each, ask three questions:\n"
            "      • Does this entity actually exist in the real world?\n"
            "      • Does it fit the meeting's domain and physical "
            "context (country, industry, who's speaking)?\n"
            "      • Is there a similar-sounding real entity that "
            "would fit BETTER given the context?\n"
            "    If yes to #3 (or no to #1 or #2), REPLACE the wrong "
            "noun with the correct one.  Examples of how this works:\n"
            "      • A place name that doesn't fit the country being "
            "discussed (e.g. a war-torn region appearing in a Japan "
            "tech-meeting context) → find the real similar-sounding "
            "place that fits.\n"
            "      • An event/brand name that doesn't exist or doesn't "
            "match the industry → find the real one.\n"
            "      • A person's surname that isn't a real surname → "
            "keep transcript spelling but flag mentally.\n"
            "    Do NOT defer to the transcript — speech-to-text "
            "consistently mishears the same word.  The wrong word "
            "appearing five times is still wrong.\n\n"
            "[2] HEADINGS.  Scan every '## ' line in the draft.  If any "
            "heading matches one of these template names (or contains "
            "these words anywhere in it), REWRITE it as a content-based "
            "topic name from THIS meeting:\n"
            "       Introduction, Introduction of <X>, Introduction and "
            "Purpose, Purpose, Background, Welcome, Opening, Opening "
            "Remarks, Overview, Closing Remarks, Conclusion, Conclusion "
            "and Next Steps, Wrap-Up, Next Steps, Other Discussion, "
            "Miscellaneous, Final Thoughts.\n"
            "    The first heading MUST be the name of the first actual "
            "topic — never a meta-label like 'Introduction of Judges' "
            "or 'Purpose of the Meeting'.\n\n"
            "[3] FORBIDDEN PHRASES inside body text.  Remove generic "
            "filler like 'aimed to', 'highlighted the importance of', "
            "'emphasized the need for' when they don't convey actual "
            "content.\n\n"
            "[4] HALLUCINATIONS — any claim in the draft that the "
            "transcript doesn't support.  Cut them.\n\n"
            "[5] MISSED TOPICS — anything significant in the transcript "
            "that doesn't appear in the draft.  Add them, in "
            "chronological order, with the same depth as the other "
            "topics.\n\n"
            "[6] ROLES — keep judges / audience / hosts / presenters / "
            "participants distinct.  Fix any blurring.\n\n"
            "[7] INTERNAL CONTRADICTIONS — resolve using the transcript.\n\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "EXPECTATION:  your output will be NOTICEABLY DIFFERENT "
            "from the draft — different first heading, several proper "
            "nouns corrected, possibly new sections added.  If your "
            "output is character-identical or near-identical to the "
            "draft, you have not actually done the review.\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
            "OUTPUT: produce ONLY the corrected summary.  No preamble, "
            "no 'Here is the revised version', no list of changes.  "
            "Same overall format (## headings, - bullets) but better.\n\n"
            "---\n\nTRANSCRIPT:\n";
    user += transcript;
    user += "\n\n---\n\nDRAFT SUMMARY:\n";
    user += draftSummary;
    user += "\n\n---\n\nNow output the corrected summary:";

    Serial.printf("[Review] transcript %u + draft %u → reviewing...\n",
                  transcript.length(), draftSummary.length());
    String fixed = _gptCallRetry(sys, user, 16000, 180000);

    if (fixed.length() < 100 || fixed.startsWith("[")) {
        Serial.println("[Review] failed or empty — falling back to draft");
        return draftSummary;
    }
    // Compute a rough "how much changed" metric — count chars where
    // draft[i] != fixed[i] for the overlap.  Tells us at a glance whether
    // the critic actually edited the draft or just rubber-stamped it.
    size_t minLen = draftSummary.length() < fixed.length()
                    ? draftSummary.length() : fixed.length();
    size_t diffChars = 0;
    for (size_t i = 0; i < minLen; i++) {
        if (draftSummary[i] != fixed[i]) diffChars++;
    }
    int pct = minLen > 0 ? (int)((diffChars * 100) / minLen) : 0;
    Serial.printf("[Review] OK — draft %u → reviewed %u chars (%u%% changed)\n",
                  draftSummary.length(), fixed.length(), pct);
    if (pct < 5) {
        Serial.println("[Review] WARNING: critic barely edited the draft (<5% change)");
    }
    return fixed;
}

// ─── factCheckSummary — third-pass laser-focused verification ───────────────
// The writer (pass 1) and reviewer (pass 2) cover most issues, but the
// reviewer is sometimes too gentle and leaves persistent errors.  This
// third pass is intentionally NARROW: it doesn't rewrite, it just
// checks specific high-impact categories one by one and applies
// surgical edits.  Cheap (~$0.005 with mini), adds ~30 s, catches
// what the broader review missed.
String factCheckSummary(const String& transcript, const String& reviewedSummary) {
    if (reviewedSummary.length() < 50) return reviewedSummary;

    String sys =
        "You are a fact-checker doing a final pass on a meeting summary.  "
        "Make targeted corrections only.  Do not rewrite or restructure.  "
        "Keep all wording you don't actively change.";

    String user;
    user.reserve(transcript.length() + reviewedSummary.length() + 4096);
    user += "You will receive a TRANSCRIPT (speech-to-text, may have errors) "
            "and a REVIEWED SUMMARY of it.  Run ONLY the following checks "
            "and apply surgical edits where needed.  Do not rewrite "
            "untouched text.  Output the corrected summary.\n\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "CHECK A — Place names\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "For every place / location mentioned in the summary, "
            "verify it fits the country and context being discussed.  "
            "If a place is in the wrong country (e.g. a Middle-Eastern "
            "city name appearing in a Japanese-context discussion), "
            "swap it for the similar-sounding correct place — using "
            "your world knowledge.  Speech-to-text systems consistently "
            "confuse phonetically similar place names from different "
            "regions.\n\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "CHECK B — Headings\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "Scan every '## ' heading.  If a heading contains any of "
            "these words: 'Introduction', 'Purpose', 'Overview', "
            "'Background', 'Welcome', 'Opening', 'Closing Remarks', "
            "'Conclusion', 'Next Steps', 'Wrap-Up', 'Final Thoughts', "
            "'Other', 'Miscellaneous' — REPLACE that heading with a "
            "content-driven name describing the actual topic of the "
            "section.  Look at the section body for the real subject.\n\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "CHECK C — Product / model names\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "For tech product names that look slightly off (missing "
            "hyphens, mangled model numbers, alternate spellings), "
            "normalise to the manufacturer's actual product name.  "
            "Use your knowledge of common dev-boards, chips, modules.\n\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "CHECK D — Company / event names\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "Companies and events: if the name appears to be a "
            "phonetic rendering of a real well-known entity in the "
            "same industry, normalise to the correct spelling.  Apply "
            "this even if the misspelling appears multiple times — "
            "ASR makes the same error consistently.\n\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
            "Output ONLY the corrected summary — same structure, same "
            "wording for anything you didn't change.  No preamble.\n\n"
            "---\n\nTRANSCRIPT:\n";
    user += transcript;
    user += "\n\n---\n\nREVIEWED SUMMARY:\n";
    user += reviewedSummary;
    user += "\n\n---\n\nNow output the fact-checked summary:";

    Serial.printf("[FactCheck] transcript %u + reviewed %u → checking...\n",
                  transcript.length(), reviewedSummary.length());
    String fixed = _gptCallRetry(sys, user, 16000, 180000);

    if (fixed.length() < 100 || fixed.startsWith("[")) {
        Serial.println("[FactCheck] failed or empty — keeping reviewed summary");
        return reviewedSummary;
    }
    size_t minLen = reviewedSummary.length() < fixed.length()
                    ? reviewedSummary.length() : fixed.length();
    size_t diff = 0;
    for (size_t i = 0; i < minLen; i++) {
        if (reviewedSummary[i] != fixed[i]) diff++;
    }
    int pct = minLen > 0 ? (int)((diff * 100) / minLen) : 0;
    Serial.printf("[FactCheck] OK — reviewed %u → final %u chars (%u%% changed)\n",
                  reviewedSummary.length(), fixed.length(), pct);
    return fixed;
}
