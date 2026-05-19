#pragma once
/*
 * globals.h  (v2 — with NTP time, chat, and enhanced state)
 * ─────────────────────────────────────────────────────────────────
 * Central header included by every module.
 * Declares (extern) every shared variable so all .cpp files can
 * read/write the same state.  Definitions live in MeetingRecorder.ino
 * ─────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include <time.h>

// ─── AP hotspot (always on) ───────────────────────────────────────────────────
// Default values used when no config.json exists on the SD card.
#define AP_SSID_DEFAULT  "MeetingRecorder"
#define AP_PASS_DEFAULT  "recorder123"
#define CONFIG_FILE      "/config.json"

// Runtime AP credentials (saved to /config.json).
// Changed via the Settings UI without reflashing.
extern String apSSID;
extern String apPass;

// Set true by handleApiConfig() when AP settings change so loop() can
// restart softAP after the HTTP response is already sent.
extern volatile bool needApRestart;

// ─── Remote API endpoints ─────────────────────────────────────────────────────
extern const char* EL_STT_URL;   // ElevenLabs speech-to-text
extern const char* OPENAI_URL;   // OpenAI chat completions

// ─── Audio constants ─────────────────────────────────────────────────────────
#define SAMPLE_RATE        16000U
#define SAMPLE_BITS        16
#define WAV_HEADER_SIZE    44
#define VOLUME_GAIN        2
#define I2S_READ_CHUNK     1024
#define CHUNK_SECONDS      15

// ─── Pin assignments ─────────────────────────────────────────────────────────
#define I2S_NUM            I2S_NUM_0
#define PDM_CLK_GPIO       (gpio_num_t)42
#define PDM_DIN_GPIO       (gpio_num_t)41
#define SD_CS_PIN          21
#define BUTTON_PIN         D1   // D1 (GPIO1) with internal pull-up

// ─── Memory limit ─────────────────────────────────────────────────────────────
#define MAX_TRANSCRIPT_RAM 12000

// ─── Runtime WiFi + API credentials (saved to /config.json) ──────────────────
extern String wifiSSID;
extern String wifiPass;
extern String elApiKey;
extern String openaiApiKey;

// ─── Meeting state ────────────────────────────────────────────────────────────
extern volatile bool meetingActive;
extern volatile bool finalStop;
extern volatile bool needWifiReconnect;

extern String meetingDir;
extern String fullTranscript;
extern String finalTranscriptText; // full transcript snapshot saved before reset
extern String rollingSummary;
extern String finalSummaryText;
extern int    chunkIndex;
extern int    wordCount;          // approximate word count for UI

// ─── Time / Naming ────────────────────────────────────────────────────────────
// meetingTimestamp  →  used as directory/file names  e.g. "2025-01-15_14-30-00"
// meetingDisplayTime →  shown in UI                  e.g. "Jan 15, 2025 · 2:30 PM"
// ntpSynced         →  true once NTP or browser time was obtained
// meetingStartEpoch →  Unix timestamp when recording began
extern String meetingTimestamp;
extern String meetingDisplayTime;
extern bool   ntpSynced;
extern time_t meetingStartEpoch;

// ─── Chunk queue ──────────────────────────────────────────────────────────────
// Replaces the old single-slot (chunkReady + currentChunkPath) handoff.
// Up to CHUNK_QUEUE_SIZE finished WAV paths can queue up so a slow STT
// retry never causes recordTask to silently overwrite an un-processed chunk.
// 8 slots × 15 s = 2 minutes of back-log before anything is dropped.
#define CHUNK_QUEUE_SIZE  8
#define CHUNK_PATH_LEN    128
extern QueueHandle_t chunkQueue;

// ─── RTOS handles ─────────────────────────────────────────────────────────────
extern TaskHandle_t      recordTaskHandle;
extern TaskHandle_t      processTaskHandle;
// Guards the transcript/summary Strings shared between processTask (writer)
// and webTask via /api/status (reader). String += / .remove() reallocate
// the internal buffer, so a reader mid-write can see freed memory.
extern SemaphoreHandle_t stateMutex;
extern i2s_chan_handle_t rx_handle;

// ─── Web server instance ──────────────────────────────────────────────────────
extern WebServer server;