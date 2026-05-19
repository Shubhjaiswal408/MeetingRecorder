// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License
//
// Modification notice (Shubh'Json bundle, MeetingRecorder project):
//   This file was renamed from ArduinoJson.hpp; the bundled internal-headers
//   folder was renamed from ArduinoJson/ to ShubhJson/; and every
//   `#include <ArduinoJson/...>` / `#include "ArduinoJson/..."` directive
//   inside the library was rewritten to `<ShubhJson/...>` accordingly.
//   In addition, the 25 sub-header includes below were originally in
//   quote-form and have been converted to angle-form to work around a
//   Windows GCC #pragma once bug where the same header reached via mixed
//   `\` and `/` path separators is not deduplicated, causing class
//   redefinition errors when the library is bundled inside an Arduino
//   sketch.  Behaviour is otherwise identical to upstream.  Full original
//   LICENSE preserved in docs/ShubhJson_LICENSE.txt.

#pragma once

#if __cplusplus < 201103L && (!defined(_MSC_VER) || _MSC_VER < 1910)
#  error ArduinoJson requires C++11 or newer. Configure your compiler for C++11 or downgrade ArduinoJson to 6.20.
#endif

// ─────────────────────────────────────────────────────────────────────────
//  Shubh'Json feature switches — turn off compile units we do not use in
//  the MeetingRecorder project.  Each one must be set BEFORE Configuration
//  .hpp pulls in its defaults (Configuration.hpp wraps every option in
//  #ifndef X #define X default, so #defines made here win.)
//
//  Why each is safe to disable:
//    PROGMEM        — we deserialise from File and String, never from
//                     PROGMEM-stored JSON.
//    COMMENTS       — neither config.json nor the OpenAI / ElevenLabs
//                     responses contain /* */ or // comments.
//    LONG_LONG      — we only store strings and small ints (chunkIndex,
//                     wordCount, epoch).
//    DOUBLE         — no floats anywhere in our schema; defaulting to
//                     float saves flash and removes double precision
//                     softfp helpers.
//    STD_STRING     — we use Arduino String, never std::string.
//    STD_STREAM     — we use Arduino Stream / File, never std::iostream.
// ─────────────────────────────────────────────────────────────────────────
#define ARDUINOJSON_ENABLE_PROGMEM     0
#define ARDUINOJSON_ENABLE_COMMENTS    0
#define ARDUINOJSON_USE_LONG_LONG      0
#define ARDUINOJSON_USE_DOUBLE         0
#define ARDUINOJSON_ENABLE_STD_STRING  0
#define ARDUINOJSON_ENABLE_STD_STREAM  0

#include <ShubhJson/Configuration.hpp>

// Include Arduino.h before stdlib.h to avoid conflict with atexit()
// https://github.com/bblanchon/ArduinoJson/pull/1693#issuecomment-1001060240
#if ARDUINOJSON_ENABLE_ARDUINO_STRING || ARDUINOJSON_ENABLE_ARDUINO_STREAM || \
    ARDUINOJSON_ENABLE_ARDUINO_PRINT ||                                       \
    (ARDUINOJSON_ENABLE_PROGMEM && defined(ARDUINO))
#  include <Arduino.h>
#endif

#if !ARDUINOJSON_DEBUG
#  ifdef __clang__
#    pragma clang system_header
#  elif defined __GNUC__
#    pragma GCC system_header
#  endif
#endif

// Remove true and false macros defined by some cores, such as Arduino Due's
// See issues #2181 and arduino/ArduinoCore-sam#50
#ifdef true
#  undef true
#endif
#ifdef false
#  undef false
#endif

#include <ShubhJson/Array/JsonArray.hpp>
#include <ShubhJson/Object/JsonObject.hpp>
#include <ShubhJson/Variant/JsonVariantConst.hpp>

#include <ShubhJson/Document/JsonDocument.hpp>

#include <ShubhJson/Array/ArrayImpl.hpp>
#include <ShubhJson/Array/ElementProxy.hpp>
#include <ShubhJson/Array/Utilities.hpp>
#include <ShubhJson/Collection/CollectionImpl.hpp>
#include <ShubhJson/Memory/ResourceManagerImpl.hpp>
#include <ShubhJson/Object/MemberProxy.hpp>
#include <ShubhJson/Object/ObjectImpl.hpp>
#include <ShubhJson/Variant/ConverterImpl.hpp>
#include <ShubhJson/Variant/JsonVariantCopier.hpp>
#include <ShubhJson/Variant/VariantCompare.hpp>
#include <ShubhJson/Variant/VariantImpl.hpp>
#include <ShubhJson/Variant/VariantRefBaseImpl.hpp>

#include <ShubhJson/Json/JsonDeserializer.hpp>
#include <ShubhJson/Json/JsonSerializer.hpp>
#include <ShubhJson/Json/PrettyJsonSerializer.hpp>
#include <ShubhJson/MsgPack/MsgPackBinary.hpp>
#include <ShubhJson/MsgPack/MsgPackDeserializer.hpp>
#include <ShubhJson/MsgPack/MsgPackExtension.hpp>
#include <ShubhJson/MsgPack/MsgPackSerializer.hpp>

#include <ShubhJson/compatibility.hpp>
