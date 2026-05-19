# File-by-File Reference

This is the canonical map of every source file in the project, what it owns,
and what to look at when you need to change a particular behaviour.

| File                  | Lines (approx) | Responsibility                                    |
| --------------------- | -------------- | ------------------------------------------------- |
| `MeetingRecorder.ino` | ~260           | `setup()`, `loop()`, global variable definitions, `webTask` |
| `src/globals.h`       | ~95            | Pins, constants, `extern` shared state            |
| `src/config.h/.cpp`   | small          | Load / save `/config.json`                        |
| `src/audio.h/.cpp`    | medium         | PDM mic init, WAV header, `recordTask` (Core 0)   |
| `src/api.h/.cpp`      | medium         | ElevenLabs STT + OpenAI summarisation, multipart upload stream |
| `src/api_chat.h/.cpp` | small          | `/api/chat` GPT Q&A backend                       |
| `src/process.h/.cpp`  | medium         | `processTask` (Core 1), SD meeting-dir helpers    |
| `src/web_handlers.h/.cpp` | medium     | HTTP route registration + page handlers           |
| `src/web_extras.h/.cpp`   | medium     | Live status, chat, browser-time-set endpoints     |
| `src/ntp_time.h`      | small          | NTP sync + browser-time fallback                  |
| `src/html_pages.h`    | large          | PROGMEM dashboard + setup HTML                    |

---

## MeetingRecorder.ino

The Arduino entry point. Three responsibilities:

1. **Global definitions** — every variable declared `extern` in `globals.h`
   has its single definition here.
2. **`setup()`** — initialises Serial, GPIO, SD card, microphone, WiFi (AP +
   STA), NTP, mutex, meeting directory, and spawns the three RTOS tasks.
3. **`loop()`** — handles physical button presses, WiFi reconnection requests
   (`needWifiReconnect`), and AP restarts (`needApRestart`).

The web server has its own task (`webTask`) — do **not** add
`server.handleClient()` to `loop()`.

## src/globals.h

Shared header included by every module. Holds:

- AP/WiFi `#define`s and default credentials
- Audio constants (`SAMPLE_RATE`, `CHUNK_SECONDS`, etc.)
- Pin assignments
- `extern` declarations for every cross-module variable

> When adding new shared state, add the `extern` here and the single
> definition in `MeetingRecorder.ino`.

## src/config.{h,cpp}

`loadConfig()` reads `/config.json` from the SD card and populates the
credential globals. `saveConfig()` writes them back. Uses `Shubh'Json`
(the bundled JSON library in `src/json/`).

## src/audio.{h,cpp}

- `initMic()` — configures the I²S PDM RX channel on GPIO 41/42 at 16 kHz.
- `generateWavHeader()` — produces a 44-byte PCM WAV header.
- `recordTask()` — FreeRTOS task pinned to Core 0. Captures audio into 15 s
  WAV chunks and signals `processTask` via `chunkMutex`.

## src/api.{h,cpp}

The network-side brain:

- `ensureWiFi()` — guard called before every HTTP request.
- `transcribeAudio(path)` — uploads a WAV to ElevenLabs Scribe using the
  `MultipartUploadStream` helper (streams from SD without buffering).
- `generateSummary(transcript, isFinal)` — calls OpenAI GPT-4o-mini with the
  rolling-vs-final prompt template.
- `jsonEscape()` — utility used by every JSON response builder.
- `MultipartUploadStream` — Arduino `Stream` subclass that splices header +
  file + footer so `HTTPClient.sendRequest()` can stream a multipart body
  without loading the entire WAV into RAM.

## src/api_chat.{h,cpp}

Implements the meeting-Q&A endpoint. Takes a user question + the current
transcript and asks GPT for a grounded answer.

## src/process.{h,cpp}

- `processTask()` — Core 1 task. Picks up chunks signalled by `recordTask`,
  runs STT, appends to `fullTranscript`, requests a rolling summary, and on
  `finalStop` writes `transcript.txt` + `summary.txt`.
- `createMeetingDir()` — makes `/meetings/<timestamp>/` and sets
  `meetingDir`.
- `deleteDirRecursive(path)` — used by `/api/history/delete`.

## src/web_handlers.{h,cpp}

Owns `startWebServer()`. Registers:

| Route                     | Method | Purpose                                         |
| ------------------------- | ------ | ----------------------------------------------- |
| `/`                       | GET    | Serves `PAGE_MAIN` from `html_pages.h`          |
| `/setup`                  | GET    | Serves `PAGE_SETUP`                             |
| `/api/start`              | POST   | Starts a meeting                                |
| `/api/stop`               | POST   | Stops + finalises                               |
| `/api/config`             | POST   | Persists creds, triggers `needWifiReconnect`/`needApRestart` |
| `/api/history`            | GET    | Lists `/meetings/` directories                  |
| `/api/history/delete`     | POST   | Deletes a meeting directory                     |

## src/web_extras.{h,cpp}

Endpoints that grew large enough to deserve their own file:

- `/api/status` — live JSON blob consumed by the dashboard's poll loop.
- `/api/chat`   — proxies to `api_chat.cpp`.
- `/api/settime` — accepts a browser-supplied epoch when NTP fails.

## src/ntp_time.h

Header-only helpers:

- `ntpInit()` — sets the timezone, runs `configTzTime()`, and updates
  `ntpSynced`.
- `stampNow()` — fills `meetingTimestamp` + `meetingDisplayTime` from the
  current clock.

## src/html_pages.h

Two `PROGMEM` C-string constants — `PAGE_MAIN` (dashboard) and `PAGE_SETUP`
(config). Holds the entire UI: HTML, CSS, and inline JS. Edit this file to
change the look or behaviour of the web UI; no build step is needed since
`html_pages.h` is recompiled into the firmware on upload.
