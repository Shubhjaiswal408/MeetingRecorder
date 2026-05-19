#pragma once
/*
 * web_extras.h
 * ─────────────────────────────────────────────────────────────────
 * Declares the new HTTP route handlers added in v2.
 * Register them in startWebServer() inside web_handlers.cpp:
 *
 *   server.on("/api/status",  HTTP_GET,  handleApiStatus);
 *   server.on("/api/chat",    HTTP_POST, handleApiChat);
 *   server.on("/api/settime", HTTP_POST, handleApiSetTime);
 *
 * ─────────────────────────────────────────────────────────────────
 */

#include "../core/globals.h"

// GET /api/status — returns JSON with full meeting state for the dashboard
void handleApiStatus();

// POST /api/chat  — body: {"question":"…","history":[…]}
//                   returns: {"answer":"…"}
void handleApiChat();

// POST /api/settime — body: {"ts":1705312200000}  (JS Date.now())
//                    sets device RTC from browser when NTP is unavailable
void handleApiSetTime();
