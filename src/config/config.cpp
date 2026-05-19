/*
 * config.cpp
 * ─────────────────────────────────────────────────────────────────
 * Reads/writes /config.json from the SD card.
 * JSON keys: ssid, pass, el_key, openai_key
 * ─────────────────────────────────────────────────────────────────
 */

#include "config.h"
#include "../core/globals.h"
#include "FS.h"
#include "SD.h"
#include "../json/ShubhJson.h"

// ─── loadConfig ───────────────────────────────────────────────────────────────
bool loadConfig() {
    File f = SD.open(CONFIG_FILE, FILE_READ);
    if (!f) {
        Serial.println("[Config] No config.json found — using defaults.");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.println("[Config] JSON parse error: " + String(err.c_str()));
        return false;
    }

    wifiSSID     = doc["ssid"]       | "";
    wifiPass     = doc["pass"]       | "";
    elApiKey     = doc["el_key"]     | "";
    openaiApiKey = doc["openai_key"] | "";
    apSSID       = doc["ap_ssid"]    | AP_SSID_DEFAULT;
    apPass       = doc["ap_pass"]    | AP_PASS_DEFAULT;

    Serial.printf("[Config] Loaded — SSID: %s  AP: %s  EL: %s...  OAI: %s...\n",
        wifiSSID.c_str(),
        apSSID.c_str(),
        elApiKey.length()     > 6 ? elApiKey.substring(0,6).c_str()     : "N/A",
        openaiApiKey.length() > 6 ? openaiApiKey.substring(0,6).c_str() : "N/A");

    return wifiSSID.length() > 0;
}

// ─── saveConfig ───────────────────────────────────────────────────────────────
void saveConfig() {
    File f = SD.open(CONFIG_FILE, FILE_WRITE);
    if (!f) {
        Serial.println("[Config] ERROR: cannot write config.json");
        return;
    }

    JsonDocument doc;
    doc["ssid"]       = wifiSSID;
    doc["pass"]       = wifiPass;
    doc["el_key"]     = elApiKey;
    doc["openai_key"] = openaiApiKey;
    doc["ap_ssid"]    = apSSID;
    doc["ap_pass"]    = apPass;

    serializeJson(doc, f);
    f.close();
    Serial.println("[Config] Saved to SD.");
}