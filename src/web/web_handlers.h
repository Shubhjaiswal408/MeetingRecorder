#pragma once
/*
 * web_handlers.h
 * ─────────────────────────────────────────────────────────────────
 * Registers all HTTP routes and starts the WebServer on port 80.
 *
 * Routes registered:
 *   GET  /               → Dashboard HTML (PAGE_MAIN)
 *   GET  /setup          → Config HTML    (PAGE_SETUP)
 *   GET  /api/status     → JSON live state
 *   POST /api/start      → Start meeting
 *   POST /api/stop       → Stop meeting
 *   POST /api/config     → Save WiFi + API keys
 *   GET  /api/history    → JSON list of past meeting dirs + summaries
 *   POST /api/history/delete → Delete a meeting directory
 * ─────────────────────────────────────────────────────────────────
 */

// Call once in setup() after WiFi is configured.
void startWebServer();
