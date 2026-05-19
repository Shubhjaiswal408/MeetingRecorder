# MeetingRecorder

> **Pocket-sized AI meeting recorder you can build yourself.**
> Live transcription, automatic summaries, and chat with your meetings — all on a XIAO ESP32-S3.

A standalone meeting recorder built on the **Seeed XIAO ESP32-S3 Sense**. Press one button, talk freely, and walk away with an accurate transcript, an AI-generated summary, and the ability to chat with the meeting after it ends. No phone required. No subscription required. Just hardware, WiFi, and a microSD card.

> Built and curated by **[Shubh](https://github.com/Shubhjaiswal408)** · Showcased on **[techiesms](https://www.youtube.com/@techiesms)**

---

## ✨ Features at a glance

### 🎙️ AI Transcription, Live
Every word from every speaker is transcribed in real time by **ElevenLabs Scribe** — handles multiple voices, accents, and technical terms. Accurate text appears chunk by chunk while the meeting is still in progress.

### 🧠 Automatic Meeting Summaries
**GPT-4o-mini** generates a rolling summary during the meeting and a polished final report the moment you press stop. Key decisions, action items, and discussion points ready before you even leave the room.

### 💬 Chat With Your Meetings
Ask anything about a current or past meeting and get instant grounded answers, with conversation memory for natural follow-ups. Find decisions, list action items, or summarise in three bullets — without scrolling a single transcript.

### 🔌 Plug-and-Record. No Computer Needed.
Standalone hardware with a one-touch record button and a built-in WiFi hotspot for first-time setup. No phone, no laptop, no monthly subscription — just power it up and start your meeting.

### 📱 Beautiful Dashboard on Any Device
Modern dark-themed web interface that works on phones, tablets, and laptops — no app to install. Live transcript, live summary, meeting history, and AI chat, all behind one mobile-friendly URL.

### 💾 Your Data, Your Card
Every meeting saved locally to a microSD card — full WAV audio, transcripts, and summaries — yours to keep. Download from the dashboard or pop the card into any computer; no cloud storage subscription required.

---

## 🧰 Hardware

| Component | Notes |
|---|---|
| **Seeed XIAO ESP32-S3 Sense** | Built-in PDM microphone on the Sense expansion board |
| **microSD card (FAT32)** | Any size; meetings auto-archive here |
| **Push-button** | Wired between D1 (GPIO 1) and GND for one-touch record |
| **USB-C power** | Or a Li-Po battery for portable use |

Pin assignments live in [`src/core/globals.h`](src/core/globals.h).

---

## 🚀 Quick start

1. **Install** Arduino IDE 2.x and the [ESP32 board package](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html) (board: **XIAO_ESP32S3**).
2. **Clone** this repo and open `MeetingRecorder.ino` in the IDE.
3. **Upload.** No Library Manager step — the JSON parser is bundled as `Shubh'Json` inside the sketch.
4. **First boot:** the device creates a WiFi hotspot called `MeetingRecorder` (password `recorder123`). Connect from your phone, open <http://192.168.4.1/setup>, and enter:
   - Your home WiFi SSID + password
   - An **ElevenLabs** API key (for speech-to-text)
   - An **OpenAI** API key (for summaries)
5. **Press the button or hit Start in the dashboard.** That's it.

Full setup, troubleshooting, and architecture notes in [`docs/README.md`](docs/README.md).

---

## 📚 Project documentation

- **[docs/README.md](docs/README.md)** — full setup, build & flash, API reference, troubleshooting
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — runtime architecture, FreeRTOS task layout, SD storage map
- **[docs/FILES.md](docs/FILES.md)** — file-by-file source reference
- **[docs/MeetingRecorder_Features.pdf](docs/MeetingRecorder_Features.pdf)** — printable feature sheet

---

## 🧪 Bundled libraries

This sketch is **zero-install** — no Library Manager step required.

- **Shubh'Json** (in [`ShubhJson/`](ShubhJson/) + [`src/json/`](src/json/)) — bundled JSON parser, branded for this project. Derived from the MIT-licensed ArduinoJson by Benoit BLANCHON; original LICENSE preserved in [`docs/ShubhJson_LICENSE.txt`](docs/ShubhJson_LICENSE.txt) as required.

---

## 📐 Footprint

| | Used | Available |
|---|---|---|
| **Flash** | 1.23 MB | 3.34 MB (36%) |
| **RAM** | 40 KB | 320 KB (12%) |

Plenty of headroom for OTA updates, speaker diarisation, multi-language transcripts, and more.

---

## 🤝 Credits

- Project, hardware integration, and Shubh'Json bundling by **[Shubh Jaiswal](https://github.com/Shubhjaiswal408)**
- YouTube showcase and tutorials on **[techiesms](https://www.youtube.com/@techiesms)**
- JSON parser internals: [ArduinoJson](https://arduinojson.org) by Benoit BLANCHON (MIT)
- Speech-to-text: [ElevenLabs Scribe](https://elevenlabs.io)
- Summarisation & chat: [OpenAI GPT-4o-mini](https://platform.openai.com)
