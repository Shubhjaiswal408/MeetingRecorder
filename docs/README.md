# Meeting Recorder — XIAO ESP32-S3

A standalone, battery-friendly meeting recorder built on the Seeed XIAO ESP32-S3
Sense expansion board. Captures audio with the on-board PDM microphone, stores
15-second WAV chunks to a microSD card, and uses ElevenLabs Scribe + OpenAI
GPT-4o-mini to produce live transcripts and rolling summaries — all served from
a built-in web UI.

---

## 1. Hardware

| Component                 | Notes                                                      |
| ------------------------- | --------------------------------------------------         |
| Seeed XIAO ESP32-S3 Sense | Provides PDM mic on GPIO 41 / 42                           |
| microSD card (FAT32)      | Inserted into the Sense expansion board, CS = D2 (GPIO 21) |
| Push-button               | Wired between **D1 (GPIO 1)** and **GND**                  |
| Built-in user LED         | Status indicator (ON = idle, OFF = recording)              |

Pin assignments live in [`src/globals.h`](../src/globals.h).

---

## 2. Build & Flash

1. Install **Arduino IDE 2.x**.
2. Add the **ESP32 board package** (Boards Manager → "esp32" by Espressif).
3. Select board: **XIAO_ESP32S3**.
4. Required libraries — **nothing to install via Library Manager**:
   - `Shubh'Json` — bundled inside `src/json/` (zero-install JSON parser, see `src/json/ShubhJson.h`)
   - `HTTPClient`, `WiFi`, `WebServer`, `SD`, `FS` — all bundled with the ESP32 core
5. Open `MeetingRecorder.ino` and click **Upload**.

> The IDE automatically compiles every `.cpp` file inside `src/`. The main
> sketch references those headers via `#include "src/foo.h"` — no extra build
> flags are required.

---

## 3. First-Time Setup

1. Power on the board. A WiFi hotspot called **`MeetingRecorder`**
   (password `recorder123`) will appear.
2. Connect to it from a phone or laptop and open
   <http://192.168.4.1/setup>.
3. Enter:
   - Home WiFi SSID + password
   - **ElevenLabs** API key (Scribe speech-to-text)
   - **OpenAI** API key (GPT-4o-mini summaries)
4. Save. The board reconnects to your home network; the AP stays available as a
   fallback.

Credentials are stored in `/config.json` on the SD card.

---

## 4. Running a Meeting

You can start / stop recording in two ways:

- **Hardware:** press the button on D1.
- **Web UI:** open <http://meeting-recorder.local/> (or the LAN IP shown in
  Serial Monitor) and click **Start / Stop**.

While a meeting is active:

- Audio is captured in 15-second WAV chunks (`SAMPLE_RATE = 16 kHz`, 16-bit).
- Each chunk is transcribed by ElevenLabs, appended to the rolling transcript,
  and re-summarised by GPT-4o-mini.
- The UI updates live via `/api/status`.

When you stop, a final summary is generated and the meeting is archived under
`/meetings/<YYYY-MM-DD_HH-MM-SS>/` on the SD card.

---

## 5. Project Layout

```
MeetingRecorder/
├── MeetingRecorder.ino         # setup(), loop(), global definitions
├── ShubhJson/                  # Bundled Shubh'Json internals (don't edit)
├── src/                        # All module sources (Arduino auto-compiles recursively)
│   ├── core/
│   │   └── globals.h           # Shared #defines + extern declarations
│   ├── config/
│   │   ├── config.h
│   │   └── config.cpp          # config.json load/save
│   ├── audio/
│   │   ├── audio.h
│   │   └── audio.cpp           # PDM mic, WAV header, recordTask (Core 0)
│   ├── api/
│   │   ├── api.h / api.cpp     # ElevenLabs STT + OpenAI summarisation
│   │   ├── api_chat.h          # /api/chat Q&A endpoint
│   │   └── api_chat.cpp
│   ├── process/
│   │   ├── process.h
│   │   └── process.cpp         # processTask (Core 1) + SD helpers
│   ├── web/
│   │   ├── web_handlers.{h,cpp} # HTTP routes
│   │   ├── web_extras.{h,cpp}   # /api/status, /api/chat, /api/settime
│   │   └── html_pages.h         # PROGMEM HTML pages (UI)
│   ├── time/
│   │   └── ntp_time.h           # NTP + browser-time sync
│   └── json/
│       ├── ShubhJson.h          # Shubh'Json wrapper — what user code should #include
│       ├── ShubhJson_core.h     # Entry-point (used internally by ShubhJson.h)
│       └── ShubhJson_core.hpp   # Entry-point (C++ side)
└── docs/
    ├── README.md               # This file
    ├── ARCHITECTURE.md         # Runtime architecture & dataflow
    ├── FILES.md                # File-by-file reference
    └── ShubhJson_LICENSE.txt   # MIT license for the bundled JSON parser
```

> **Why the `ShubhJson/` folder lives at the sketch root:** Arduino IDE only
> adds the sketch's root directory (the folder holding `.ino`) to the
> compiler's include path. The bundled library's internal headers use
> angle-include directives (`#include <ShubhJson/...>`) that need this
> include path to resolve. Keeping the folder at root is the canonical
> Arduino way to bundle a header-only library. The user-facing wrapper
> `src/json/ShubhJson.h` is what you actually `#include` in your code —
> the folder at root is invisible plumbing.

See [ARCHITECTURE.md](ARCHITECTURE.md) for a deeper look at the runtime and
[FILES.md](FILES.md) for a per-file reference.

---

## 6. Web API Cheat-Sheet

| Method | Route                  | Purpose                              |
| ------ | ---------------------- | ------------------------------------ |
| GET    | `/`                    | Dashboard HTML                       |
| GET    | `/setup`               | Configuration page                   |
| GET    | `/api/status`          | Live JSON state (transcript, summary, etc.) |
| POST   | `/api/start`           | Start a meeting                      |
| POST   | `/api/stop`            | Stop and finalise the meeting        |
| POST   | `/api/config`          | Save WiFi + API credentials          |
| GET    | `/api/history`         | List past meetings on the SD card    |
| POST   | `/api/history/delete`  | Delete a stored meeting              |
| POST   | `/api/chat`            | Ask GPT a question about the transcript |
| POST   | `/api/settime`         | Sync clock from the browser          |

---

## 7. Troubleshooting

| Symptom                          | Likely cause / fix                            |
| -------------------------------- | --------------------------------------------- |
| `SD init FAILED` on boot         | Card not FAT32, not inserted, or CS pin wrong |
| `Mic init FAILED`                | Sense expansion board not seated              |
| `WiFi TIMEOUT — AP only.`        | Wrong credentials → open `/setup` over AP     |
| Web UI freezes during STT call   | Should not happen — `webTask` is on its own RTOS task |
| Transcript empty after stopping  | Check ElevenLabs key + remaining quota        |
