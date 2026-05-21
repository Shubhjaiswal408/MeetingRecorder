# Meeting Recorder — XIAO ESP32-S3

A standalone AI meeting recorder built on the Seeed XIAO ESP32-S3 Sense expansion
board. Captures audio with the on-board PDM microphone, stores 15-second WAV
chunks to a microSD card, and uses ElevenLabs Scribe + OpenAI GPT-4o-mini to
produce live transcripts, rolling summaries, and a comprehensive final summary
— all served from a built-in web UI accessible at `meetingrecorder.local` from
any device on the same WiFi.

Handles meetings of **any duration** via a chunked map-reduce summarisation
pipeline, so a 4-hour conference is summarised as accurately as a 5-minute test.

---

## 1. Hardware

| Component                 | Notes                                                      |
| ------------------------- | --------------------------------------------------         |
| Seeed XIAO ESP32-S3 Sense | Provides PDM mic on GPIO 41 / 42                           |
| microSD card (FAT32)      | Inserted into the Sense expansion board, CS = D2 (GPIO 21) |
| Push-button               | Wired between **D1 (GPIO 1)** and **GND**                  |
| LiPo battery (optional)   | Solder to BAT+ / BAT- pads on the back of the XIAO         |
| Built-in user LED         | Heartbeat blink while busy, OFF when idle                  |

Pin assignments live in [`src/core/globals.h`](../src/core/globals.h).

---

## 2. Build & Flash

1. Install **Arduino IDE 2.x**.
2. Add the **ESP32 board package** (Boards Manager → "esp32" by Espressif).
3. Select board: **XIAO_ESP32S3**.
4. Required libraries — **nothing to install via Library Manager**:
   - `Shubh'Json` — bundled inside `src/json/` (zero-install JSON parser)
   - `HTTPClient`, `WiFi`, `WebServer`, `SD`, `FS`, `ESPmDNS` — all bundled with the ESP32 core
5. Open `MeetingRecorder.ino` and click **Upload**.

> The IDE automatically compiles every `.cpp` file inside `src/`. The main
> sketch references those headers via `#include "src/foo/bar.h"` — no extra
> build flags are required.

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
4. Save. The board reconnects to your home network, the AP shuts down, and
   the dashboard is now reachable at **<http://meetingrecorder.local>** from
   any device on the same WiFi — your phone/laptop keeps full internet access.

Credentials are stored in `/config.json` on the SD card.

---

## 4. Running a Meeting

Two ways to start / stop:

- **Hardware:** press the button on D1.
- **Web UI:** open <http://meetingrecorder.local/> and click **Start / Stop**.

While a meeting is active:

- Audio is captured in 15-second WAV chunks (`SAMPLE_RATE = 16 kHz`, 16-bit).
- Each chunk is transcribed by ElevenLabs, appended to the rolling transcript
  on SD, and re-summarised by GPT-4o-mini.
- The UI updates live via `/api/status` every 2.5 s (and pauses when the tab
  is hidden, to save mobile battery).
- The built-in LED gives a slow heartbeat blink (one short flash every 5 s)
  so you can confirm the device is still working from across the room.

When you stop, the device picks one of two summary strategies based on
transcript length:

- **≤ 25 KB transcript (≈ 30 min meeting):** single GPT call with a
  comprehensive prompt → polished 5-section summary in ~20 s.
- **> 25 KB transcript:** **map-reduce** — the SD-stored transcript is
  streamed in 25 KB segments, each summarised separately, then the segment
  summaries are merged by a final synthesis call. This is what makes
  unlimited-duration meetings work on a 320 KB-RAM ESP32: only one segment
  is ever in RAM at a time.

The final summary is saved to `/meeting_<timestamp>/summary_final.txt` on
the SD card.

---

## 5. Project Layout

```
MeetingRecorder/
├── MeetingRecorder.ino         # setup(), loop(), global definitions
├── ShubhJson/                  # Bundled Shubh'Json internals (don't edit)
├── src/                        # Module sources (Arduino auto-compiles recursively)
│   ├── core/
│   │   └── globals.h           # Shared #defines + extern declarations
│   ├── config/
│   │   └── config.{h,cpp}      # config.json load/save
│   ├── audio/
│   │   └── audio.{h,cpp}       # PDM mic, WAV header, recordTask (Core 0)
│   ├── api/
│   │   ├── api.{h,cpp}         # ElevenLabs STT + OpenAI summarisation
│   │   │                       #   - generateSummary (rolling + short-meeting final)
│   │   │                       #   - generateSegmentSummary (map step)
│   │   │                       #   - synthesizeFinalSummary (reduce step)
│   │   └── api_chat.{h,cpp}    # /api/chat Q&A endpoint
│   ├── process/
│   │   └── process.{h,cpp}     # processTask (Core 1), final-summary pipeline,
│   │                           #   regenerateSummaryForMeeting, SD helpers
│   ├── web/
│   │   ├── web_handlers.{h,cpp} # HTTP routes
│   │   ├── web_extras.{h,cpp}   # /api/status, /api/chat, /api/settime
│   │   └── html_pages.h         # PROGMEM HTML pages (Dashboard UI)
│   ├── time/
│   │   └── ntp_time.h           # NTP + browser-time sync
│   └── json/
│       ├── ShubhJson.h          # User-facing wrapper — what code includes
│       ├── ShubhJson_core.h     # Entry-point (used internally)
│       └── ShubhJson_core.hpp   # Entry-point (C++ side)
└── docs/
    ├── README.md                # This file
    ├── ARCHITECTURE.md          # Runtime architecture & dataflow
    ├── FILES.md                 # File-by-file reference
    └── ShubhJson_LICENSE.txt    # MIT license for the bundled JSON parser
```

> **Why `ShubhJson/` lives at the sketch root:** Arduino IDE only adds the
> sketch's root directory to the compiler's include path. The bundled
> library's internal headers use angle-include directives
> (`#include <ShubhJson/...>`) that need this include path to resolve.
> Keeping the folder at root is the canonical Arduino way to bundle a
> header-only library. The wrapper `src/json/ShubhJson.h` is what user
> code actually `#include`s — the folder at root is invisible plumbing.

See [ARCHITECTURE.md](ARCHITECTURE.md) for a deeper look at the runtime and
[FILES.md](FILES.md) for a per-file reference.

---

## 6. Web API Cheat-Sheet

| Method | Route                          | Purpose                              |
| ------ | ------------------------------ | ------------------------------------ |
| GET    | `/`                            | Dashboard HTML                       |
| GET    | `/setup`                       | Configuration page                   |
| GET    | `/api/status`                  | Live JSON state (transcript, summary, network, RAM, uptime) |
| POST   | `/api/start`                   | Start a meeting                      |
| POST   | `/api/stop`                    | Stop and finalise the meeting        |
| POST   | `/api/config`                  | Save WiFi / API keys / AP credentials |
| GET    | `/api/history`                 | List past meetings stored on the SD card |
| POST   | `/api/history/delete`          | Delete a stored meeting              |
| POST   | `/api/history/regenerate`      | Re-run the final-summary pipeline on a past meeting using its saved transcript |
| POST   | `/api/factory-reset`           | Wipe all meetings + credentials, reboot into AP setup mode |
| POST   | `/api/chat`                    | Ask GPT a question about a current or past meeting |
| POST   | `/api/settime`                 | Sync clock from the browser          |

---

## 7. Power & Battery

The firmware automatically optimises power for battery use:

- **Bluetooth fully disabled at boot** (`btStop()`) — ~5-10 mA saved
- **WiFi modem sleep** enabled when STA is connected — ~30 mA saved
- **CPU clock** runs at 80 MHz when idle, bumps to 240 MHz when a meeting
  starts, drops back when finished — ~20 mA saved in the idle case
- **LED** only blinks once every 5 s while busy, OFF when idle — ~5 mA saved
  during recording

Estimated runtime on a 400 mAh LiPo:

| Use case                            | Runtime  |
| ----------------------------------- | -------- |
| Idle (waiting for meeting)          | ~8.5 h   |
| Continuous recording                | ~2.5 h   |
| Mixed typical use                   | ~5 h     |

For longer sessions a 1000 mAh LiPo gives roughly 3× these numbers in the
same form factor.

---

## 8. Features

### 8.1 Smart WiFi

- **First boot (no creds):** AP-only mode — connect to the
  `MeetingRecorder` hotspot and open `192.168.4.1/setup`.
- **Configured:** STA-only mode — your phone/laptop stays on home WiFi
  (internet works!) and reaches the dashboard via
  `http://meetingrecorder.local` (mDNS).
- **STA fails:** falls back to AP+STA so you can re-enter credentials.

### 8.2 Map-Reduce Summarisation (unlimited duration)

- Single GPT call for meetings ≤ 25 KB transcript (~30 min).
- Map-reduce for anything longer: stream 25 KB segments off SD, summarise
  each, then synthesise into one polished final summary. RAM usage is
  bounded regardless of meeting length.

### 8.3 Regenerate Summary

Every meeting card in the History tab has a ↺ button. Click it to re-run
the full summary pipeline against the saved transcript. Useful when a
prompt changes, or to recover a meeting whose original final-summary call
failed and fell back to a rolling summary.

### 8.4 Factory Reset

Settings → **Danger Zone → Factory Reset** wipes every meeting on the SD
card, clears WiFi credentials, API keys, and AP settings, then reboots
into AP setup mode. Two-step confirmation prevents accidental triggers.

### 8.5 Ask AI Chat

Once a meeting has a summary, the **Ask AI** tab unlocks. Conversation
memory of 6 turns; questions are answered against the meeting context
(works for both the current meeting and any past meeting from History).

### 8.6 Live Dashboard

- Live transcript (rolling, last ~4 KB shown in UI)
- Live rolling summary (markdown-rendered)
- Meeting stats: chunks, words, duration, network info
- Mobile-responsive with transparent status bar
- All polling pauses when the tab is hidden — saves device RAM and
  phone battery

---

## 9. Troubleshooting

| Symptom                                | Likely cause / fix                              |
| -------------------------------------- | ---------------------------------------------- |
| `SD init FAILED` on boot               | Card not FAT32, not inserted, or CS pin wrong  |
| `Mic init FAILED`                      | Sense expansion board not seated               |
| `WiFi TIMEOUT — enabling AP for recovery` | Wrong credentials → open `/setup` over AP    |
| Can't reach `meetingrecorder.local`    | Old Android version — use the LAN IP from Serial Monitor instead |
| Transcript empty after stopping        | Check ElevenLabs key + remaining quota         |
| Final summary looks like rolling format | The final GPT call failed and fell back. Click ↺ on the meeting in History to regenerate |
| Summary cuts off in History tab        | Updated firmware reads up to 8 KB per summary — was 1.2 KB in older versions |
| Empty meetings cluttering History      | Meetings with no usable summary are auto-hidden — `summary_final.txt < 50 chars` filters them |
| `[Insert Name]` style placeholders     | Re-record (old recordings have old prompt output) or click ↺ to regenerate |
| Device dies after ~3 hours on battery  | 400 mAh isn't a lot — try a 1000 mAh cell      |

---

## 10. License

The bundled JSON parser (`ShubhJson`) is derived from the MIT-licensed
ArduinoJson by Benoit BLANCHON. The original copyright and license are
preserved verbatim in [ShubhJson_LICENSE.txt](ShubhJson_LICENSE.txt) as
required by the MIT terms.

The rest of the project is open source under the same terms.
