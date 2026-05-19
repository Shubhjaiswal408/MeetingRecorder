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
        bool   doProcess = false;
        String path      = "";

        if (xSemaphoreTake(chunkMutex, 0) == pdTRUE) {
            if (chunkReady) {
                path       = currentChunkPath;
                chunkReady = false;
                doProcess  = true;
            }
            xSemaphoreGive(chunkMutex);
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

                    // Slow I/O outside the lock — uses snapshot, not the live String
                    File ff = SD.open(meetingDir + "/full_transcript.txt", FILE_WRITE);
                    if (ff) { ff.print(transcriptSnap); ff.close(); }

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
        if (finalStop && !meetingActive && !chunkReady) {
            finalStop = false;
            Serial.println("\n[ProcessTask] Meeting stopped — generating FINAL summary...");

            // Snapshot the transcript under lock, then do slow HTTP outside it.
            String transcriptSnap;
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            finalTranscriptText = fullTranscript;   // freeze for the UI before reset
            transcriptSnap      = fullTranscript;
            xSemaphoreGive(stateMutex);

            if (transcriptSnap.length() > 10) {
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

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}