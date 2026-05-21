#pragma once
#include <Arduino.h>
/*
 * process.h
 * ─────────────────────────────────────────────────────────────────
 * FreeRTOS process task (Core 1) — picks up recorded WAV chunks,
 * calls STT + GPT, and updates shared summary state.
 * Also owns the SD directory helpers.
 * ─────────────────────────────────────────────────────────────────
 */

// FreeRTOS task — pin to Core 1.
// Receives WAV paths from chunkQueue, transcribes audio, updates
// rollingSummary.  On finalStop drains the queue then generates the final summary.
void processTask(void* pv);

// Create a new timestamped meeting directory on the SD card.
// Sets the global meetingDir variable.
void createMeetingDir();

// Recursively delete all files in <path> then remove the directory.
// Used by the web history delete endpoint.
void deleteDirRecursive(const String& path);

// Regenerate the final summary for a past meeting by reading its
// `full_transcript.txt` from the SD card and re-running the same
// single-call / map-reduce logic the live `processTask` uses.  Returns
// the new summary string (also saved to summary_final.txt in the
// same directory) — or an empty String on failure.
//
// Used by the History tab's "Regenerate Summary" button so users can
// rescue meetings whose final-summary GPT call originally failed and
// fell back to the (much shorter) rolling summary.
String regenerateSummaryForMeeting(const String& meetingDirPath);
