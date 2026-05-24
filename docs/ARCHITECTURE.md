# Architecture

## 1. Runtime Topology

The firmware uses three pinned FreeRTOS tasks plus the Arduino `loop()` so that
audio capture, the web UI, and slow HTTP calls never block each other.

```
┌──────────────────── Core 0 ────────────────────┐  ┌──────────────────── Core 1 ────────────────────┐
│                                                │  │                                                │
│   recordTask (prio 2)                          │  │   webTask     (prio 3)  ← keeps UI responsive  │
│   ─ PDM mic → 15 s WAV chunks                  │  │   ─ server.handleClient() on a tight loop      │
│   ─ Writes to SD: /meeting_<ts>/chunkNN.wav    │  │                                                │
│   ─ Posts WAV path to chunkQueue (8 slots)     │  │   processTask (prio 1)                         │
│                                                │  │   ─ Receives from chunkQueue                   │
│                                                │  │   ─ POSTs WAV to ElevenLabs (STT)              │
│                                                │  │   ─ Appends transcript to SD + in-RAM tail     │
│                                                │  │   ─ POSTs to OpenAI for rolling summary        │
│                                                │  │   ─ On finalStop: runs single-call OR          │
│                                                │  │     map-reduce final summary pipeline          │
│                                                │  │                                                │
│                                                │  │   loop() (Arduino, prio 1)                     │
│                                                │  │   ─ Button polling, WiFi reconnect, AP restart │
│                                                │  │   ─ LED heartbeat blink state machine          │
└────────────────────────────────────────────────┘  └────────────────────────────────────────────────┘
```

Why this split?

- **STT / GPT calls block for seconds.** If they ran in `loop()` the web UI
  would freeze. `processTask` owns them, `webTask` keeps the dashboard
  responsive throughout.
- **PDM capture is real-time.** Putting it on its own core (Core 0) prevents
  audio drops while Core 1 is busy doing HTTP work.

---

## 2. Data Flow — A Single Meeting

```
┌─ Button / Web UI ─┐
│   setCpuFrequencyMhz(240)   ← bump to full speed
│   meetingActive = true
│   stampNow() sets meetingTimestamp
└─────────┬─────────┘
          │
          ▼
   recordTask (Core 0) ──── 15 s WAV ────► /meeting_<ts>/chunk_NN.wav
          │
          │ xQueueSend(chunkQueue, path)   ← up to 8 paths buffered
          ▼
   processTask (Core 1)
          │
          ├─► ElevenLabs STT      → chunk_NN.txt + appended to full_transcript.txt on SD
          │
          │   (rolling fullTranscript in RAM is trimmed at 12 KB — for live UI display only)
          │
          └─► OpenAI rolling summary    → rollingSummary
          │
          │   webTask serves /api/status with the latest state
          ▼
   ── repeat until user stops ──
          │
          ▼
   finalStop = true
          │
          ▼
   processTask
          ├─► Read transcript size from SD's full_transcript.txt
          │
          ├─► If ≤ 25 KB → single-call: read entire file, generateSummary(transcript, true)
          │
          ├─► If > 25 KB → map-reduce:
          │       MAP    : stream 25 KB segments from SD,
          │                generateSegmentSummary() per segment,
          │                accumulate segment summaries (~1-2 KB each)
          │       REDUCE : synthesizeFinalSummary(combined) → final polished output
          │
          ├─► Writes summary_final.txt
          ├─► processingFinal = false
          └─► setCpuFrequencyMhz(80)    ← drop back to idle
```

---

## 3. Shared State

Everything cross-module lives in [`src/core/globals.h`](../src/core/globals.h)
as `extern` declarations. The single definition of every global lives in
`MeetingRecorder.ino` so the linker resolves them exactly once.

Categories:

- **Credentials** — `wifiSSID`, `wifiPass`, `elApiKey`, `openaiApiKey`,
  `apSSID`, `apPass`. Loaded from `/config.json` at boot, rewritable via UI.
- **Meeting state** — `meetingActive`, `finalStop`, `processingFinal`,
  `meetingDir`, `fullTranscript` (in-RAM tail, capped at 12 KB),
  `rollingSummary`, `finalTranscriptText`, `finalSummaryText`,
  `chunkIndex`, `wordCount`.
- **Chunk queue** — `chunkQueue` (8-slot FreeRTOS queue of WAV paths).
  Replaces the old single-slot `chunkReady` / `currentChunkPath` pair;
  prevents chunk loss when STT retries take longer than a single 15 s
  recording window.
- **Time** — `meetingTimestamp`, `meetingDisplayTime`, `ntpSynced`,
  `meetingStartEpoch`.
- **RTOS handles** — `recordTaskHandle`, `processTaskHandle`,
  `stateMutex`, `rx_handle`.
- **Web server** — `server` (port 80).

`volatile` is used for flags read by one task and written by another;
`stateMutex` guards transcript / summary `String`s between `processTask`
(the writer) and `webTask` via `/api/status` (the reader). String `+=` and
`.remove()` reallocate the internal buffer, so a reader mid-write can see
freed memory without the lock.

---

## 4. Storage Layout on the SD Card

```
/
├── config.json                       # WiFi creds + API keys + AP creds
└── meeting_2026-05-15_14-30-00/      # Per-meeting directory
    ├── chunk_0.wav                   # Raw 15-second audio chunks
    ├── chunk_0.txt                   # Per-chunk transcript (debug)
    ├── chunk_1.wav
    ├── chunk_1.txt
    ├── ...
    ├── full_transcript.txt           # Concatenated full transcript
    │                                 #   (source of truth, append-only)
    └── summary_final.txt             # Final GPT summary
```

The `/api/history` route enumerates `/meeting_*/` and surfaces it in the UI.
The `/api/history/regenerate` route reads `full_transcript.txt` from a past
meeting directory and re-runs the final-summary pipeline.

---

## 5. WiFi Strategy

The firmware picks a WiFi mode at boot based on whether credentials are
present and whether STA connects:

```
boot
 │
 ▼
loadConfig()
 │
 ▼
wifiSSID set?
 ├── NO  ──▶ WIFI_AP only
 │           (first-time setup — connect to "MeetingRecorder",
 │            open 192.168.4.1/setup)
 │
 └── YES ──▶ WIFI_STA only
             WiFi.begin(...)
              │
              ▼
             Connected within 15 s?
              ├── YES ──▶ Start mDNS as "meetingrecorder"
              │           AP stays off — dashboard via
              │           http://meetingrecorder.local
              │
              └── NO  ──▶ WIFI_AP_STA recovery
                          (AP up + retry STA — open /setup over AP)
```

After the user saves new creds via `/setup`, the `loop()`'s
`needWifiReconnect` branch keeps the AP up during the connect attempt
(so the success page can still be served if the new creds turn out
wrong), and on success: restarts mDNS, then shuts down the AP and
switches to STA-only.

---

## 6. Power Modes

- **Boot:** `btStop()` disables Bluetooth controller (~5-10 mA saved).
- **Idle:** CPU at 80 MHz + WiFi modem sleep + LED off (~50 mA combined save vs. no-optimisation).
- **Recording / processing:** CPU at 240 MHz + WiFi awake. LED gives one
  80 ms pulse every 5 s as a heartbeat indicator.
- **After final summary:** drops back to 80 MHz, but only if a new meeting
  hasn't been started in the meantime (race-guard with `!meetingActive`).

---

## 7. Module Responsibilities

| Module          | Owns                                                                 |
| --------------- | -------------------------------------------------------------------- |
| `config`        | Reading & writing `/config.json`                                     |
| `audio`         | PDM I²S setup, WAV header, `recordTask`                              |
| `api`           | ElevenLabs STT, OpenAI calls (rolling / final / segment / synthesis), multipart upload |
| `api_chat`      | `/api/chat` Q&A endpoint logic                                       |
| `process`       | `processTask`, final-summary pipeline (single-call + map-reduce), regenerate helper, SD directory helpers |
| `web_handlers`  | Route registration + main page handlers (`/`, `/setup`, `/api/start` etc.) |
| `web_extras`    | `/api/status`, `/api/chat`, `/api/settime` endpoints                 |
| `ntp_time`      | NTP sync + browser-supplied fallback                                 |
| `html_pages`    | `PAGE_MAIN` + `PAGE_SETUP` PROGMEM strings                           |

A per-file reference is in [FILES.md](FILES.md).

---

## 8. Failure Modes & Guards

- **No internet:** `ensureWiFi()` retries before every HTTP op. STT / GPT
  failures fall back to placeholder transcripts / "Summary unavailable".
- **Final summary GPT call fails:** the saved `summary_final.txt` is the
  last rolling summary as a fallback. Users can hit the **↺ Regenerate**
  button on the History tab to re-run the full pipeline from the saved
  `full_transcript.txt`.
- **Slow HTTP:** Doesn't block UI — `webTask` is on its own RTOS task with
  higher priority than `processTask`.
- **SD full / removed mid-meeting:** `recordTask` aborts the current chunk
  and surfaces an error on the dashboard via `/api/status`. The
  per-chunk transcripts that did make it to SD are preserved.
- **AP credential change:** `loop()` watches `needApRestart` and re-arms
  `softAP` *after* the HTTP response has been flushed (otherwise the
  client would never see the success page).
- **Long meeting OOM risk:** prevented by map-reduce — only one 25 KB
  segment is in RAM at a time, no matter how long the meeting.
- **User starts the next meeting mid-finalisation:** the CPU drop at the
  end of the final-summary block is guarded by `!meetingActive`, so the
  new recording never sees CPU drop under it.




Tools → Board → ESP32S3 Dev Module
Tools → PSRAM → "OPI PSRAM"           ← MUST
Tools → Flash Size → 16MB (128Mb)
Tools → Partition Scheme → 16M Flash (3MB APP/9.9MB FATFS)
Tools → USB CDC On Boot → Enabled     ← Serial debug ke liye
Tools → USB Mode → Hardware CDC and JTAG