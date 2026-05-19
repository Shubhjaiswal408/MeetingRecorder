/*
 * audio.cpp
 * ─────────────────────────────────────────────────────────────────
 * Handles all I2S / PDM audio capture.
 *
 * Flow (Core 0):
 *   meetingActive → open WAV file → read I2S chunks → write to SD
 *   → after CHUNK_SECONDS → fix WAV header → signal processTask
 * ─────────────────────────────────────────────────────────────────
 */

#include "audio.h"
#include "../core/globals.h"
#include "FS.h"
#include "SD.h"

// ─── initMic ──────────────────────────────────────────────────────────────────
bool initMic() {
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cfg, NULL, &rx_handle) != ESP_OK) return false;

    i2s_pdm_rx_config_t pdm = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .clk = PDM_CLK_GPIO, .din = PDM_DIN_GPIO }
    };
    if (i2s_channel_init_pdm_rx_mode(rx_handle, &pdm) != ESP_OK) return false;
    if (i2s_channel_enable(rx_handle) != ESP_OK) return false;
    return true;
}

// ─── generateWavHeader ────────────────────────────────────────────────────────
void generateWavHeader(uint8_t* h, uint32_t dataBytes) {
    const uint16_t channels      = 1;
    const uint32_t sampleRate    = SAMPLE_RATE;
    const uint16_t bitsPerSample = SAMPLE_BITS;
    const uint32_t byteRate      = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign    = channels * (bitsPerSample / 8);
    const uint32_t fileSize      = dataBytes + WAV_HEADER_SIZE - 8;

    memset(h, 0, WAV_HEADER_SIZE);
    // RIFF chunk descriptor
    h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
    h[4]=(fileSize)&0xFF; h[5]=(fileSize>>8)&0xFF; h[6]=(fileSize>>16)&0xFF; h[7]=(fileSize>>24)&0xFF;
    h[8]='W'; h[9]='A'; h[10]='V'; h[11]='E';
    // fmt sub-chunk
    h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;  // sub-chunk size = 16
    h[20]=1;  h[21]=0;                     // PCM audio format
    h[22]=(channels)&0xFF; h[23]=(channels>>8)&0xFF;
    h[24]=(sampleRate)&0xFF; h[25]=(sampleRate>>8)&0xFF;
    h[26]=(sampleRate>>16)&0xFF; h[27]=(sampleRate>>24)&0xFF;
    h[28]=(byteRate)&0xFF; h[29]=(byteRate>>8)&0xFF;
    h[30]=(byteRate>>16)&0xFF; h[31]=(byteRate>>24)&0xFF;
    h[32]=(blockAlign)&0xFF; h[33]=(blockAlign>>8)&0xFF;
    h[34]=(bitsPerSample)&0xFF; h[35]=(bitsPerSample>>8)&0xFF;
    // data sub-chunk
    h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
    h[40]=(dataBytes)&0xFF; h[41]=(dataBytes>>8)&0xFF;
    h[42]=(dataBytes>>16)&0xFF; h[43]=(dataBytes>>24)&0xFF;
}

// ─── recordTask (Core 0) ──────────────────────────────────────────────────────
void recordTask(void* pv) {
    Serial.println("[RecordTask] Started on core " + String(xPortGetCoreID()));

    uint8_t* buffer = (uint8_t*)malloc(I2S_READ_CHUNK);
    if (!buffer) {
        Serial.println("[RecordTask] FATAL: malloc failed!");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        // Wait until a meeting is active
        if (!meetingActive) {
            vTaskDelay(20 / portTICK_PERIOD_MS);
            continue;
        }

        String wavPath = meetingDir + "/chunk_" + String(chunkIndex) + ".wav";
        Serial.printf("[RecordTask] Opening %s\n", wavPath.c_str());

        File file = SD.open(wavPath.c_str(), FILE_WRITE);
        if (!file) {
            Serial.println("[RecordTask] SD open failed — retrying...");
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        // Write placeholder header (will be fixed after chunk ends)
        uint8_t header[WAV_HEADER_SIZE] = {0};
        generateWavHeader(header, 0);
        file.write(header, WAV_HEADER_SIZE);
        file.flush();

        uint32_t totalBytes = 0;
        uint32_t startTime  = millis();
        uint32_t lastHB     = millis();

        Serial.printf("[RecordTask] ● Recording chunk %d...\n", chunkIndex);

        while (meetingActive) {
            size_t bytesRead = 0;
            esp_err_t err = i2s_channel_read(rx_handle, buffer, I2S_READ_CHUNK,
                                             &bytesRead, pdMS_TO_TICKS(100));
            if (err == ESP_OK && bytesRead > 0) {
                // Apply software volume gain
                for (size_t i = 0; i + 1 < bytesRead; i += 2) {
                    int16_t s;
                    memcpy(&s, &buffer[i], 2);
                    int32_t amp = (int32_t)s << VOLUME_GAIN;
                    s = (int16_t)constrain(amp, -32768, 32767);
                    memcpy(&buffer[i], &s, 2);
                }
                file.write(buffer, bytesRead);
                totalBytes += bytesRead;
            } else if (err != ESP_OK) {
                Serial.printf("[RecordTask] i2s_read err=0x%x\n", err);
            }

            // Heartbeat log every 5 s
            if (millis() - lastHB >= 5000) {
                lastHB = millis();
                Serial.printf("[RecordTask] ♪ %us/%us  (%u KB)\n",
                    (millis()-startTime)/1000, CHUNK_SECONDS, totalBytes/1024);
            }

            // Split chunk after CHUNK_SECONDS
            if ((millis() - startTime) >= (uint32_t)(CHUNK_SECONDS * 1000)) {
                Serial.println("[RecordTask] Chunk time limit — splitting.");
                break;
            }
        }

        // Patch the WAV header with the real data length
        file.seek(0);
        generateWavHeader(header, totalBytes);
        file.write(header, WAV_HEADER_SIZE);
        file.flush();
        file.close();

        Serial.printf("[RecordTask] Chunk %d done: %u bytes audio\n", chunkIndex, totalBytes);
        if (totalBytes == 0) Serial.println("[RecordTask] WARNING: zero bytes — mic issue?");

        // Enqueue path for processTask.
        // xQueueSend is non-blocking (timeout=0) — if the queue is full after
        // CHUNK_QUEUE_SIZE back-logged chunks, we warn and drop this one.
        // That only happens after >2 continuous minutes of STT failures, which
        // would already mean the transcript is hopelessly stale.
        char pathBuf[CHUNK_PATH_LEN];
        strncpy(pathBuf, wavPath.c_str(), CHUNK_PATH_LEN - 1);
        pathBuf[CHUNK_PATH_LEN - 1] = '\0';
        if (xQueueSend(chunkQueue, pathBuf, 0) != pdTRUE) {
            Serial.println("[RecordTask] WARNING: chunk queue full — chunk dropped!");
        }

        chunkIndex++;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
