#pragma once
/*
 * ntp_time.h
 * ─────────────────────────────────────────────────────────────────
 * Tiny helpers for obtaining wall-clock time on the ESP32.
 * 
 * Strategy (in priority order):
 *   1. NTP sync when STA WiFi is up  (automatic, most accurate)
 *   2. Browser time pushed via POST /api/settime  (fallback)
 *   3. Monotonic counter from millis()  (last resort — no date)
 *
 * Call ntpInit() right after WiFi.begin() / WiFi.waitForConnectResult().
 * Call setBrowserTime(epochMs) from the /api/settime handler.
 * Call stampNow() when a meeting starts to capture meetingTimestamp.
 * ─────────────────────────────────────────────────────────────────
 */

#include "../core/globals.h"
#include <WiFi.h>
#include <time.h>

// ── NTP pools ──────────────────────────────────────────────────────
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.google.com";
static const char* NTP3 = "time.cloudflare.com";

// Timezone offset is now a RUNTIME value loaded from config.json
// (see `tzOffsetMin` in globals.h).  Default = IST (+5:30 = 330 min).
// Users can change it from the Settings tab.

// ─────────────────────────────────────────────────────────────────
//  ntpInit() — call once after WiFi STA connects, or after the user
//  changes the timezone in Settings (handleApiConfig calls this
//  again so the clock immediately reflects the new offset).
// ─────────────────────────────────────────────────────────────────
inline void ntpInit() {
    configTime(tzOffsetMin * 60, 0, NTP1, NTP2, NTP3);
    struct tm ti;
    unsigned long t0 = millis();
    while (!getLocalTime(&ti, 100) && millis() - t0 < 5000) delay(200);
    if (ti.tm_year > 120) {
        ntpSynced = true;
        Serial.printf("[NTP] Synced: %04d-%02d-%02d %02d:%02d:%02d\n",
            ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
            ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        Serial.println("[NTP] Sync failed — waiting for browser time");
    }
}

// ─────────────────────────────────────────────────────────────────
//  setBrowserTime() — called by /api/settime handler
//  epochMs: JavaScript Date.now() value
// ─────────────────────────────────────────────────────────────────
inline void setBrowserTime(long long epochMs) {
    if (ntpSynced) return;
    time_t t = (time_t)(epochMs / 1000LL);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    ntpSynced = true;
    Serial.printf("[TIME] Set from browser: epoch=%lld\n", (long long)t);
    // Re-stamp the current meeting dir with the now-known time
    struct tm ti;
    if (getLocalTime(&ti, 200)) {
        char dispBuf[40];
        strftime(dispBuf, sizeof(dispBuf), "%b %d, %Y · %I:%M %p", &ti);
        meetingDisplayTime = String(dispBuf);
    }
}

// ─────────────────────────────────────────────────────────────────
//  stampNow() — capture timestamp strings at meeting start
//  Fills globals: meetingTimestamp, meetingDisplayTime, meetingStartEpoch
//
//  NOTE: meetingTimestamp must NOT include "meeting_" prefix —
//  process.cpp prepends "/meeting_" when building the directory path.
// ─────────────────────────────────────────────────────────────────
inline void stampNow() {
    struct tm ti;
    if (ntpSynced && getLocalTime(&ti, 500)) {
        char fileBuf[32], dispBuf[40];
        // File-safe name: 2025-01-15_14-30-00
        strftime(fileBuf, sizeof(fileBuf), "%Y-%m-%d_%H-%M-%S", &ti);
        // Display: Jan 15, 2025 · 2:30 PM
        strftime(dispBuf, sizeof(dispBuf), "%b %d, %Y · %I:%M %p", &ti);
        meetingTimestamp   = String(fileBuf);       // e.g. "2025-01-15_14-30-00"
        meetingDisplayTime = String(dispBuf);
        time(&meetingStartEpoch);
    } else {
        // FIX: was "meeting_" + String(ms) which caused /meeting_meeting_XXXXX
        // Now just the raw millis number — process.cpp adds the "meeting_" prefix
        unsigned long ms = millis();
        meetingTimestamp   = String(ms);            // e.g. "15958"  → dir = /meeting_15958
        meetingDisplayTime = "~" + String(ms/1000) + "s uptime";
        meetingStartEpoch  = 0;
    }
}

// ─────────────────────────────────────────────────────────────────
//  elapsedSeconds() — seconds since meetingStartEpoch (or millis)
// ─────────────────────────────────────────────────────────────────
inline long elapsedSeconds() {
    if (meetingStartEpoch > 0) {
        time_t now; time(&now);
        return (long)(now - meetingStartEpoch);
    }
    return 0;
}
