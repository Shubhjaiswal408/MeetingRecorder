#pragma once
#include <Arduino.h>
/*
 * audio.h
 * ─────────────────────────────────────────────────────────────────
 * PDM microphone initialisation, WAV header generator, and the
 * FreeRTOS record task that runs on Core 0.
 * ─────────────────────────────────────────────────────────────────
 */

// Initialise the I2S PDM receive channel.
// Must be called once in setup() before starting the record task.
// Returns true on success.
bool initMic();

// Write a standard 44-byte PCM WAV header into buffer h.
// dataBytes = number of audio bytes that follow the header.
void generateWavHeader(uint8_t* h, uint32_t dataBytes);

// FreeRTOS task — pin to Core 0.
// Continuously captures audio into 15-second WAV chunks on the SD
// card and signals processTask via chunkMutex when each chunk is done.
void recordTask(void* pv);
