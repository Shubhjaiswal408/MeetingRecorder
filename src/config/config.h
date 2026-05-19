#pragma once
/*
 * config.h
 * ─────────────────────────────────────────────────────────────────
 * Load and save WiFi credentials + API keys to /config.json on SD.
 * ─────────────────────────────────────────────────────────────────
 */

// Returns true if a WiFi SSID was found in the config file.
bool loadConfig();

// Writes current credentials to /config.json.
void saveConfig();
