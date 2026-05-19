// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>
#include <ShubhJson/Polyfills/utility.hpp>

#include <stdlib.h>  // for size_t

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

// The default reader is a simple wrapper for Readers that are not copyable
template <typename TSource, typename Enable = void>
struct Reader {
 public:
  Reader(TSource& source) : source_(&source) {}

  int read() {
    // clang-format off
    return source_->read();  // Error here? See https://arduinojson.org/v7/invalid-input/
    // clang-format on
  }

  size_t readBytes(char* buffer, size_t length) {
    return source_->readBytes(buffer, length);
  }

 private:
  TSource* source_;
};

template <typename TSource, typename Enable = void>
struct BoundedReader {
  // no default implementation because we need to pass the size to the
  // constructor
};

ARDUINOJSON_END_PRIVATE_NAMESPACE

#include <ShubhJson/Deserialization/Readers/IteratorReader.hpp>
#include <ShubhJson/Deserialization/Readers/RamReader.hpp>
#include <ShubhJson/Deserialization/Readers/VariantReader.hpp>

#if ARDUINOJSON_ENABLE_ARDUINO_STREAM
#  include <ShubhJson/Deserialization/Readers/ArduinoStreamReader.hpp>
#endif

#if ARDUINOJSON_ENABLE_ARDUINO_STRING
#  include <ShubhJson/Deserialization/Readers/ArduinoStringReader.hpp>
#endif

#if ARDUINOJSON_ENABLE_PROGMEM
#  include <ShubhJson/Deserialization/Readers/FlashReader.hpp>
#endif

#if ARDUINOJSON_ENABLE_STD_STREAM
#  include <ShubhJson/Deserialization/Readers/StdStreamReader.hpp>
#endif

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

template <typename TInput>
Reader<remove_reference_t<TInput>> makeReader(TInput&& input) {
  return Reader<remove_reference_t<TInput>>{detail::forward<TInput>(input)};
}

template <typename TChar>
BoundedReader<TChar*> makeReader(TChar* input, size_t inputSize) {
  return BoundedReader<TChar*>{input, inputSize};
}

ARDUINOJSON_END_PRIVATE_NAMESPACE
