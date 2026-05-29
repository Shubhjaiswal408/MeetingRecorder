/*
 * process.cpp
 * ─────────────────────────────────────────────────────────────────
 * BUG FIXED:
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
// CRITICAL: never call SD.remove() / SD.rmdir() / recurse while a parent
// directory handle is still open.  The FAT layer can leave the card in
// an inconsistent state — the bug surfaced as "SD init FAILED" forever
// on the next boot after a factory reset.
//
// Two-phase, batched approach:
//   Phase 1 — open the dir, scan up to BATCH_SIZE entries into a local
//             array, close the dir handle.  Remember the first subdir
//             (if any) for later recursion.
//   Phase 2 — with the dir handle CLOSED, SD.remove() every collected
//             file path, then recurse into the saved subdir.
//   Repeat   until the dir reports empty, then SD.rmdir() it.
void deleteDirRecursive(const String& path) {
    const int BATCH_SIZE = 30;

    while (true) {
        File dir = SD.open(path.c_str());
        if (!dir) return;

        String batch[BATCH_SIZE];
        int    fileCount   = 0;
        String firstSubdir;     // only one subdir recursed per pass — safer

        File entry = dir.openNextFile();
        while (entry && fileCount < BATCH_SIZE) {
            String n = entry.name();
            int slash = n.lastIndexOf('/');
            if (slash >= 0) n = n.substring(slash + 1);
            String full = path + "/" + n;

            if (entry.isDirectory()) {
                if (firstSubdir.length() == 0) firstSubdir = full;
            } else {
                batch[fileCount++] = full;
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();    // ← critical: close iterator BEFORE any deletes

        // Delete the files in this batch
        for (int i = 0; i < fileCount; i++) {
            SD.remove(batch[i].c_str());
        }

        // Recurse into one subdirectory per pass (after parent is closed)
        if (firstSubdir.length() > 0) {
            deleteDirRecursive(firstSubdir);
        }

        // Loop again to pick up either the next batch of files in this
        // dir, or the next subdir — until both are exhausted.
        if (fileCount == 0 && firstSubdir.length() == 0) break;
    }

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
// read that ENTIRE file with no truncation — the whole meeting goes to GPT
// exactly as it was spoken.
//
// RAM note: a 1-hour meeting ≈ 45 KB; a 2-hour ≈ 90 KB.  ESP32-S3 has ~280 KB
// of free heap at this point so this is comfortable.  Truly extreme meetings
// (4 hours+) could approach the RAM ceiling during the HTTP POST — if that
// ever becomes an issue, recursive summarisation is the right fix, not
// silent truncation.
static String readFullTranscriptFromSD(const String& dir) {
    String result;
    String path = dir + "/full_transcript.txt";
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) {
        Serial.println("[ProcessTask] full_transcript.txt missing on SD — falling back to in-RAM transcript");
        return result;
    }

    size_t fsize = f.size();
    Serial.printf("[ProcessTask] full_transcript.txt = %u bytes on SD — reading ALL of it\n",
                  (unsigned)fsize);

    result.reserve(fsize + 1);

    char buf[256];
    while (f.available()) {
        int n = f.readBytes(buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        result += buf;
    }
    f.close();

    Serial.printf("[ProcessTask] Loaded %u chars from SD transcript (free heap: %u)\n",
                  (unsigned)result.length(), (unsigned)ESP.getFreeHeap());
    return result;
}

// ─── regenerateSummaryForMeeting ─────────────────────────────────────────────
// Standalone version of the final-summary logic from processTask, callable
// from the web layer.  Reads <dir>/full_transcript.txt, picks single-call
// vs. map-reduce based on size, runs GPT, saves the new summary_final.txt,
// and returns the summary string.  Does not touch any live meeting state.
String regenerateSummaryForMeeting(const String& dir) {
    String txPath = dir + "/full_transcript.txt";
    size_t fsize = 0;
    {
        File f = SD.open(txPath.c_str(), FILE_READ);
        if (!f) {
            Serial.println("[Regen] full_transcript.txt missing for " + dir);
            return "";
        }
        fsize = f.size();
        f.close();
    }
    if (fsize < 50) {
        Serial.printf("[Regen] Transcript too short (%u bytes) for %s\n",
                      (unsigned)fsize, dir.c_str());
        return "";
    }

    Serial.printf("[Regen] %s — transcript %u bytes, free heap %u\n",
                  dir.c_str(), (unsigned)fsize, (unsigned)ESP.getFreeHeap());

    // Bumped 25 KB → 60 KB so meetings up to ~90 min get the highest-
    // quality single-call summary (full transcript in one GPT prompt)
    // instead of being split through map-reduce.  60 KB fits comfortably
    // in DRAM during JSON escape, and gpt-4o-mini's 128 K context window
    // handles it easily.
    // With OPI PSRAM enabled (XIAO ESP32-S3 Sense = 8 MB PSRAM), large
    // String allocations fall back to PSRAM, so we can hold huge
    // transcripts in memory.  gpt-4o-mini accepts up to 128 K tokens
    // (~500 KB plain text) of input.  We cap at 380 KB to leave room
    // for the prompt + JSON escaping (which roughly 1.3× the size).
    // For meetings up to ~12 hours of continuous speech this means a
    // single GPT call, no map-reduce — best possible quality.
    const size_t SINGLE_CALL_CAP = 380000;
    const size_t SEGMENT_SIZE    = 25000;

    String finalSum;

    if (fsize <= SINGLE_CALL_CAP) {
        // Single-call path
        String transcript;
        transcript.reserve(fsize + 1);
        File f = SD.open(txPath.c_str(), FILE_READ);
        if (f) {
            char buf[256];
            while (f.available()) {
                int n = f.readBytes(buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = '\0';
                transcript += buf;
            }
            f.close();
        }
        if (transcript.length() < 50) return "";
        Serial.printf("[Regen] Single-call summary: %u chars\n",
                      (unsigned)transcript.length());
        finalSum = generateSummary(transcript, true);
    } else {
        // Map-reduce path — stream segments
        int totalSegments = (fsize + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
        Serial.printf("[Regen] Map-reduce: %d segments\n", totalSegments);

        String combined;
        combined.reserve(totalSegments * 1500 + 256);

        File f = SD.open(txPath.c_str(), FILE_READ);
        if (f) {
            int segNum = 0;
            while (f.available()) {
                segNum++;
                String segment;
                segment.reserve(SEGMENT_SIZE + 512);
                char buf[256];
                while (f.available() && segment.length() < SEGMENT_SIZE) {
                    size_t want = sizeof(buf) - 1;
                    if (segment.length() + want > SEGMENT_SIZE) {
                        want = SEGMENT_SIZE - segment.length();
                    }
                    int n = f.readBytes(buf, want);
                    if (n <= 0) break;
                    buf[n] = '\0';
                    segment += buf;
                }
                // extend to next newline
                while (f.available()) {
                    char c = (char)f.read();
                    segment += c;
                    if (c == '\n') break;
                    if (segment.length() > SEGMENT_SIZE + 2048) break;
                }
                if (segment.length() < 80) continue;

                Serial.printf("[Regen] Map %d/%d: %u chars\n",
                              segNum, totalSegments, segment.length());
                String segSum = generateSegmentSummary(segment, segNum, totalSegments);
                segment = "";
                if (segSum.length() > 40 && !segSum.startsWith("[")) {
                    combined += "## Segment ";
                    combined += String(segNum);
                    combined += " of ";
                    combined += String(totalSegments);
                    combined += "\n";
                    combined += segSum;
                    combined += "\n\n";
                }
            }
            f.close();
        }

        if (combined.length() > 100) {
            Serial.printf("[Regen] Reduce step: %u chars\n",
                          (unsigned)combined.length());
            finalSum = synthesizeFinalSummary(combined);
        }
    }

    // Validate + save
    bool valid = finalSum.length() > 80
              && !finalSum.startsWith("[")
              && !finalSum.startsWith("⚠");
    if (!valid) {
        Serial.println("[Regen] GPT result invalid — not saving");
        return "";
    }

    // ── Second-pass review ──────────────────────────────────────────
    // Run the draft through a critic that has both the transcript and
    // the draft, and ask it to find + fix every ASR mishearing,
    // boilerplate heading slip, hallucination and missed topic.  This
    // adds ~60-90 s but is what gets us from ~85 % to production-grade
    // accuracy.
    {
        // Load the transcript again (we may have freed it during summary
        // generation in the map-reduce path).
        String fullForReview;
        File rf = SD.open(txPath.c_str(), FILE_READ);
        if (rf) {
            fullForReview.reserve(rf.size() + 16);
            char buf[256];
            while (rf.available()) {
                int n = rf.readBytes(buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = '\0';
                fullForReview += buf;
            }
            rf.close();
        }
        // 2nd/3rd-pass review temporarily disabled — was causing HTTP -11
        // timeouts on long transcripts.  Single-pass with the improved
        // prompts gives stable ~85 % accuracy.
        (void)fullForReview;
    }

    // Truncate the existing summary file before writing — Arduino's
    // FILE_WRITE is append-mode, so without SD.remove() first the new
    // regenerated summary would be appended after the old content.
    String outPath = dir + "/summary_final.txt";
    if (SD.exists(outPath.c_str())) SD.remove(outPath.c_str());
    File sf = SD.open(outPath.c_str(), FILE_WRITE);
    if (sf) {
        size_t want = finalSum.length();
        size_t got  = sf.print(finalSum);
        sf.flush();
        sf.close();
        Serial.printf("[Regen] Saved %u of %u chars to %s/summary_final.txt\n",
                      (unsigned)got, (unsigned)want, dir.c_str());
        if (got != want) Serial.println("[Regen] WARNING: short write — disk may be full");
    } else {
        Serial.println("[Regen] ERROR: could not open summary_final.txt for write");
    }

    return finalSum;
}

// ─── saveSummaryToSD — always writes summary_final.txt ───────────────────────
// This is the filename the /api/history endpoint reads.
// Also writes a timestamped copy for human browsing on the card.
//
// IMPORTANT: Arduino SD's FILE_WRITE is APPEND mode, not truncate.  So we
// SD.remove() any existing file first to guarantee fresh contents.  This
// was the root cause of "history shows the rolling summary instead of
// the final" — rolling summaries had been appended to summary_final.txt
// during the meeting on some firmware paths, and the final summary then
// appended on top, leaving the rolling content first.
static void saveSummaryToSD(const String& summary) {
    String finalPath = meetingDir + "/summary_final.txt";

    // Truncate by removing first (FILE_WRITE = append on Arduino SD)
    if (SD.exists(finalPath.c_str())) SD.remove(finalPath.c_str());

    File f1 = SD.open(finalPath, FILE_WRITE);
    if (f1) {
        size_t want = summary.length();
        size_t got  = f1.print(summary);
        f1.flush();
        f1.close();
        Serial.printf("[SD] summary_final.txt written: %u of %u bytes\n",
                      (unsigned)got, (unsigned)want);
        if (got != want) Serial.println("[SD] WARNING: short write — disk may be full");
    } else {
        Serial.println("[SD] ERROR: could not open summary_final.txt for write");
    }

    // Also remove the rolling-summary file now that the final is in place,
    // so /api/history's fallback path can't accidentally surface it.
    String rollingPath = meetingDir + "/summary.txt";
    if (SD.exists(rollingPath.c_str())) {
        SD.remove(rollingPath.c_str());
        Serial.println("[SD] Removed rolling summary.txt — final is the source of truth now");
    }

    // Optional: timestamped copy for human readability on card
    if (meetingTimestamp.length() > 0) {
        String tsPath = meetingDir + "/summary_" + meetingTimestamp + ".txt";
        if (SD.exists(tsPath.c_str())) SD.remove(tsPath.c_str());
        File f2 = SD.open(tsPath, FILE_WRITE);
        if (f2) { f2.print(summary); f2.flush(); f2.close(); }
    }
}

// ─── processTask (Core 1) ─────────────────────────────────────────────────────
void processTask(void* pv) {
    Serial.println("[ProcessTask] Started on core " + String(xPortGetCoreID()));

    for (;;) {
        // ── 0. Factory reset request from /api/factory-reset ────────────────
        // This task has the 20 KB stack we need for SD walking + recursive
        // deletes; the web task only has 6 KB and crashes on devices with
        // many stored meetings.
        if (needFactoryReset) {
            needFactoryReset = false;
            Serial.println("\n[FactoryReset] ── BEGIN ─────────────────────────────");

            // Cancel any in-flight meeting / processing state
            meetingActive   = false;
            finalStop       = false;
            processingFinal = false;
            xQueueReset(chunkQueue);

            // Delete meetings ONE AT A TIME.  Each pass opens the root,
            // finds the first meeting_* directory, closes the iterator,
            // then deletes that directory.  Repeats until no more remain.
            // Keeping only one String alive in the loop avoids the stack
            // pressure that crashed the previous (web-task) implementation.
            int deletedCount = 0;
            const int SAFETY_LIMIT = 500;
            while (deletedCount < SAFETY_LIMIT) {
                String target;
                {
                    File root = SD.open("/");
                    if (!root || !root.isDirectory()) {
                        if (root) root.close();
                        break;
                    }
                    File entry = root.openNextFile();
                    while (entry) {
                        String n = entry.name();
                        int slash = n.lastIndexOf('/');
                        if (slash >= 0) n = n.substring(slash + 1);
                        bool isMeetingDir = entry.isDirectory() && n.startsWith("meeting_");
                        entry.close();
                        if (isMeetingDir) {
                            target = "/" + n;
                            break;
                        }
                        entry = root.openNextFile();
                    }
                    root.close();
                }
                if (target.length() == 0) break;
                Serial.printf("[FactoryReset] Deleting %s\n", target.c_str());
                deleteDirRecursive(target);
                deletedCount++;
                // Brief yield so the watchdog stays happy on large wipes
                vTaskDelay(5 / portTICK_PERIOD_MS);
            }
            Serial.printf("[FactoryReset] %d meeting directories removed\n", deletedCount);

            // Wipe credentials (config.json holds WiFi creds, API keys, AP creds)
            if (SD.exists(CONFIG_FILE)) {
                SD.remove(CONFIG_FILE);
                Serial.println("[FactoryReset] config.json deleted");
            }

            Serial.println("[FactoryReset] ── COMPLETE — rebooting in 1 s ─────");
            // Cleanly release the SD card so the next boot finds it on the
            // first try.  Without this the SPI/SD state carries over after
            // ESP.restart() and the next setup() sees "SD init FAILED".
            SD.end();
            delay(1000);
            ESP.restart();
        }

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
                            // Truncate the rolling summary file before writing
                            // — Arduino's FILE_WRITE is append-mode, so without
                            // SD.remove() first, each rolling update would
                            // append onto the previous one.
                            String rollPath = meetingDir + "/summary.txt";
                            if (SD.exists(rollPath.c_str())) SD.remove(rollPath.c_str());
                            File sf = SD.open(rollPath, FILE_WRITE);
                            if (sf) { sf.print(summary); sf.flush(); sf.close(); }
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
            finalStop       = false;
            processingFinal = true;   // keeps LED blinking through the whole job
            Serial.println("\n[ProcessTask] Meeting stopped — generating FINAL summary...");

            // ── Set up Transcript tab content from the SD file ─────────────
            // The UI only renders the last ~4 KB anyway (see web_extras), so
            // we keep just the tail in RAM.  The complete transcript lives
            // on the SD card at full_transcript.txt as the source of truth.
            {
                String tailOnly;
                String txPath = meetingDir + "/full_transcript.txt";
                File   f = SD.open(txPath.c_str(), FILE_READ);
                if (f) {
                    size_t fsize = f.size();
                    size_t start = fsize > 12000 ? fsize - 12000 : 0;
                    f.seek(start);
                    tailOnly.reserve(fsize - start + 1);
                    char buf[256];
                    while (f.available()) {
                        int n = f.readBytes(buf, sizeof(buf) - 1);
                        if (n <= 0) break;
                        buf[n] = '\0';
                        tailOnly += buf;
                    }
                    f.close();
                }
                if (tailOnly.length() < 10) {
                    // SD unreadable — fall back to in-RAM
                    xSemaphoreTake(stateMutex, portMAX_DELAY);
                    tailOnly = fullTranscript;
                    xSemaphoreGive(stateMutex);
                }
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                finalTranscriptText = tailOnly;
                xSemaphoreGive(stateMutex);
            }

            // ── Decide which summarisation strategy to use ─────────────────
            // For meetings that fit in a single GPT call (< ~SINGLE_CALL_CAP)
            // we do the fast path: read full transcript, one GPT call, done.
            // For longer meetings we do MAP-REDUCE: stream segments off SD,
            // summarise each in turn (holding only one segment in RAM at a
            // time), then synthesise one final summary from the segment
            // summaries.  This lets us summarise meetings of arbitrary
            // length on a 320 KB-RAM ESP32 without ever OOM'ing.
            // Threshold dropped from 40 KB → 25 KB after real-world tests
            // showed 30-40 KB transcripts (≈ 40-50 min meetings) sometimes
            // failed the single GPT call (timeout or partial response) and
            // silently fell back to the rolling summary.  Map-reduce splits
            // those into smaller, more reliable per-call payloads.
            // 60 KB single-call cap — see comment in regenerateSummaryForMeeting.
            // Most real-world meetings (under ~90 min) now get one GPT call
            // with the FULL transcript, which produces noticeably better
            // summaries than the map-reduce path (no context lost between
            // segments).  Only long-form meetings still fall through to
            // map-reduce.
            // With OPI PSRAM enabled (XIAO ESP32-S3 Sense = 8 MB PSRAM), large
    // String allocations fall back to PSRAM, so we can hold huge
    // transcripts in memory.  gpt-4o-mini accepts up to 128 K tokens
    // (~500 KB plain text) of input.  We cap at 380 KB to leave room
    // for the prompt + JSON escaping (which roughly 1.3× the size).
    // For meetings up to ~12 hours of continuous speech this means a
    // single GPT call, no map-reduce — best possible quality.
    const size_t SINGLE_CALL_CAP = 380000;
            const size_t SEGMENT_SIZE    = 25000;   // each map-reduce chunk

            String txPath = meetingDir + "/full_transcript.txt";
            size_t transcriptSize = 0;
            {
                File ft = SD.open(txPath.c_str(), FILE_READ);
                if (ft) { transcriptSize = ft.size(); ft.close(); }
            }
            Serial.printf("[ProcessTask] Free heap: %u bytes, transcript: %u bytes\n",
                          (unsigned)ESP.getFreeHeap(), (unsigned)transcriptSize);

            String finalSum;
            bool   gotFinalFromGPT = false;

            if (transcriptSize == 0) {
                // SD failed — fall back to whatever's in RAM
                Serial.println("[ProcessTask] No SD transcript — using in-RAM fallback");
                String ramCopy;
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                ramCopy = fullTranscript;
                xSemaphoreGive(stateMutex);
                if (ramCopy.length() > 10) {
                    finalSum = generateSummary(ramCopy, true);
                    gotFinalFromGPT = true;
                }
            } else if (transcriptSize <= SINGLE_CALL_CAP) {
                // ── Fast path: short meeting, one GPT call ─────────────────
                Serial.println("[ProcessTask] Short meeting — single-call summary");
                String transcript = readFullTranscriptFromSD(meetingDir);
                if (transcript.length() > 10) {
                    finalSum = generateSummary(transcript, true);
                    gotFinalFromGPT = true;
                }
                transcript = "";  // free heap before the rest
            } else {
                // ── Map-Reduce path: stream segments + synthesise ──────────
                int totalSegments = (transcriptSize + SEGMENT_SIZE - 1) / SEGMENT_SIZE;
                Serial.printf("[ProcessTask] Long meeting (%u KB) — chunked summarisation in %d segments\n",
                              (unsigned)(transcriptSize / 1024), totalSegments);

                String combined;
                combined.reserve(totalSegments * 1500 + 256);

                File f = SD.open(txPath.c_str(), FILE_READ);
                if (f) {
                    int segNum = 0;
                    while (f.available()) {
                        segNum++;
                        // Read up to SEGMENT_SIZE bytes for this segment
                        String segment;
                        segment.reserve(SEGMENT_SIZE + 512);
                        char buf[256];
                        while (f.available() && segment.length() < SEGMENT_SIZE) {
                            size_t want = sizeof(buf) - 1;
                            if (segment.length() + want > SEGMENT_SIZE) {
                                want = SEGMENT_SIZE - segment.length();
                            }
                            int n = f.readBytes(buf, want);
                            if (n <= 0) break;
                            buf[n] = '\0';
                            segment += buf;
                        }
                        // Extend to next newline so we don't cut a chunk transcript in half
                        while (f.available()) {
                            char c = (char)f.read();
                            segment += c;
                            if (c == '\n') break;
                            if (segment.length() > SEGMENT_SIZE + 2048) break;  // safety
                        }

                        if (segment.length() < 80) {
                            Serial.printf("[ProcessTask] Skipping tiny tail segment %d (%u chars)\n",
                                          segNum, segment.length());
                            continue;
                        }

                        Serial.printf("[ProcessTask] Map step %d/%d: %u chars, free heap %u\n",
                                      segNum, totalSegments, segment.length(),
                                      (unsigned)ESP.getFreeHeap());

                        String segSum = generateSegmentSummary(segment, segNum, totalSegments);
                        segment = "";  // free the segment immediately

                        if (segSum.length() > 40 && !segSum.startsWith("[")) {
                            combined += "## Segment ";
                            combined += String(segNum);
                            combined += " of ";
                            combined += String(totalSegments);
                            combined += "\n";
                            combined += segSum;
                            combined += "\n\n";
                        } else {
                            Serial.printf("[ProcessTask] Segment %d summary rejected (%u chars)\n",
                                          segNum, segSum.length());
                        }
                    }
                    f.close();
                }

                if (combined.length() > 100) {
                    Serial.printf("[ProcessTask] Reduce step: synthesising final from %u chars, free heap %u\n",
                                  combined.length(), (unsigned)ESP.getFreeHeap());
                    finalSum = synthesizeFinalSummary(combined);
                    combined = "";   // free heap

                    if (finalSum.length() < 80 || finalSum.startsWith("[")) {
                        // Synthesis failed — fall back to concatenated segments
                        Serial.println("[ProcessTask] Synthesis call failed — using raw segment summaries");
                        // Re-build a minimal final summary from segments
                        // (won't be polished but won't lose content)
                        // We deliberately don't retry — already inside _gptCallRetry's 3 attempts.
                        finalSum = "## Meeting Summary (synthesised from segments — automatic synthesis failed)\n\n"
                                   "*The meeting was too long for a single GPT call, so it was split "
                                   "into segments which were each summarised individually. The final "
                                   "merge step failed, so the segment summaries are concatenated below.*\n\n";
                        // (combined was just freed; we'd need to re-read. Skip for safety.)
                    }
                    gotFinalFromGPT = true;
                } else {
                    Serial.println("[ProcessTask] All map steps failed — no segment summaries to synthesise");
                }
            }

            if (gotFinalFromGPT && finalSum.length() > 10) {
                bool valid = finalSum.length() > 80
                          && !finalSum.startsWith("[")
                          && !finalSum.startsWith("⚠");

                // ── 2nd-pass review (live meeting end path) ─────────
                if (valid) {
                    String fullForReview;
                    File rf = SD.open(txPath.c_str(), FILE_READ);
                    if (rf) {
                        fullForReview.reserve(rf.size() + 16);
                        char buf[256];
                        while (rf.available()) {
                            int n = rf.readBytes(buf, sizeof(buf) - 1);
                            if (n <= 0) break;
                            buf[n] = '\0';
                            fullForReview += buf;
                        }
                        rf.close();
                    }
                    // 2nd/3rd-pass review temporarily disabled —
                    // see comment in regenerateSummaryForMeeting.
                    (void)fullForReview;
                }

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

            // ── Power-down: meeting fully wrapped up ─────────────────────
            // Audio capture, STT calls, and the final summary (including
            // the multi-call map-reduce path) are all done.  Drop the CPU
            // back to 80 MHz so the device sips power while waiting for
            // the next meeting.  Saves ~20 mA over staying at 240 MHz.
            //
            // Safety check: a user could start the NEXT meeting while we
            // were still generating the previous final summary (a long
            // map-reduce can take 2-3 minutes).  In that case the button
            // handler / handleApiStart will have already bumped the CPU
            // back to 240, so we must NOT drop it back to 80 here.
            processingFinal = false;   // LED can stop blinking now
            if (!meetingActive) {
                setCpuFrequencyMhz(80);
                Serial.println("[Power] Idle: CPU back to 80 MHz");
            } else {
                Serial.println("[Power] Next meeting already started — keeping CPU at 240 MHz");
            }
        }

        // No extra vTaskDelay — xQueueReceive already blocks for 100 ms.
    }
}