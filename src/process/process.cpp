/*
 * process.cpp
 * ─────────────────────────────────────────────────────────────────
 * BUG FIXED (in addition to v3 fixes):
 *   The reset block at meeting end immediately cleared finalTranscriptText
 *   with `finalTranscriptText = ""` on the very next line after setting it.
 *   This meant the Transcript tab always showed nothing after a meeting ended.
 *
 *   Fix: finalTranscriptText and finalSummaryText are now preserved after
 *   meeting end so the UI can display them.  They are cleared by
 *   handleApiStart() when the NEXT meeting begins.
 *
 *   Also: wordCount and rollingSummary are now reset at meeting end so
 *   they don't bleed into the next meeting's display.
 * ─────────────────────────────────────────────────────────────────
 */

#include "process.h"
#include "../core/globals.h"
#include "../api/api.h"
#include "../time/ntp_time.h"
#include "FS.h"
#include "SD.h"

// ─── deleteDirRecursive ───────────────────────────────────────────────────────
void deleteDirRecursive(const String& path) {
    File dir = SD.open(path.c_str());
    if (!dir) return;

    File entry = dir.openNextFile();
    while (entry) {
        String entryPath = path + "/" + String(entry.name());
        if (entry.isDirectory()) {
            entry.close();
            deleteDirRecursive(entryPath);
        } else {
            entry.close();
            SD.remove(entryPath.c_str());
        }
        entry = dir.openNextFile();
    }
    dir.close();
    SD.rmdir(path.c_str());
    Serial.printf("[SD] Deleted: %s\n", path.c_str());
}

// ─── createMeetingDir ─────────────────────────────────────────────────────────
void createMeetingDir() {
    stampNow();
    wordCount = 0;

    meetingDir = meetingTimestamp.length() > 0
        ? "/meeting_" + meetingTimestamp
        : "/meeting_" + String(millis());

    bool ok = SD.mkdir(meetingDir.c_str());
    Serial.printf("[SD] Meeting dir: %s  (%s)\n",
                  meetingDir.c_str(), ok ? "OK" : "FAIL");
}

// ─── readFullTranscriptFromSD ────────────────────────────────────────────────
// The in-RAM `fullTranscript` is trimmed at MAX_TRANSCRIPT_RAM (12 KB) so the
// live UI doesn't blow up memory on long meetings.  That trim used to also
// silently truncate the final-summary input, which is why 40-minute meetings
// were getting summaries that only covered the LAST 10-12 minutes.
//
// Now: every chunk transcript is appended to `full_transcript.txt` on the SD
// card (see the FILE_APPEND write in processTask).  At final-summary time we
// read that complete file and pass it to GPT instead of the trimmed in-RAM
// version, so the whole meeting is summarised — not just the tail.
//
// For very long meetings (>60 KB transcript, roughly >80 minutes), we keep
// the first 30 KB + last 30 KB joined with a clearly marked elision in the
// middle.  This preserves both the opening context and the closing decisions
// while staying within ESP32 RAM and OpenAI request-size budgets.
static String readFullTranscriptFromSD(const String& dir) {
    String result;
    String path = dir + "/full_transcript.txt";
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) {
        Serial.println("[ProcessTask] full_transcript.txt missing on SD — falling back to in-RAM transcript");
        return result;
    }

    size_t fsize = f.size();
    Serial.printf("[ProcessTask] full_transcript.txt = %u bytes on SD\n", (unsigned)fsize);

    const size_t SOFT_CAP = 60000;       // ≈ 75-80 min of typical speech
    const size_t HEAD_KEEP = 30000;
    const size_t TAIL_KEEP = 30000;

    char buf[256];

    if (fsize <= SOFT_CAP) {
        result.reserve(fsize + 1);
        while (f.available()) {
            int n = f.readBytes(buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            result += buf;
        }
    } else {
        // Long meeting — keep both ends, elide the middle.
        Serial.printf("[ProcessTask] Transcript > %u KB — keeping head + tail, eliding middle\n",
                      (unsigned)(SOFT_CAP / 1024));
        result.reserve(HEAD_KEEP + TAIL_KEEP + 200);

        // Read first HEAD_KEEP bytes
        size_t got = 0;
        while (f.available() && got < HEAD_KEEP) {
            size_t want = min((size_t)sizeof(buf) - 1, HEAD_KEEP - got);
            int n = f.readBytes(buf, want);
            if (n <= 0) break;
            buf[n] = '\0';
            result += buf;
            got += n;
        }

        result += "\n\n[ ... middle portion of the transcript omitted to keep the summary request within size limits — the meeting continued without interruption ... ]\n\n";

        // Seek to (fsize - TAIL_KEEP) and read to end
        f.seek(fsize - TAIL_KEEP);
        while (f.available()) {
            int n = f.readBytes(buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            result += buf;
        }
    }
    f.close();

    Serial.printf("[ProcessTask] Loaded %u chars from SD transcript\n", (unsigned)result.length());
    return result;
}

// ─── saveSummaryToSD — always writes summary_final.txt ───────────────────────
// This is the filename the /api/history endpoint reads.
// Also writes a timestamped copy for human browsing on the card.
static void saveSummaryToSD(const String& summary) {
    // Primary: summary_final.txt  ← history endpoint reads THIS
    File f1 = SD.open(meetingDir + "/summary_final.txt", FILE_WRITE);
    if (f1) { f1.print(summary); f1.close(); }
    else     { Serial.println("[SD] WARNING: could not write summary_final.txt"); }

    // Optional: timestamped copy for human readability on card
    if (meetingTimestamp.length() > 0) {
        File f2 = SD.open(meetingDir + "/summary_" + meetingTimestamp + ".txt", FILE_WRITE);
        if (f2) { f2.print(summary); f2.close(); }
    }
}

// ─── processTask (Core 1) ─────────────────────────────────────────────────────
void processTask(void* pv) {
    Serial.println("[ProcessTask] Started on core " + String(xPortGetCoreID()));

    for (;;) {
        // ── 1. Pick up a ready chunk ────────────────────────────────────────
        // Block for up to 100 ms so the task yields instead of spin-polling.
        // xQueueReceive is thread-safe — no extra mutex needed.
        bool   doProcess = false;
        String path      = "";
        char   pathBuf[CHUNK_PATH_LEN];
        if (xQueueReceive(chunkQueue, pathBuf, pdMS_TO_TICKS(100)) == pdTRUE) {
            path      = String(pathBuf);
            doProcess = true;
        }

        // ── 2. Process chunk ────────────────────────────────────────────────
        if (doProcess) {
            Serial.println("\n[ProcessTask] ─── Processing: " + path + " ───");

            File chk = SD.open(path.c_str(), FILE_READ);
            uint32_t fsize = chk ? chk.size() : 0;
            if (chk) chk.close();
            Serial.printf("[ProcessTask] File size: %u bytes\n", fsize);

            if (fsize <= WAV_HEADER_SIZE) {
                Serial.println("[ProcessTask] SKIP: file too small.");
                goto process_done;
            }

            {
                Serial.println("[ProcessTask] Calling ElevenLabs STT...");
                String transcript = transcribeAudio(path);
                Serial.println("[ProcessTask] Transcript: " + transcript);

                // Save per-chunk transcript
                String txtPath = path;
                txtPath.replace(".wav", ".txt");
                File tf = SD.open(txtPath.c_str(), FILE_WRITE);
                if (tf) { tf.println(transcript); tf.close(); }

                bool hasContent = transcript.length() > 3
                               && !transcript.startsWith("[STT failed")
                               && !transcript.startsWith("[no speech");

                if (hasContent) {
                    // ── Critical section: update shared transcript state ──
                    // Hold the lock only for the in-RAM string mutations,
                    // then snapshot for the slow SD + HTTP work below.
                    String transcriptSnap;
                    int    curWordCount;
                    xSemaphoreTake(stateMutex, portMAX_DELAY);
                    fullTranscript += transcript + "\n";

                    // Word-count fix: count word-START transitions so the
                    // first word is also counted ("hello world" → 2, not 1).
                    if (transcript.length() > 0 && transcript[0] != ' ') wordCount++;
                    for (size_t i = 1; i < transcript.length(); i++)
                        if (transcript[i] != ' ' && transcript[i-1] == ' ') wordCount++;

                    if ((int)fullTranscript.length() > MAX_TRANSCRIPT_RAM) {
                        int cutAt = (int)fullTranscript.length() - MAX_TRANSCRIPT_RAM;
                        int nl = fullTranscript.indexOf('\n', cutAt);
                        if (nl > 0) cutAt = nl + 1;
                        fullTranscript.remove(0, cutAt);
                        Serial.println("[ProcessTask] Transcript trimmed.");
                    }
                    transcriptSnap = fullTranscript;
                    curWordCount   = wordCount;
                    xSemaphoreGive(stateMutex);

                    // APPEND just this chunk's transcript to the running full file.
                    // FILE_APPEND opens in "a" mode so the file grows naturally
                    // chunk-by-chunk and stays complete even when the in-RAM
                    // `fullTranscript` gets trimmed for memory.  This is what
                    // the final-summary step reads back.
                    File ff = SD.open(meetingDir + "/full_transcript.txt", FILE_APPEND);
                    if (ff) {
                        ff.print(transcript);
                        ff.print('\n');
                        ff.close();
                    }

                    // Rolling summary (only after 20+ words to avoid trivial summaries)
                    if (curWordCount >= 20) {
                        Serial.println("[ProcessTask] Calling GPT for rolling summary...");
                        String summary = generateSummary(transcriptSnap, false);

                        bool valid = summary.length() > 60
                                  && !summary.startsWith("[")
                                  && !summary.startsWith("⚠");
                        if (valid) {
                            xSemaphoreTake(stateMutex, portMAX_DELAY);
                            rollingSummary = summary;
                            xSemaphoreGive(stateMutex);
                            Serial.println("\n┌────────── ROLLING SUMMARY ──────────────┐");
                            Serial.println(summary);
                            Serial.println("└─────────────────────────────────────────┘\n");
                            File sf = SD.open(meetingDir + "/summary.txt", FILE_WRITE);
                            if (sf) { sf.print(summary); sf.close(); }
                        } else {
                            Serial.println("[ProcessTask] Rolling summary rejected: " + summary.substring(0, 80));
                        }
                    } else {
                        Serial.printf("[ProcessTask] Skipping rolling summary — only %d words so far.\n", curWordCount);
                    }
                }
            }
            process_done:;
        }

        // ── 3. Final summary when meeting stops ─────────────────────────────
        // Wait until all queued chunks have been drained before summarising.
        if (finalStop && !meetingActive && uxQueueMessagesWaiting(chunkQueue) == 0) {
            finalStop = false;
            Serial.println("\n[ProcessTask] Meeting stopped — generating FINAL summary...");

            // CRITICAL: read the COMPLETE transcript from SD, not the trimmed
            // in-RAM one.  The in-RAM `fullTranscript` is capped at
            // MAX_TRANSCRIPT_RAM (12 KB) so a 40-minute meeting loses its first
            // 30 minutes from RAM — but SD has the full thing.
            String transcriptSnap = readFullTranscriptFromSD(meetingDir);

            // Fallback to in-RAM version if SD read failed for any reason
            // (card pulled, file corruption, etc.) so we still produce SOMETHING.
            if (transcriptSnap.length() < 10) {
                Serial.println("[ProcessTask] SD transcript unusable — falling back to in-RAM (last ~12 KB only)");
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                transcriptSnap = fullTranscript;
                xSemaphoreGive(stateMutex);
            }

            // Store the full transcript for the UI's Transcript tab.
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            finalTranscriptText = transcriptSnap;
            xSemaphoreGive(stateMutex);

            if (transcriptSnap.length() > 10) {
                Serial.printf("[ProcessTask] Sending %u chars to GPT for final summary\n",
                              (unsigned)transcriptSnap.length());
                String finalSum = generateSummary(transcriptSnap, true);

                bool valid = finalSum.length() > 80
                          && !finalSum.startsWith("[")
                          && !finalSum.startsWith("⚠");

                String finalSummarySnap;
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                if (valid) {
                    finalSummaryText = finalSum;
                } else {
                    Serial.println("[ProcessTask] Final summary invalid — using rolling summary.");
                    finalSummaryText = rollingSummary.length() > 60
                        ? rollingSummary
                        : "Meeting recorded but summary failed. Check your OpenAI API key.";
                }
                finalSummarySnap = finalSummaryText;
                xSemaphoreGive(stateMutex);

                Serial.println("\n╔══════════════════════════════════════════╗");
                Serial.println("║         FINAL MEETING SUMMARY           ║");
                Serial.println("╚══════════════════════════════════════════╝");
                Serial.println(finalSummarySnap);
                Serial.println("════════════════════════════════════════════\n");

                // SD write outside the lock — uses the snapshot, not the live String.
                saveSummaryToSD(finalSummarySnap);
                Serial.println("[ProcessTask] Final summary saved to SD.");

            } else {
                Serial.println("[ProcessTask] Not enough transcript for final summary.");
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                finalSummaryText    = "Not enough speech was recorded for a summary.";
                finalTranscriptText = fullTranscript;
                xSemaphoreGive(stateMutex);
            }

            // Reset working state for next meeting.
            // NOTE: finalTranscriptText and finalSummaryText are intentionally
            // kept alive so the Transcript/Summary tabs remain readable until
            // the NEXT meeting starts (handleApiStart clears them).
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            fullTranscript = "";
            rollingSummary = "";
            chunkIndex     = 0;
            wordCount      = 0;
            xSemaphoreGive(stateMutex);
        }

        // No extra vTaskDelay — xQueueReceive already blocks for 100 ms.
    }
}