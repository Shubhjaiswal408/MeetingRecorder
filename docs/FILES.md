# File-by-File Reference

Canonical map of every source file in the project — what it owns and what to
edit when you want to change a particular behaviour.

| File                              | Responsibility                                                              |
| --------------------------------- | --------------------------------------------------------------------------- |
| `MeetingRecorder.ino`             | `setup()`, `loop()`, global variable definitions, LED state machine, `webTask` |
| `src/core/globals.h`              | Pins, constants, `extern` shared state                                       |
| `src/config/config.{h,cpp}`       | Load / save `/config.json`                                                  |
| `src/audio/audio.{h,cpp}`         | PDM mic init, WAV header, `recordTask` (Core 0)                              |
| `src/api/api.{h,cpp}`             | ElevenLabs STT, OpenAI summarisation (rolling + final + segment + synthesis), multipart upload stream |
| `src/api/api_chat.{h,cpp}`        | `/api/chat` GPT Q&A backend                                                 |
| `src/process/process.{h,cpp}`     | `processTask` (Core 1), final-summary pipeline, regenerate helper, SD helpers |
| `src/web/web_handlers.{h,cpp}`    | HTTP route registration + page handlers                                      |
| `src/web/web_extras.{h,cpp}`      | Live status, chat, browser-time-set endpoints                                |
| `src/web/html_pages.h`            | PROGMEM dashboard + setup HTML / CSS / JS                                    |
| `src/time/ntp_time.h`             | NTP sync + browser-time fallback                                             |
| `src/json/ShubhJson.h`            | User-facing wrapper for the bundled JSON parser                              |

---

## `MeetingRecorder.ino`

Three responsibilities:

1. **Global definitions** — every variable declared `extern` in `globals.h`
   has its single definition here. This is where the linker pulls them from.
2. **`setup()`** — boot sequence: Serial, GPIO, SD, mic, smart WiFi mode,
   NTP, mutexes, chunk queue, meeting directory, RTOS tasks, idle CPU/power
   mode. `btStop()` is called here too.
3. **`loop()`** — handles physical button presses, WiFi reconnection
   requests (`needWifiReconnect`), AP restarts (`needApRestart`), and the
   **LED heartbeat state machine** (one short pulse every 5 s while busy,
   OFF when idle).

The web server has its own task (`webTask`, also defined in this file) — do
**not** add `server.handleClient()` to `loop()`.

---

## `src/core/globals.h`

Shared header included by every module. Holds:

- AP/WiFi `#define`s and default credentials
- Audio constants (`SAMPLE_RATE`, `CHUNK_SECONDS`, etc.)
- Pin assignments (button = D1, SD CS = GPIO 21, mic = GPIO 41/42)
- `CHUNK_QUEUE_SIZE` / `CHUNK_PATH_LEN` for the inter-task chunk queue
- `extern` declarations for every cross-module variable

> When adding new shared state, declare it `extern` here and define it in
> `MeetingRecorder.ino` (single source of truth).

---

## `src/config/config.{h,cpp}`

`loadConfig()` reads `/config.json` from the SD card and populates the
credential globals (`wifiSSID`, `wifiPass`, `elApiKey`, `openaiApiKey`,
`apSSID`, `apPass`). `saveConfig()` writes them back. Uses Shubh'Json (the
bundled JSON library in `src/json/`).

---

## `src/audio/audio.{h,cpp}`

- `initMic()` — configures the I²S PDM RX channel on GPIO 41/42 at 16 kHz.
- `generateWavHeader()` — produces a 44-byte PCM WAV header.
- `recordTask()` — FreeRTOS task pinned to Core 0. Captures audio into
  15 s WAV chunks and posts the path to `chunkQueue`. A queue full event
  (which only happens after several minutes of back-to-back STT failures)
  is logged as a warning rather than silently dropped.

---

## `src/api/api.{h,cpp}`

The network-side brain. Major functions:

- `ensureWiFi()` — guard called before every HTTP request. Reconnects
  on the existing creds if STA dropped.
- `transcribeAudio(path)` — uploads a WAV to ElevenLabs Scribe using
  `MultipartUploadStream` so the file is streamed from SD without
  buffering the whole thing.
- `generateSummary(transcript, isFinal)` — single GPT call with the
  rolling-vs-final prompt template. Used for live rolling summaries
  during a meeting AND for short-meeting final summaries.
- `generateSegmentSummary(segment, n, total)` — **map step** of the
  long-meeting pipeline. Detailed summary of one ≈ 25 KB transcript
  segment.
- `synthesizeFinalSummary(combined)` — **reduce step**. Takes the
  concatenated segment summaries and produces the single polished
  final 5-section summary.
- `jsonEscape()` — JSON string-escape utility used by every JSON
  response builder.
- `MultipartUploadStream` — Arduino `Stream` subclass that splices
  header + file + footer so `HTTPClient.sendRequest()` can stream a
  multipart body without loading the entire WAV into RAM.

All GPT calls go through internal helpers `_gptCallOnce` (single
attempt) and `_gptCallRetry` (3-attempt with WiFi-bounce). Timeouts:
30 s for rolling, 90 s for final, 60-120 s for segment / synthesis.

---

## `src/api/api_chat.{h,cpp}`

Implements the meeting-Q&A endpoint. Takes a user question + the
current or past meeting's transcript and asks GPT for a grounded
answer. Conversation memory of 6 turns. Snapshots shared state under
`stateMutex` before the slow HTTP call.

---

## `src/process/process.{h,cpp}`

- `processTask()` — Core 1 task. Pulls chunk paths from `chunkQueue`,
  runs STT, appends the per-chunk transcript to SD's
  `full_transcript.txt` (append mode), requests a rolling summary, and
  on `finalStop` runs the **final-summary pipeline**:

  - Reads transcript size from SD
  - **≤ 25 KB:** single GPT call via `generateSummary(transcript, true)`
  - **> 25 KB:** map-reduce — streams 25 KB segments off SD, calls
    `generateSegmentSummary()` per segment, then
    `synthesizeFinalSummary()` over the combined summaries

  Updates `finalSummaryText` and writes `summary_final.txt`. Sets
  `processingFinal` true while busy so the LED keeps blinking through
  the (potentially multi-minute) process.

- `regenerateSummaryForMeeting(dir)` — standalone version of the same
  pipeline, runs against a past meeting's saved transcript. Called by
  `/api/history/regenerate`.

- `readFullTranscriptFromSD(dir)` — loads `full_transcript.txt` into a
  String (used in the single-call path).

- `createMeetingDir()` — makes `/meeting_<timestamp>/` and sets
  `meetingDir`.

- `deleteDirRecursive(path)` — used by `/api/history/delete` and
  `/api/factory-reset`.

---

## `src/web/web_handlers.{h,cpp}`

Owns `startWebServer()`. Registered routes:

| Route                          | Method | Purpose                                                          |
| ------------------------------ | ------ | ---------------------------------------------------------------- |
| `/`                            | GET    | Serves `PAGE_MAIN` (dashboard) from `html_pages.h`               |
| `/setup`                       | GET    | Serves `PAGE_SETUP`                                              |
| `/api/start`                   | POST   | Starts a meeting (bumps CPU to 240 MHz, resets state)            |
| `/api/stop`                    | POST   | Stops + triggers final-summary pipeline                          |
| `/api/config`                  | POST   | Persists creds, triggers `needWifiReconnect` / `needApRestart`   |
| `/api/history`                 | GET    | Lists `/meeting_*/` directories, filters out empty / failed ones |
| `/api/history/delete`          | POST   | Recursively deletes a meeting directory                          |
| `/api/history/regenerate`      | POST   | Re-runs the final-summary pipeline on a past meeting             |
| `/api/factory-reset`           | POST   | Wipes meetings + config, reboots                                 |

---

## `src/web/web_extras.{h,cpp}`

Endpoints that grew large enough to deserve their own file:

- `/api/status` — live JSON blob consumed by the dashboard's poll loop
  (transcript tail, summary, network info, RAM, uptime, meeting state).
  Snapshots shared state under `stateMutex` before building the JSON.
- `/api/chat`   — proxies to `api_chat.cpp`.
- `/api/settime` — accepts a browser-supplied epoch when NTP fails.

---

## `src/time/ntp_time.h`

Header-only helpers:

- `ntpInit()` — sets the timezone, runs `configTzTime()`, updates
  `ntpSynced`. Tries `pool.ntp.org`, `time.google.com`,
  `time.cloudflare.com` in sequence.
- `stampNow()` — fills `meetingTimestamp` + `meetingDisplayTime` from
  the current clock.
- `setBrowserTime(epochMs)` — fallback when NTP fails — the dashboard
  posts `Date.now()` to `/api/settime` and we use it directly.

---

## `src/web/html_pages.h`

Two `PROGMEM` C-string constants — `PAGE_MAIN` (the dashboard) and
`PAGE_SETUP` (the first-time setup page). Holds the entire UI: HTML,
CSS, and inline JS.

Notable client-side logic:

- `renderMd(text)` — minimal markdown → HTML renderer (`##` headings,
  `-` bullets, numbered lists, `**bold**`, `*italic*`).
- Polling loop pauses when `document.hidden` (saves device RAM and
  phone battery when the tab is in the background).
- Clipboard copy has an `execCommand` fallback so it works over HTTP
  (`navigator.clipboard` only works on HTTPS / localhost).
- History tab buttons: **Ask AI**, **Download**, **↺ Regenerate**,
  **Delete** per meeting card.
- Settings tab has a **Danger Zone → Factory Reset** card at the
  bottom with two-step confirmation.

Edit this file to change look or behaviour of the web UI — no build
step needed; `html_pages.h` is recompiled into the firmware on every
upload.

---

## `src/json/ShubhJson.{h,...}` + `ShubhJson/`

Bundled, zero-install JSON parser derived from ArduinoJson. The
user-facing wrapper `src/json/ShubhJson.h` is what your code should
`#include` — the parser internals live in the `ShubhJson/` folder at
the sketch root (so Arduino IDE's include path resolves the angle-
includes).

The original ArduinoJson MIT license is preserved verbatim at
[ShubhJson_LICENSE.txt](ShubhJson_LICENSE.txt) as required.
