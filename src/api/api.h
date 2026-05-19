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
