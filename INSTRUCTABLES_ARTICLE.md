# Build Your Own AI Meeting Recorder — Seeed XIAO ESP32-S3 Sense

**Category:** Electronics → Microcontrollers
**Difficulty:** Intermediate
**Time:** 1–2 hours

---

## Introduction

In this project, I will show you how to build an AI-powered meeting recorder using the Seeed XIAO ESP32-S3 Sense. This compact device can record meetings, transcribe speech into text, generate AI summaries, and store everything directly on a microSD card — all without needing a computer.

The system also hosts a real-time web dashboard over WiFi, allowing you to monitor recordings, view transcripts, and manage meeting history directly from your browser.

Simply press a button, talk freely, and walk away with a full transcript, an AI-generated multi-section summary, and the ability to chat with the meeting afterward. Everything runs on a coin-sized microcontroller smaller than a thumb drive. There is no app to install, no cloud subscription, no phone required during the meeting.

---

## Features

✅ **Continuous audio recording** using the onboard PDM microphone

✅ **Automatic speech-to-text transcription** after every 15-second audio chunk via ElevenLabs Scribe

✅ **Live AI-generated meeting summaries** during recording with GPT-4o-mini

✅ **Automatic saving** of recordings, transcripts, and summaries to microSD card

✅ **Real-time responsive web dashboard** accessible through WiFi at `http://meetingrecorder.local`

✅ **Meeting history panel** for browsing and managing previous recordings

✅ **One-click copy option** for summaries and transcripts

✅ **Physical push button** for starting and stopping recordings

✅ **Map-reduce summarization pipeline** — handles meetings of unlimited duration (4-hour conferences summarized as accurately as 5-minute tests)

✅ **Ask AI chat** — Ask questions about any meeting and get instant answers with conversation memory

✅ **Regenerate summaries** — Re-run the AI summary pipeline on past meetings if prompts improve or API calls fail

✅ **Battery-friendly operation** — ~50 mA power savings via optimized WiFi/BT settings, runs ~5 hours on a 400 mAh LiPo

✅ **Smart WiFi setup** — First boot creates a hotspot; thereafter accessible on home WiFi via mDNS

✅ **Factory reset** — Two-step confirmation to wipe all meetings and start fresh

✅ **Local data ownership** — All meetings stored on your microSD card; yours to keep, no cloud subscription required

---

## Supplies

### Hardware

| Item | Notes |
|------|-------|
| **Seeed XIAO ESP32-S3 Sense** | Must be the *Sense* variant — includes the expansion board with PDM microphone and microSD slot |
| **microSD card** | Any size; format as FAT32 before use |
| **Tactile push-button** | Standard through-hole or panel-mount button |
| **2× short wires** | To wire the button to the board (22 AWG or similar) |
| **USB-C cable** | For flashing and power |
| **Li-Po battery (optional)** | 400 mAh for ~5 hours mixed use; 1000 mAh for all-day portability. Solder to BAT+/BAT– pads |

**Total hardware cost:** roughly $15–25 depending on sourcing.

### Software & Accounts

- **Arduino IDE 2.x** (free download from arduino.cc)
- **ESP32 board package** by Espressif (installed inside Arduino IDE — free)
- **ElevenLabs account** — free tier includes generous quota for testing; get your API key from their dashboard
- **OpenAI account** — pay-as-you-go; GPT-4o-mini is extremely cheap (~$0.002 per summary)
- **This project's source code** — available at GitHub (Shubhjaiswal408/MeetingRecorder)

---

## Step 1: Wire the Button

The XIAO ESP32-S3 Sense's expansion board already includes the PDM microphone and microSD socket. The only external wiring is the record button.

1. Solder or clip one leg of the push-button to **pin D1** (GPIO 1) on the XIAO.
2. Solder or clip the other leg to any **GND** pin.

That is the entire circuit. The firmware uses the internal pull-up resistor, so no additional resistors are needed.

**Optional battery wiring:** For a truly portable device, solder a single-cell Li-Po to the **BAT+** and **BAT–** pads on the underside of the XIAO. A protective circuit is built in, so standard LiPos are safe.

---

## Step 2: Prepare the microSD Card

1. Insert the microSD card into your computer.
2. Format it as **FAT32**. 
   - **Windows:** Right-click the drive → Format → FAT32
   - **Mac:** Disk Utility → Erase → MS-DOS (FAT)
   - **Linux:** `mkfs.vfat /dev/sdX` (replace X with your device)
3. No files needed beforehand — the firmware creates its own folder structure on first run.
4. Insert the card into the slot on the bottom of the XIAO Sense expansion board until it clicks.

> **Note:** The slot is spring-loaded push-to-insert / push-to-eject. Ensure the card is fully seated.

---

## Step 3: Install Arduino IDE and the ESP32 Board Package

1. Download and install **Arduino IDE 2.x** from arduino.cc.
2. Open Arduino IDE and go to **File → Preferences** (or Arduino → Settings on Mac).
3. In the *Additional boards manager URLs* field, paste:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Open **Tools → Board → Boards Manager**, search for **esp32** by Espressif, and click **Install**. This takes a few minutes.
5. Once installed, go to **Tools → Board → esp32 → XIAO_ESP32S3** and select it.

**No other library installs are needed.** The firmware bundles its own JSON parser (`Shubh'Json`) and uses only libraries that ship with the ESP32 core (`WiFi`, `WebServer`, `HTTPClient`, `SD`, `FS`, `ESPmDNS`).

---

## Step 4: Download and Open the Firmware

1. Download the project repository from GitHub: **Shubhjaiswal408/MeetingRecorder** (or clone via Git).
2. Extract the ZIP into a folder named `MeetingRecorder`.
3. Open **`MeetingRecorder.ino`** in Arduino IDE.

You will see several tabs — this is normal. Arduino automatically compiles all `.cpp` source files inside the `src/` folder. You do not need to edit any of them to get started.

---

## Step 5: Upload the Firmware

1. Plug the XIAO into your computer via USB-C.
2. Verify **Tools → Port** shows the correct COM port (Windows) or `/dev/cu.usbserial-...` (Mac/Linux).
   - If no port appears, install the CH340/CP2102 USB driver for your OS.
3. Click the **Upload** button (right-pointing arrow icon).
4. The IDE will compile (~1 minute on first run) and then flash the firmware.
5. Once it says *Done uploading*, the device reboots automatically.

> **Troubleshooting:** If you see *Failed to connect*, hold the **BOOT** button on the XIAO while clicking Upload, then release it after the IDE starts sending data.

---

## Step 6: Configure WiFi and API Keys

On its first boot (or after a factory reset), the device creates its own WiFi hotspot for configuration.

1. On your phone or laptop, open WiFi settings and connect to **`MeetingRecorder`** (password: `recorder123`).
2. Open a browser and navigate to **`http://192.168.4.1/setup`**.
3. Fill in the configuration form:
   - **WiFi SSID** — your home/office network name
   - **WiFi Password** — your network password
   - **ElevenLabs API Key** — from your ElevenLabs dashboard (for speech-to-text)
   - **OpenAI API Key** — from platform.openai.com (for summaries via GPT-4o-mini)
4. Click **Save**.

The device disconnects from the hotspot, joins your home WiFi, and shuts down the setup hotspot. Your phone reconnects to home WiFi automatically.

From this point on, access the dashboard at **`http://meetingrecorder.local`** from any device on the same network. Credentials are saved to `/config.json` on the microSD card and survive reboots.

> **Android tip:** Older Android versions do not support mDNS (`.local` addresses). If `meetingrecorder.local` doesn't load, open Arduino IDE's **Tools → Serial Monitor** (115200 baud) while the device boots — it prints the local IP address, which you can use directly.

---

## Step 7: Record Your First Meeting

Open **`http://meetingrecorder.local`** in any browser to see the dark-themed dashboard.

**To start recording:**
- Press the physical button on D1, OR
- Click **Start Meeting** in the dashboard

**While recording:**
- The LED on the XIAO blinks once every 5 seconds (a slow heartbeat confirming operation).
- Audio is captured in 15-second chunks at 16 kHz and saved to the microSD card.
- Each chunk is automatically sent to ElevenLabs for transcription.
- The transcript is appended to the SD card in real time.
- The dashboard displays the **live transcript** and updates the **rolling AI summary** chunk by chunk.
- All data is saved locally on your microSD card as it happens.

**To stop recording:**
- Press the button again, OR
- Click **Stop Meeting** in the dashboard

After stopping, the firmware generates the final comprehensive summary (5 sections: Overview, Key Discussion Points, Decisions Made, Action Items, Names/Dates/Numbers). This takes 15–30 seconds. The summary appears in the dashboard and is saved to the SD card.

---

## Step 8: Use Meeting History and Chat

### Meeting History
Click the **History** tab to view all past meetings. Each card displays:
- Date and time of recording
- Duration and word count
- Full transcript and summary

For each meeting you can:
- **Read** the full transcript and summary directly in the UI
- **Download** the original WAV audio file
- **Regenerate** the summary (↺ button) — useful if the original API call failed or if you want to re-run with updated prompts
- **Delete** the meeting if no longer needed

### Ask AI Chat
Once a meeting has a summary, the **Ask AI** tab unlocks. Key features:
- Type any question about the meeting and the AI answers from context
- Conversation memory of 6 turns for natural follow-up questions
- Works for both the current meeting and any past meeting from History
- Example questions: "What was decided about the budget?", "List the action items"

---

## Step 9: (Optional) Portable Battery Operation

To make the device truly standalone and portable:

1. **Add a battery:** Solder a single-cell Li-Po to the **BAT+** and **BAT–** pads on the underside of the XIAO. A flat 400 mAh cell fits nicely; 1000 mAh gives roughly 3× the runtime.

2. **Create an enclosure:** 3D print or use a small project box (50 × 40 × 20 mm works well). The XIAO is 21 × 17.5 mm.

3. **Add a power switch:** Wire an SPDT switch in series with the battery positive lead to cut power completely when not in use.

**Battery life estimates** (with power optimizations built into firmware):

| Use case | 400 mAh | 1000 mAh |
|----------|---------|----------|
| Idle (waiting for meeting) | ~8.5 hours | ~21 hours |
| Continuous recording | ~2.5 hours | ~6 hours |
| Typical mixed use | ~5 hours | ~12 hours |

---

## How It Works

For those interested in the technical details:

### Dual-Core Architecture
The ESP32-S3 has two processor cores:
- **Core 0** runs `recordTask` — reads PDM audio from the microphone, assembles 15-second WAV files, and saves them to the SD card.
- **Core 1** runs `processTask` — processes completed WAV files, sends them to ElevenLabs for transcription, appends the transcript, and triggers the rolling summary via OpenAI.
- The main loop handles WiFi connectivity and the web server.

### Map-Reduce Summarization
With only 320 KB of RAM, the device cannot hold a long meeting's transcript in memory. For transcripts over 25 KB (~30 minutes), the firmware:
1. Streams the file off SD in 25 KB segments
2. Summarizes each segment separately (the "map" step)
3. Makes one final OpenAI call to synthesize all segment summaries into a polished unified report (the "reduce" step)

Only one segment is in RAM at a time, enabling unlimited-duration meetings.

### Web Dashboard
A built-in web server on port 80 serves the entire dashboard as a single HTML/CSS/JS page. All state is exposed via a `/api/status` JSON endpoint polled every 2.5 seconds. Polling pauses when the browser tab is hidden to save battery.

### SD Card Storage Layout
```
/config.json                          ← WiFi + API keys
/meeting_YYYYMMDD_HHMMSS/
    chunk_001.wav  chunk_002.wav ...  ← raw audio chunks (15s each)
    transcript.txt                    ← full rolling transcript
    summary_rolling.txt               ← last rolling summary
    summary_final.txt                 ← polished 5-section report
```

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `SD init FAILED` on Serial Monitor | Card not inserted, not FAT32, or not fully clicked in |
| `Mic init FAILED` | Sense expansion board not properly seated on XIAO |
| Can't reach `meetingrecorder.local` | Old Android — use the IP address from Serial Monitor instead |
| Transcript is empty after stopping | Check your ElevenLabs API key and verify remaining quota |
| Final summary looks like rolling format | The final GPT call timed out. Click ↺ in History to regenerate |
| Device won't flash | Hold BOOT button during upload; release after connection starts |
| AP hotspot doesn't appear | Insert the microSD card — the device needs it to function |
| WiFi keeps dropping | Move closer to your router or check password in `/config.json` via Serial Monitor |
| Battery drains very fast | Ensure no active meeting is running; check that CPU throttling is working (80 MHz idle) |

---

## Tips for Best Results

- **Microphone placement:** Position the device within 1–2 meters of the main speakers. The PDM microphone is omnidirectional but works best at close range.
- **Avoid vibrations:** Don't place the device on vibrating surfaces (tables with HVAC rumble, etc.).
- **Room acoustics:** Rooms with soft furnishings (curtains, rugs) produce better transcripts than hard echoing spaces.
- **Speaker timing:** Overlapping speech is handled reasonably well, but accuracy improves when speakers take turns.
- **Check API quotas:** Monitor your ElevenLabs and OpenAI usage regularly to avoid surprise overage charges.

---

## Going Further

The firmware is modular and well-commented. Natural next steps include:

- **Custom summary prompts:** Prompts are plain strings in `src/api/api.cpp` — swap them for meeting minutes format, JIRA ticket format, or any custom structure.
- **Speaker labels:** ElevenLabs Scribe returns speaker diarization metadata; adding speaker labels to transcripts is straightforward.
- **OTA updates:** Use Arduino OTA to push new firmware over WiFi (plenty of flash headroom available).
- **Multi-language support:** Change the ElevenLabs `language_code` parameter to transcribe in any supported language.
- **Local LLM:** Route OpenAI calls through a local Ollama endpoint to eliminate cloud dependencies (except ElevenLabs transcription).
- **Custom integrations:** The modular design makes it easy to add hooks to Slack, email summaries, or post to a database.

---

## Materials Summary

| Component | Cost | Where to buy |
|-----------|------|--------------|
| Seeed XIAO ESP32-S3 Sense | $8–12 | Seeed Studio, Digi-Key, Digikey |
| microSD card (any size) | $3–5 | Any electronics store |
| Push-button | $0.50 | Adafruit, SparkFun, or local electronics shop |
| Li-Po battery (optional, 1000 mAh) | $5–8 | Adafruit, SparkFun, HobbyKing |
| USB-C cable | $2–3 | Already have one? |
| **Subtotal (without battery)** | **~$13–20** | |
| **Subtotal (with battery)** | **~$20–28** | |

API costs:
- **ElevenLabs:** Free tier typically covers testing; production usage ~$0.01–0.02 per hour of audio
- **OpenAI GPT-4o-mini:** ~$0.002 per summary (regardless of meeting length due to map-reduce efficiency)

---

## Credits & References

- **Project, firmware, and hardware integration** by Shubh Jaiswal (GitHub: Shubhjaiswal408)
- **YouTube tutorials and showcase** by techiesms
- **Bundled JSON parser** derived from ArduinoJson by Benoit Blanchon (MIT license)
- **Speech-to-text** via ElevenLabs Scribe
- **Summarization and chat** via OpenAI GPT-4o-mini

**Project repository:** [github.com/Shubhjaiswal408/MeetingRecorder](https://github.com/Shubhjaiswal408/MeetingRecorder)

---

**You now have a working AI meeting recorder!** Press the button, speak freely, and let the device handle transcription and summarization. All your data stays on the microSD card—no subscription required.
