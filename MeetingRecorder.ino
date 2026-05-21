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
#include <ESPmDNS.h>
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
volatile bool finalStop         = false;
volatile bool processingFinal   = false;   // true during final-summary generation
volatile bool needWifiReconnect = false;
volatile bool needApRestart     = false;   // set when AP SSID/pass changed via UI
volatile bool needFactoryReset  = false;   // set by /api/factory-reset, handled in processTask

// Chunk queue — replaces single-slot chunkReady/currentChunkPath.
// Defined extern in globals.h; created in setup().
QueueHandle_t chunkQueue;
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

// Default timezone: IST (+5:30 = 330 min).  Loaded from config.json at boot;
// changeable from the Settings tab via the timezone dropdown.
int tzOffsetMin = 330;

// RTOS handles
TaskHandle_t      recordTaskHandle;
TaskHandle_t      processTaskHandle;
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

    // Bluetooth: this project never uses BT — turn the controller fully off
    // so the BLE/BT modem doesn't keep its radio alive in the background.
    // Saves ~5-10 mA continuous on ESP32-S3.
    btStop();
    Serial.println("[Power] Bluetooth controller stopped");

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

    // ── WiFi mode logic ────────────────────────────────────────────────────
    // Three paths:
    //   (A) No creds saved yet → AP-ONLY for first-time setup
    //   (B) Creds saved & STA connects → STA-ONLY + mDNS (AP turned off so
    //       the user's phone/laptop keeps internet access on the home WiFi)
    //   (C) Creds saved but STA fails → AP+STA fallback so the user can
    //       reconfigure via the hotspot
    if (wifiSSID.length() == 0) {
        // (A) First-time setup — AP only
        WiFi.mode(WIFI_AP);
        WiFi.softAP(apSSID.c_str(), apPass.c_str());
        Serial.printf("[Setup] No WiFi creds — AP-only mode\n");
        Serial.printf("[Setup] Connect to '%s' (password: %s)\n",
                      apSSID.c_str(), apPass.c_str());
        Serial.printf("[Setup] Then open http://%s/setup to configure WiFi\n",
                      WiFi.softAPIP().toString().c_str());
    } else {
        // Creds exist — try STA-only first
        Serial.printf("[Setup] Connecting STA: %s\n", wifiSSID.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
        uint32_t wt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wt < 15000) {
            delay(500); Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            // (B) STA OK — STA-only, AP stays off.  User's phone/laptop
            // keeps internet on the home network and reaches the dashboard
            // via mDNS (meetingrecorder.local) — no more "connect to the
            // device hotspot and lose internet" inconvenience.
            WiFi.setSleep(true);     // modem-sleep for power savings
            WiFi.setTxPower(WIFI_POWER_19_5dBm);
            Serial.printf("\n[Setup] WiFi OK — IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());

            // Start mDNS so the dashboard is reachable at meetingrecorder.local
            // from any device on the same WiFi.  No need to memorise IPs.
            if (MDNS.begin("meetingrecorder")) {
                MDNS.addService("http", "tcp", 80);
                Serial.println("[Setup] mDNS active: http://meetingrecorder.local");
            } else {
                Serial.println("[Setup] mDNS init failed — fall back to IP");
            }

            ntpInit();   // sync clock right after STA connects
        } else {
            // (C) STA failed — enable AP+STA for recovery
            Serial.println("\n[Setup] WiFi TIMEOUT — enabling AP for recovery");
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAP(apSSID.c_str(), apPass.c_str());
            Serial.printf("[Setup] Recovery AP: '%s' at %s\n",
                          apSSID.c_str(), WiFi.softAPIP().toString().c_str());
            Serial.println("[Setup] Open http://192.168.4.1/setup to fix WiFi creds");
        }
    }

    stateMutex = xSemaphoreCreateMutex();
    if (!stateMutex) { Serial.println("[Setup] stateMutex FAILED"); while (1); }

    chunkQueue = xQueueCreate(CHUNK_QUEUE_SIZE, CHUNK_PATH_LEN);
    if (!chunkQueue) { Serial.println("[Setup] chunkQueue FAILED"); while (1); }

    createMeetingDir();
    startWebServer();

    // Core 0: audio capture
    xTaskCreatePinnedToCore(recordTask,  "Record",  8192,  NULL, 2, &recordTaskHandle,  0);
    // Core 1: web server (high priority — keeps UI responsive)
    xTaskCreatePinnedToCore(webTask,     "WebSrv",  6144,  NULL, 3, NULL,               1);
    // Core 1: chunk processing (lower priority — runs when web yields)
    xTaskCreatePinnedToCore(processTask, "Process", 20480, NULL, 1, &processTaskHandle, 1);

    // ── Idle power mode ──────────────────────────────────────────────────
    // Boot is done; until a meeting starts there is no audio capture and
    // no HTTPS in flight, so the CPU can drop from 240 MHz to 80 MHz.
    // This cuts the chip's quiescent current by roughly 20-25 mA without
    // affecting the web UI's responsiveness (web routes are I/O-bound).
    // We bump back to 240 MHz when a meeting starts (see button handler
    // below + handleApiStart) so STT/GPT and recording run at full speed.
    setCpuFrequencyMhz(80);
    Serial.println("[Power] Idle: CPU @ 80 MHz, WiFi modem sleep ON");

    Serial.println("\n========================================");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  Dashboard : http://meetingrecorder.local\n");
        Serial.printf("  (or)      : http://%s\n",
                      WiFi.localIP().toString().c_str());
    }
    if (WiFi.getMode() & WIFI_AP) {
        Serial.printf("  AP setup  : http://%s  (SSID: %s)\n",
                      WiFi.softAPIP().toString().c_str(), apSSID.c_str());
    }
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

    // WiFi reconnect after config save (new SSID/password entered via UI)
    if (needWifiReconnect) {
        needWifiReconnect = false;
        Serial.println("[WiFi] Switching to new credentials...");
        // Keep AP up DURING the attempt so the user on the AP doesn't get
        // disconnected if the new creds turn out to be wrong.
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(apSSID.c_str(), apPass.c_str());
        WiFi.disconnect(false);
        delay(300);
        WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
        uint32_t st = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - st < 15000) {
            delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.setSleep(true);
            WiFi.setTxPower(WIFI_POWER_19_5dBm);
            Serial.printf("\n[WiFi] Connected — IP: %s\n",
                          WiFi.localIP().toString().c_str());

            // Restart mDNS on the new network
            MDNS.end();
            if (MDNS.begin("meetingrecorder")) {
                MDNS.addService("http", "tcp", 80);
                Serial.println("[WiFi] mDNS active: http://meetingrecorder.local");
            }

            // Shut down AP now — the dashboard is reachable via mDNS so the
            // hotspot is no longer needed.  User's phone/laptop can go back
            // to the home WiFi and keep their internet.
            delay(800);   // give the success page a moment to be served
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            Serial.println("[WiFi] AP disabled — STA-only mode (use meetingrecorder.local)");

            ntpInit();
        } else {
            Serial.println("\n[WiFi] Reconnect failed — AP stays up for retry");
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
            // Bump CPU before flipping meetingActive so the audio task that
            // is about to read its first I2S buffer already has full speed.
            setCpuFrequencyMhz(240);
            Serial.println("[Power] Active: CPU @ 240 MHz");
            Serial.println("[Loop] ● MEETING STARTED (button)");
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            fullTranscript     = "";
            finalTranscriptText= "";
            chunkIndex         = 0;
            wordCount          = 0;
            rollingSummary     = "Meeting started — summary appears after first chunk.";
            finalSummaryText   = "";
            xSemaphoreGive(stateMutex);
            xQueueReset(chunkQueue);   // discard any leftover chunk paths
            finalStop          = false;
            stampNow();
            meetingActive      = true;
            // LED is now managed by the blink state machine below.
        } else {
            Serial.println("\n[Loop] ■ MEETING STOPPED (button)");
            meetingActive = false;
            finalStop     = true;
            // CPU stays at 240 MHz here — processTask still has to run the
            // final summary (potentially a multi-call map-reduce).  It will
            // drop back to 80 MHz itself once the summary is saved.
            // LED keeps blinking while processingFinal / queue drain runs.
        }
    }
    prevBtn = curBtn;

    // ── LED state machine ──────────────────────────────────────────────────
    // Slow heartbeat blink (one 80 ms pulse every 5 s) while the device is
    // busy — either actively recording, draining queued chunks, or
    // generating the final summary.  When fully idle the LED is OFF.
    //
    // Cuts LED duty cycle from 100% to ~1.6% during recording, which on a
    // 5 mA LED saves roughly 5 mA average.  More importantly the slow blink
    // gives a clear "still working" signal during long map-reduce summary
    // jobs that can last several minutes.
    static unsigned long ledBlinkStart = 0;
    static bool          ledPulseOn    = false;

    bool deviceBusy = meetingActive
                   || finalStop
                   || processingFinal
                   || uxQueueMessagesWaiting(chunkQueue) > 0;

    if (deviceBusy) {
        unsigned long now = millis();
        if (!ledPulseOn && (now - ledBlinkStart) >= 5000) {
            ledBlinkStart = now;
            ledPulseOn    = true;
            digitalWrite(LED_BUILTIN, LOW);    // active-low: pulse ON
        } else if (ledPulseOn && (now - ledBlinkStart) >= 80) {
            ledPulseOn = false;
            digitalWrite(LED_BUILTIN, HIGH);   // pulse OFF
        }
    } else {
        // Fully idle — make sure LED is OFF
        if (ledPulseOn) ledPulseOn = false;
        digitalWrite(LED_BUILTIN, HIGH);
    }

    delay(5);
}
