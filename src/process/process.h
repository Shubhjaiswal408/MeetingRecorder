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
// Waits for chunkReady signal, transcribes audio, updates
// rollingSummary.  On finalStop generates the final summary.
void processTask(void* pv);

// Create a new timestamped meeting directory on the SD card.
// Sets the global meetingDir variable.
void createMeetingDir();

// Recursively delete all files in <path> then remove the directory.
// Used by the web history delete endpoint.
void deleteDirRecursive(const String& path);
