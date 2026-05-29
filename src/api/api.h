#pragma once
/*
 * api.h
 * ─────────────────────────────────────────────────────────────────
 * ElevenLabs speech-to-text, OpenAI GPT-4o-mini summarisation,
 * helper utilities, and the multipart HTTP stream class used for
 * WAV file uploads.
 * ─────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include "FS.h"

// ─── WiFi reconnect guard ──────────────────────────────────────────────────────
// Checks WiFi status and reconnects if dropped. Called before every HTTP op.
// Returns true if WiFi is connected (or successfully reconnected), false otherwise.
bool ensureWiFi();

// ─── ElevenLabs Scribe STT ────────────────────────────────────────────────────
// Upload WAV at <path> to ElevenLabs and return the transcript text.
// Retries up to 3 times with progressive back-off.
String transcribeAudio(const String& path);

// ─── OpenAI GPT-4o-mini Summarisation ────────────────────────────────────────
// Generate a rolling (isFinal=false) or final (isFinal=true) summary.
// Retries up to 3 times with progressive back-off.
String generateSummary(const String& transcript, bool isFinal);

// ─── Long-meeting (chunked / map-reduce) summarisation ───────────────────────
// For meetings that exceed the practical single-call budget we summarise the
// transcript in segments and then synthesise one comprehensive final summary
// from the segment summaries.  This lets a single ESP32 with bounded RAM
// summarise arbitrarily long meetings (4 h, 8 h, all-day) by never holding
// more than one segment at a time.

// Summarise one segment of a longer transcript.
// segNum/totalSeg are 1-indexed and only inform the prompt (so the model
// knows where this segment sits in the meeting timeline).
String generateSegmentSummary(const String& transcriptSegment,
                              int segNum, int totalSeg);

// Take the concatenated segment summaries and merge them into ONE polished
// final meeting summary with the standard 5-section structure.
String synthesizeFinalSummary(const String& combinedSegmentSummaries);

// ─── 2nd-pass quality review ────────────────────────────────────────────────
// Takes the draft summary + the original transcript and asks GPT to act as
// a critic: find ASR mishearings, boilerplate headings, contradictions,
// hallucinations and missed topics — then return the fully corrected
// summary in the same format.
String reviewAndFixSummary(const String& transcript, const String& draftSummary);

// ─── 3rd-pass fact-check ────────────────────────────────────────────────────
// Final verification pass — focused specifically on the high-impact issues
// that the writer + reviewer tend to miss:
//   1. Proper nouns that don't fit the meeting's geography / industry
//   2. Boilerplate headings ('Introduction', 'Closing Remarks', etc.)
//   3. Product / model names slightly mangled
// Takes the reviewed summary + transcript, runs one more sweep, returns
// the polished final.
String factCheckSummary(const String& transcript, const String& reviewedSummary);

// ─── JSON string escape ───────────────────────────────────────────────────────
// Escapes a String so it is safe to embed in a JSON value.
String jsonEscape(const String& s);

// ─── Multipart stream ─────────────────────────────────────────────────────────
// Streams  header + SD file + footer  as a single Arduino Stream so
// HTTPClient can POST multipart/form-data without buffering the whole file.
class MultipartUploadStream : public Stream {
    File   _file;
    String _header, _footer;
    size_t _pos, _hLen, _fLen, _fileLen, _total;
public:
    MultipartUploadStream(File f, const String& header, const String& footer);
    int    read()                          override;
    size_t readBytes(char* buf, size_t len)override;
    int    available()                     override;
    int    peek()                          override;
    void   flush()                         override;
    size_t write(uint8_t)                  override;
};
