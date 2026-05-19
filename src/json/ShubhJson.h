#pragma once
/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║                          Shubh'Json                                  ║
 * ║                                                                      ║
 * ║   Bundled JSON library for the MeetingRecorder project.              ║
 * ║   Curated and integrated by Shubh.                                   ║
 * ║                                                                      ║
 * ║   Why this file exists                                               ║
 * ║   ───────────────────                                                ║
 * ║   So beginners cloning the sketch do not need to open Library        ║
 * ║   Manager and hunt down a JSON parser. Everything required to        ║
 * ║   parse and emit JSON ships inside the sketch itself.                ║
 * ║                                                                      ║
 * ║   Usage                                                              ║
 * ║   ─────                                                              ║
 * ║       #include "json/ShubhJson.h"                                    ║
 * ║                                                                      ║
 * ║       JsonDocument doc;                                              ║
 * ║       deserializeJson(doc, payload);                                 ║
 * ║       String name = doc["name"] | "";                                ║
 * ║                                                                      ║
 * ║   Implementation                                                     ║
 * ║   ──────────────                                                     ║
 * ║   The parser/serializer internals derive from ArduinoJson (MIT      ║
 * ║   licensed, copyright © 2014-2026 Benoit BLANCHON). The full         ║
 * ║   original license is preserved in docs/ShubhJson_LICENSE.txt as     ║
 * ║   the MIT license requires.  Shubh'Json is a project-local rebrand:  ║
 * ║   the wrapper API, integration glue, and zero-install packaging are  ║
 * ║   by Shubh; everything in the ShubhJson/ folder at the sketch root   ║
 * ║   is the upstream parser code with renamed include paths.            ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */

// Pull in the bundled core. The two entry-point headers live alongside
// this file (src/json/), but the ShubhJson/ folder of internal .hpp
// files lives at the SKETCH ROOT — Arduino IDE only adds the sketch
// root to the compiler include path, and the library's internal
// `#include <ShubhJson/...>` directives resolve against it.
// Bundled LICENSE: docs/ShubhJson_LICENSE.txt (preserved per MIT).
#include "ShubhJson_core.h"

// ─────────────────────────────────────────────────────────────────────
//  Optional Shubh'Json aliases — let user code spell the types in the
//  Shubh'Json style if they prefer. All of these resolve to the same
//  underlying types, so mix-and-match is fine.
// ─────────────────────────────────────────────────────────────────────
using ShubhJsonDocument    = JsonDocument;
using ShubhJsonObject      = JsonObject;
using ShubhJsonArray       = JsonArray;
using ShubhJsonVariant     = JsonVariant;
using ShubhDeserialization = DeserializationError;

// Convenience macros so the public-facing API reads as Shubh'Json.
#define shubhParse(doc, src)     deserializeJson((doc), (src))
#define shubhStringify(doc, out) serializeJson((doc), (out))
