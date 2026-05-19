# Architecture

## 1. Runtime Topology

The firmware uses three pinned FreeRTOS tasks plus the Arduino `loop()` so that
audio capture, the web UI, and slow HTTP calls never block each other.

```
┌──────────────────── Core 0 ────────────────────┐  ┌──────────────────── Core 1 ────────────────────┐
│                                                │  │                                                │
│   recordTask (prio 2)                          │  │   webTask     (prio 3)  ← keeps UI responsive  │
│   ─ PDM mic → 15 s WAV chunks                  │  │   ─ server.handleClient() on a tight loop      │
│   ─ Writes to SD: /meetings/<ts>/chunkNN.wav   │  │                                                │
│   ─ Signals chunkReady via chunkMutex          │  │   processTask (prio 1)                         │
│                                                │  │   ─ Waits on chunkReady                        │
│                                                │  │   ─ POSTs WAV to ElevenLabs (STT)              │
│                                                │  │   ─ Appends to fullTranscript                  │
│                                                │  │   ─ POSTs to OpenAI for rolling summary        │
│                                                │  │   ─ On finalStop: writes final summary + .txt  │
│                                                │  │                                                │
│                                                │  │   loop() (Arduino, prio 1)                     │
│                                                │  │   ─ Button polling, WiFi reconnect, AP restart │
└────────────────────────────────────────────────┘  └────────────────────────────────────────────────┘
```

Why this split?

- **STT/GPT calls block for seconds.** If they ran in `loop()` the web UI
  would freeze. `processTask` owns them, `webTask` keeps the dashboard
  responsive throughout.
- **PDM capture is real-time.** Putting it on its own core (Core 0) prevents
  audio drops while Core 1 is busy doing HTTP work.

---

## 2. Data Flow — A Single Meeting

```
┌─ Button / Web UI ─┐
│   meetingActive = true; stampNow() sets meetingTimestamp                │
└─────────┬─────────┘
          │
          ▼
   recordTask (Core 0) ──── 15 s WAV ────► /meetings/<ts>/chunkNN.wav
          │
          │ chunkReady = true
          ▼
   processTask (Core 1)
          │
          ├─► ElevenLabs STT      → chunkNN.txt + appended to fullTranscript
          │
          └─► OpenAI summarise    → rollingSummary
          │
          │ webTask serves /api/status with the latest state
          ▼
   ── repeat until user stops ──
          │
          ▼
   finalStop = true
          │
          ▼
   processTask
          ├─► OpenAI: final summary (whole transcript)
          ├─► Writes transcript.txt + summary.txt
          └─► Resets all meeting state
```

---

## 3. Shared State

Everything cross-module lives in [`src/globals.h`](../src/globals.h) as
`extern` declarations. The **single definition** of every global lives in
`MeetingRecorder.ino` so the linker resolves them exactly once.

Categories:

- **Credentials** — `wifiSSID`, `wifiPass`, `elApiKey`, `openaiApiKey`,
  `apSSID`, `apPass`. Loaded from `/config.json` at boot, rewritable via UI.
- **Meeting state** — `meetingActive`, `chunkReady`, `finalStop`,
  `currentChunkPath`, `meetingDir`, `fullTranscript`, `rollingSummary`,
  `chunkIndex`, `wordCount`.
- **Time** — `meetingTimestamp`, `meetingDisplayTime`, `ntpSynced`,
  `meetingStartEpoch`.
- **RTOS handles** — `recordTaskHandle`, `processTaskHandle`, `chunkMutex`,
  `rx_handle`.
- **Web server** — `server` (port 80).

`volatile` is used for flags read by one task and written by another;
`chunkMutex` guards the chunk handoff between `recordTask` and `processTask`.

---

## 4. Storage Layout on the SD Card

```
/
├── config.json                       # WiFi creds + API keys + AP creds
└── meetings/
    └── 2026-05-19_14-30-00/          # Per-meeting directory
        ├── chunk00.wav               # Raw 15-second audio chunks
        ├── chunk00.txt               # Per-chunk transcript (debug)
        ├── chunk01.wav
        ├── chunk01.txt
        ├── ...
        ├── transcript.txt            # Concatenated full transcript
        └── summary.txt               # GPT-generated final summary
```

The `/api/history` route enumerates `/meetings/` and surfaces it in the UI.

---

## 5. Module Responsibilities

| Module          | Owns                                                  |
| --------------- | ----------------------------------------------------- |
| `config`        | Reading & writing `/config.json`                      |
| `audio`         | PDM I²S setup, WAV header, `recordTask`               |
| `api`           | ElevenLabs STT, OpenAI summarisation, multipart upload |
| `api_chat`      | The `/api/chat` Q&A endpoint logic                    |
| `process`       | `processTask`, meeting directory helpers              |
| `web_handlers`  | Route registration + page handlers                    |
| `web_extras`    | `/api/status`, `/api/chat`, `/api/settime` endpoints  |
| `ntp_time`      | NTP sync + browser-supplied fallback                  |
| `html_pages`    | `PAGE_MAIN` + `PAGE_SETUP` PROGMEM strings            |

A per-file reference is in [FILES.md](FILES.md).

---

## 6. Failure Modes & Guards

- **No internet:** `ensureWiFi()` retries before every HTTP op. STT/GPT
  failures fall back to a placeholder transcript / "Summary unavailable".
- **Slow HTTP:** Doesn't block UI — `webTask` is on its own RTOS task with
  higher priority than `processTask`.
- **SD full / removed mid-meeting:** `recordTask` aborts the current chunk
  and surfaces an error on the dashboard via `/api/status`.
- **AP credential change:** `loop()` watches `needApRestart` and re-arms
  `softAP` *after* the HTTP response has been flushed (otherwise the client
  would never see the success page).
