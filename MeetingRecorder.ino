/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  XIAO ESP32-S3  ·  MEETING RECORDER                                  ║
 * ╠══════════════════════════════════════════════════════════════════════╣
 * ║  PROJECT LAYOUT                                                      ║
 * ║  MeetingRecorder.ino  ← globals + setup() + loop()  (this file)      ║
 * ║  src/                 ← all module sources (.cpp / .h)               ║
 * ║    globals.h            shared #defines + extern declarations        ║
 * ║    html_pages.h         PROGMEM HTML (PAGE_MAIN / PAGE_SETUP)        ║
 * ║    config.{h,cpp}       load/save /config.json                       ║
 * ║    audio.{h,cpp}        PDM mic, WAV header, recordTask (Core 0)     ║
 * ║    api.{h,cpp}          ElevenLabs STT + OpenAI GPT                  ║
 * ║    api_chat.{h,cpp}     Q&A chat endpoint                            ║
 * ║    process.{h,cpp}      processTask (Core 1) + SD helpers            ║
 * ║    web_handlers.{h,cpp} HTTP routes + startWebServer()               ║
 * ║    web_extras.{h,cpp}   /api/status, /api/chat, /api/settime         ║
 * ║    ntp_time.h           NTP + browser time helpers                   ║ 
 * ║    docs/              ← project documentation (see docs/README.md)   ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */

#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include <Arduino.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include "src/json/ShubhJson.h"   
#include <time.h>

#include "src/core/globals.h"
#include "src/web/html_pages.h"
#include "src/config/config.h"
#include "src/audio/audio.h"
#include "src/api/api.h"
#include "src/api/api_chat.h"
#include "src/process/process.h"
#include "src/web/web_handlers.h"
#include "src/web/web_extras.h"
#include "src/time/ntp_time.h"

// ============================================================
//  GLOBAL DEFINITIONS
//  Every variable declared `extern` in globals.h is defined
//  exactly once here.
// ============================================================

const char* EL_STT_URL = "https://api.elevenlabs.io/v1/speech-to-text";
const char* OPENAI_URL = "https://api.openai.com/v1/chat/completions";

// WiFi / API credentials
String wifiSSID     = "";
String wifiPass     = "";
String elApiKey     = "";
String openaiApiKey = "";

// AP hotspot credentials (runtime — loaded from config.json, changeable in UI)
String apSSID = AP_SSID_DEFAULT;
String apPass = AP_PASS_DEFAULT;

// Core meeting state
volatile bool meetingActive     = false;
volatile bool chunkReady        = false;
volatile bool finalStop         = false;
volatile bool needWifiReconnect = false;
volatile bool needApRestart     = false;   // set when AP SSID/pass changed via UI

String currentChunkPath    = "";
String meetingDir          = "";
String fullTranscript      = "";
String finalTranscriptText = "";   // ← FIX Bug 4: was missing, caused linker error
String rollingSummary      = "No summary yet.";
String finalSummaryText    = "";
int    chunkIndex          = 0;
int    wordCount           = 0;    // ← FIX Bug 4: was missing

// Time / Naming (v2 globals — all were missing from old .ino)
String meetingTimestamp   = "";    // ← FIX Bug 4
String meetingDisplayTime = "";    // ← FIX Bug 4
bool   ntpSynced          = false; // ← FIX Bug 4
time_t meetingStartEpoch  = 0;     // ← FIX Bug 4

// RTOS handles
TaskHandle_t      recordTaskHandle;
TaskHandle_t      processTaskHandle;
SemaphoreHandle_t chunkMutex;
SemaphoreHandle_t stateMutex;
i2s_chan_handle_t rx_handle = NULL;

// Web server
WebServer server(80);

static void webTask(void* pv) {
    Serial.println("[WebTask] Started on core " + String(xPortGetCoreID()));
    for (;;) {
        server.handleClient();
        vTaskDelay(2 / portTICK_PERIOD_MS);
    }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && millis() - t0 < 4000);
    delay(500);

    Serial.println("\n========================================");
    Serial.println("   XIAO ESP32-S3  MEETING RECORDER v3");
    Serial.println("========================================");

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("[Setup] GPIO OK");

    // SD card
    Serial.print("[Setup] SD init... ");
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("FAILED — card not found or not FAT32");
        while (1) delay(500);
    }
    Serial.printf("OK  (%llu MB)\n", SD.cardSize() / (1024ULL * 1024ULL));

    loadConfig();

    // Microphone
    Serial.print("[Setup] Mic init... ");
    if (!initMic()) {
        Serial.println("FAILED — check Sense expansion board");
        while (1) delay(500);
    }
    Serial.println("OK");

    // WiFi: AP always on + STA for internet
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSSID.c_str(), apPass.c_str());
    Serial.printf("[Setup] AP: %-20s  IP=%s\n",
                  apSSID.c_str(), WiFi.softAPIP().toString().c_str());

    if (wifiSSID.length() > 0) {
        Serial.printf("[Setup] Connecting STA: %s\n", wifiSSID.c_str());
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
        uint32_t wt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wt < 15000) {
            delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(false);
            WiFi.setTxPower(WIFI_POWER_19_5dBm);
            Serial.printf("\n[Setup] WiFi OK — IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            ntpInit();   // sync clock right after STA connects
        } else {
            Serial.println("\n[Setup] WiFi TIMEOUT — AP only.");
            // Re-arm AP — some ESP32 SDK builds drop softAP after a failed
            // WiFi.begin() attempt even when mode is WIFI_AP_STA.
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP(apSSID.c_str(), apPass.c_str());
            Serial.printf("[Setup] AP re-armed: %s\n", WiFi.softAPIP().toString().c_str());
        }
    } else {
        Serial.println("[Setup] No WiFi — open http://192.168.4.1/setup");
    }

    chunkMutex = xSemaphoreCreateMutex();
    stateMutex = xSemaphoreCreateMutex();
    if (!chunkMutex || !stateMutex) { Serial.println("[Setup] Mutex FAILED"); while (1); }

    createMeetingDir();
    startWebServer();

    // Core 0: audio capture
    xTaskCreatePinnedToCore(recordTask,  "Record",  8192,  NULL, 2, &recordTaskHandle,  0);
    // Core 1: web server (high priority — keeps UI responsive)
    xTaskCreatePinnedToCore(webTask,     "WebSrv",  6144,  NULL, 3, NULL,               1);
    // Core 1: chunk processing (lower priority — runs when web yields)
    xTaskCreatePinnedToCore(processTask, "Process", 20480, NULL, 1, &processTaskHandle, 1);

    Serial.println("\n========================================");
    Serial.printf( "  AP  URL : http://%s  (SSID: %s)\n",
                   WiFi.softAPIP().toString().c_str(), apSSID.c_str());
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("  LAN URL : http://%s\n", WiFi.localIP().toString().c_str());
    Serial.println("  Press button or use web UI to start");
    Serial.println("========================================\n");
}

void loop() {
    // AP restart after hotspot settings change
    if (needApRestart) {
        needApRestart = false;
        Serial.printf("[WiFi] Restarting AP with new SSID: %s\n", apSSID.c_str());
        WiFi.softAPdisconnect(false);
        delay(200);
        WiFi.softAP(apSSID.c_str(), apPass.c_str());
        Serial.printf("[WiFi] AP ready at %s\n", WiFi.softAPIP().toString().c_str());
    }

    // WiFi reconnect after config save
    if (needWifiReconnect) {
        needWifiReconnect = false;
        Serial.println("[WiFi] Reconnecting...");
        // disconnect(false) = keep AP running; avoid mode reset that drops hotspot
        WiFi.disconnect(false);
        delay(300);
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
        uint32_t st = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - st < 15000) {
            delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(false);
            WiFi.setTxPower(WIFI_POWER_19_5dBm);
            Serial.printf("\n[WiFi] Connected — IP: %s\n",
                          WiFi.localIP().toString().c_str());
            ntpInit();
        } else {
            Serial.println("\n[WiFi] Reconnect failed — AP still up.");
            // Re-arm AP in case the failed STA attempt dropped it
            WiFi.softAP(apSSID.c_str(), apPass.c_str());
        }
    }

    // Physical button (active LOW)
    static bool prevBtn = HIGH;
    bool curBtn = digitalRead(BUTTON_PIN);

    if (prevBtn == HIGH && curBtn == LOW) {
        delay(80);
        if (!meetingActive) {
            Serial.println("\n[Loop] ● MEETING STARTED (button)");
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            fullTranscript     = "";
            finalTranscriptText= "";
            chunkIndex         = 0;
            wordCount          = 0;
            rollingSummary     = "Meeting started — summary appears after first chunk.";
            finalSummaryText   = "";
            xSemaphoreGive(stateMutex);
            chunkReady         = false;
            finalStop          = false;
            stampNow();
            meetingActive      = true;
            digitalWrite(LED_BUILTIN, LOW);
        } else {
            Serial.println("\n[Loop] ■ MEETING STOPPED (button)");
            meetingActive = false;
            finalStop     = true;
            digitalWrite(LED_BUILTIN, HIGH);
        }
    }
    prevBtn = curBtn;
    delay(5);
}
