# PlantRail: smart plant watering and monitoring

An ESP32 rail carriage that waters three pots by itself, plus an ESP32-CAM that watches the plants and an AI that diagnoses leaf disease. Everything is visible on a website.

* **Soil sensors decide.** Three capacitive sensors measure each pot; a pot below its threshold gets watered.
* **The carriage finds the pots.** A stepper moves a rack carriage between two IR sensors (Home / End). An ultrasonic sensor on the carriage scans the rail once, finds the three pots, and later confirms a pot is really under the nozzle before the pump starts.
* **The camera looks, AI diagnoses.** The ESP32-CAM takes a photo, a vision AI (free Google Gemini, or Claude) identifies the plant, spots disease / pests / nutrient problems and returns treatment steps and care tips.
* **A website shows it all**, styled after the "Scan Plants Fast" poster: live camera, moisture rings, rail diagram, diagnosis, history charts.

```
ESP32 DevKit ─┐                                   ┌─► website (browser)
 (rail, pots, │ REST   ┌───────────────┐  HTTP    │
  pump)       ├───────►│ Python server ├──────────┤
ESP32-CAM ────┘ /capture│  (this repo)  │          └─► Claude vision API
                        └───────────────┘
```

Watering logic runs **on the ESP32**, so plants keep getting watered even when the PC is off. The server only displays, logs history and runs the AI.

## Repository layout

| Path | What |
|---|---|
| `firmware/main_controller/` | ESP32 DevKit sketch: rail, pots, soil, pump, REST API |
| `firmware/esp32_cam/` | ESP32-CAM sketch: snapshot + MJPEG stream |
| `server/` | Python (FastAPI) server: website host, device proxy, AI diagnosis, history DB, simulator |
| `web/` | the website (plain HTML / CSS / JS, no build step) |
| `docs/HARDWARE.md` | **pin map, wiring, power, mechanics** |
| `.env.example` | server settings template |

## 1. Try the website without hardware

```bash
start_server.bat --mock
```

Open <http://localhost:8000>. A simulated rail, soil sensors and camera let you click through everything. AI answers are canned demo results until you add an API key.

(Manual way: `python -m venv .venv`, `.venv\Scripts\pip install -r server\requirements.txt`, `.venv\Scripts\python -m server --mock`.)

## 2. Set up the real thing

### a) Wiring
Follow [docs/HARDWARE.md](docs/HARDWARE.md). The new pins are:

| Part | GPIO |
|---|---|
| HC-SR04 TRIG / ECHO (ECHO through a 1 kΩ / 2 kΩ divider) | 25 / 35 |
| IR 1 (Home) / IR 2 (End) | 4 / 23 |
| Pump relay / MOSFET input | 26 |

### b) Flash the main controller
1. Arduino IDE, install the **esp32** board package (tested with 3.3.7). No extra libraries are needed.
2. In `firmware/main_controller/` copy `secrets.example.h` to `secrets.h` and enter your Wi-Fi name / password and an API key.
3. Check `config.h` (`PUMP_ACTIVE_LOW`, `IR_ACTIVE_LOW`, `INVERT_RAIL_DIR`).
4. Board **ESP32 Dev Module**, upload. The Serial Monitor (115200) prints the IP address.

### c) Flash the camera
Same steps in `firmware/esp32_cam/` (board **AI Thinker ESP32-CAM**). Open `http://plantcam.local/capture` to check it.

### d) Start the server
```bash
start_server.bat
```
The first run creates `.env`. Fill in:

| Variable | Meaning |
|---|---|
| `ESP32_URL`, `CAM_URL` | `http://plantrail.local` / `http://plantcam.local`, or the IPs from the Serial Monitor if `.local` does not resolve |
| `ESP32_API_KEY` | same value as `API_KEY` in `secrets.h` |
| `GEMINI_API_KEY` | **free** key from <https://aistudio.google.com/apikey>: enables real AI diagnosis (see below) |
| `ANTHROPIC_API_KEY` | optional paid alternative (Claude) |
| `AUTO_SCAN_HOURS` | automatic camera scan every N hours (0 = off) |

Open <http://localhost:8000> (from a phone: `http://<PC-IP>:8000`).

### e) Bring-up checklist (first time, in this order)
1. **Care → Direction test.** Press `200 ▶`: the carriage must move towards the End sensor. If not, flip `INVERT_RAIL_DIR` in `config.h` (it is `true` by default in this repo because the author's carriage ran backwards with `false`).
2. **IR sensors.** Move the flag by hand: the *IR sensors* tile on Home must switch to "At Home" / "At End".
3. **Pump test**, with the pump in a cup. It must be off when idle. Wire the pump through **COM and NO**. Relay type is on the Care page (Behaviour); the default (active-low) is right for most blue relay boards. If the pump never starts, switch the type; if the relay LED is on while idle, see *Water pump* in [docs/HARDWARE.md](docs/HARDWARE.md).
4. **Calibrate rail.** The carriage homes, travels to the End sensor while the ultrasonic sensor samples, and detects the three pots. The chart shows the scan; if the pot count is not 3, tune **Pot detection** / **Pots are closer / lower than the base** and run it again. Positions are saved in flash.
   **No working ultrasonic sensor or End sensor yet?** Use **Care → Manual setup** instead: jog the carriage to Home and press *Set Home here*, jog to each pot and press *Save pot N*, then *Finish*. No scan is needed; the "check for the pot with ultrasonic" option is then off until you switch it on.
5. **Soil sensors.** For each pot hold the sensor in air → **Set dry**, in water → **Set wet**.
6. **Nozzle offset.** Press **Go to pot** and **Water now**; if water misses the pot, adjust the offset.
7. Turn **Auto watering** on and set each pot's threshold and pump seconds.

## How a watering job works

1. Every *check interval* (default 30 min) the ESP32 reads the three soil sensors. A pot is queued if it is enabled, below its threshold, its sensor reads sanely, it has not been watered for *wait before re-watering* minutes and has not hit the daily limit.
2. The carriage homes (IR 1 resets the position to zero) and drives to the pot's saved position.
3. The ultrasonic sensor re-measures. If no pot is detected the job **aborts without pumping** and reports an error.
4. The pump runs for the configured seconds, then the carriage returns Home.

Safety limits in firmware: pump max 30 s per run, 6 waterings per pot per day, stop on unexpected limit sensors, homing fails with a message if the carriage goes the wrong way.

## Position guard: the carriage was moved by hand

A 28BYJ-48 has no encoder, so the position is only a step counter. After an **automatic** *Calibrate rail* (IR sensors + ultrasonic scan) the firmware watches for a wrong counter and fixes it by itself:

| What happens | What the controller does |
|---|---|
| Carriage is parked at Home and somebody pushes it off the IR 1 sensor (for 3 s) | goes back Home by itself |
| IR 1 / IR 2 sees the carriage where the counter says it is not | goes Home |
| Parked at a pot / on the empty base and the ultrasonic sensor sees something else than the calibration scan did there (10 readings in a row, not near a pot edge) | goes Home |
| A limit sensor is met too early while driving to a pot, or no pot is under the nozzle | goes Home and tries the same move once more, then reports the error |
| Every watering job | starts with a fresh Home (IR 1 re-zeroes the counter), and after the job it parks Home |
| Power cut / reboot | homes at power-up |

The pot positions are stored as steps from Home, so after the carriage is back Home they are right again. The website shows the *Carriage moved - going Home* / *Position recovered* events. Limits: a push of only a few centimetres inside the same pot or the same empty stretch cannot be seen by the sensors; the next watering job's fresh Home removes that error. After 3 guard re-homes without a job in between (for example a missing flag) the guard pauses and any command on the website re-arms it. It is **off after Manual setup** (hand-taught positions), and the tunables are in `config.h` (`POS_TOL_STEPS`, `GUARD_*`). Not tested on the real rig yet; the decision logic was checked with a simulation.

## Turntable sweep (lower motor)

**Care → Turntable → 360° sweep** (or `POST /api/sweep`) does a full look around without ever winding the cable more than half a turn:

`+180°` → back to centre → `-180°` → back to centre

The centre is wherever the turntable stands when the sweep starts, and it holds for `SWEEP_PAUSE_MS` at every stop. Tune `SWEEP_DEG`, `SWEEP_PAUSE_MS`, `TURN_SPEED` and `INVERT_TURN_DIR` (which way is "+") in `config.h`. The **Stop** button halts it.

## AI diagnosis

* **360° plant scan** (Diagnosis page): the turntable under the middle pot turns +180° and back, then -180° and back, and the ESP32-CAM takes photos from four sides (0°, +90°, +180°, -90°). All four go to the AI in one request, together with the pot's soil-moisture reading and anything you typed or said.
* **Bangla and voice:** describe the problem in Bangla or English (type it, or press the microphone: Chrome / Edge speech recognition, works on `http://localhost:8000`, not on a phone over Wi-Fi). Pick the answer language with the switch (default Bangla, `AI_LANGUAGE=bn|en` in `.env`). **Read aloud** speaks the result with the browser's Bangla voice if the computer has one (Microsoft Edge usually does).
* **Ask a follow-up** under the result ("how do I spray?", "কতদিন পর আবার দেব?"), by typing or speaking.
* The answer is structured JSON: plant, health, urgency, each disease with symptoms / cause / treatment steps, prevention, care tips, watering advice.
* Server calls: `POST /api/scan/sweep`, `POST /api/diagnose` (one photo or upload, with `note` and `lang`), `GET /api/scan/progress`, `POST /api/ask`.
* **Free AI: Google Gemini.** Put a key from Google AI Studio into `.env` as `GEMINI_API_KEY` and restart the server; nothing else to set (`AI_PROVIDER=auto`). The newest "flash" model your key can use is picked automatically (override with `GEMINI_MODEL`). The free tier has request limits (the site says so when they are used up) and Google may use free-tier prompts and photos to improve its products, so do not send private photos. Not tested against the live Gemini service yet: only against a local stand-in, so report any error text you see.
* **Claude** (paid) is used instead when only `ANTHROPIC_API_KEY` is set, or when `AI_PROVIDER=claude` (`CLAUDE_MODEL`, default `claude-opus-5-5`, with server-side fallback enabled).
* Photos you scan or upload are sent to the chosen provider and saved in `data/scans/` on the PC.
* Without any key the site runs in demo mode with canned answers.
* It is decision support for a hobby grower, not a lab test.

## Controller REST API

All on the ESP32 (port 80). `POST` calls need the header `X-Api-Key`.

| Call | Purpose |
|---|---|
| `GET /api/status` | everything the website shows |
| `GET /api/scan`, `GET /api/events?since=N` | last calibration scan, event log |
| `GET /api/ultrasonic` | ultrasonic wiring diagnostic (open in a browser while idle) |
| `POST /api/home`, `/api/calibrate`, `/api/stop` | carriage control |
| `POST /api/goto?pot=N`, `/api/water?pot=N&secs=S`, `/api/check` | move / water / check all pots |
| `POST /api/sweep`, `/api/turntable?deg=D` | turntable (lower motor): 360° sweep / manual rotate |
| `POST /api/pump?secs=S`, `/api/jog?steps=N` | bench tools |
| `POST /api/teach?what=home\|pot\|end\|done&pot=N` | manual set-up: jog, then save positions (switches the position guard off) |
| `POST /api/auto?on=1`, `/api/settings?…`, `/api/soilcal?pot=N&point=dry\|wet` | configuration |

## Troubleshooting

| Symptom | Check |
|---|---|
| "Controller offline" | Serial Monitor for the IP; put it in `ESP32_URL`; PC and ESP32 on the same Wi-Fi |
| Carriage runs the wrong way | `INVERT_RAIL_DIR` |
| "Hit the End sensor while homing" | same as above |
| "Found N pots, expected 3" | open the scan chart; tune Pot detection and polarity |
| Ultrasonic always shows `-` / "No ultrasonic echo" | open `http://<ip>/api/ultrasonic`: `echo_idle` must be 0 and `hits` above 0 with an object 10-20 cm in front (`pulses_us` about 58 per cm); `echo_idle` 1 = ECHO wire / divider wrong, all zero = no power, TRIG not on GPIO25 or ECHO not on GPIO35 |
| "No pot detected under the nozzle" | adjust Nozzle offset; check the ultrasonic wiring and the divider |
| Board will not boot / flash | unplug the lower motor IN2 wire (GPIO 12) while flashing |
| Random resets | stronger 5 V supply, common ground, capacitor across the pump supply |
| Camera image upside down | `set_vflip` in `esp32_cam.ino` |
| Website slow / camera picture lags | the server now keeps the newest camera frame in memory, so the page never waits for the camera. RGB-only sensors (GC2145) use a small 640x480 preview and the big 1024x768 size only for AI photos |
| `JPEG format is not supported on this sensor` (0x106) | the board carries a sensor without JPEG output (GC2145, OV7670, GC0308 ...). The sketch falls back to RGB565 and converts the frames itself (1024x768, `RGB_SIZE`). Serial Monitor prints the sensor id (`0x2145` = GC2145, 2 MP) |

## Status

Both sketches compile for the ESP32 and ESP32-CAM, and the website, server and simulator were tested together in the browser. **Not yet verified on real hardware**: motor direction, IR polarity, pump polarity and the live Claude call (needs your API key). Use the bring-up checklist above.
