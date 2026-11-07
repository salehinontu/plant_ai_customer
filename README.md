# PlantRail: smart plant watering and monitoring

An ESP32 rail carriage that waters three pots by itself, plus an ESP32-CAM that watches the plants and an AI that diagnoses leaf disease. Everything is visible on a website.

* **Soil sensors decide.** Three capacitive sensors measure each pot; a pot below its threshold gets watered.
* **The carriage finds the pots.** A stepper moves a rack carriage between two IR sensors (Home / End). An ultrasonic sensor on the carriage scans the rail once, finds the three pots, and later confirms a pot is really under the nozzle before the pump starts.
* **The camera looks, AI diagnoses.** The ESP32-CAM takes a photo, Claude identifies the plant, spots disease / pests / nutrient problems and returns treatment steps and care tips.
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
| `ANTHROPIC_API_KEY` | enables real AI diagnosis (see below) |
| `AUTO_SCAN_HOURS` | automatic camera scan every N hours (0 = off) |

Open <http://localhost:8000> (from a phone: `http://<PC-IP>:8000`).

### e) Bring-up checklist (first time, in this order)
1. **Care → Direction test.** Press `200 ▶`: the carriage must move towards the End sensor. If not, flip `INVERT_RAIL_DIR` in `config.h` (it is `true` by default in this repo because the author's carriage ran backwards with `false`).
2. **IR sensors.** Move the flag by hand: the *IR sensors* tile on Home must switch to "At Home" / "At End".
3. **Pump test**, with the pump in a cup. It must be off when idle; if it runs constantly, flip `PUMP_ACTIVE_LOW`.
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

## Turntable sweep (lower motor)

**Care → Turntable → 360° sweep** (or `POST /api/sweep`) does a full look around without ever winding the cable more than half a turn:

`+180°` → back to centre → `-180°` → back to centre

The centre is wherever the turntable stands when the sweep starts, and it holds for `SWEEP_PAUSE_MS` at every stop. Tune `SWEEP_DEG`, `SWEEP_PAUSE_MS`, `TURN_SPEED` and `INVERT_TURN_DIR` (which way is "+") in `config.h`. The **Stop** button halts it.

## AI diagnosis

* The server sends the photo (and the pot's soil-moisture reading) to **Claude** (`claude-opus-5-5` by default, change with `CLAUDE_MODEL`) and gets structured JSON back: plant, health, issues with confidence and treatment, care tips, watering advice.
* Photos you scan or upload are sent to the Anthropic API and saved in `data/scans/` on the PC. Each scan is one paid API call.
* Server-side fallback is enabled so a harmless plant photo that trips a safety classifier is re-run on a fallback model instead of failing.
* Without `ANTHROPIC_API_KEY` the site runs in demo mode with canned answers.
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
| `POST /api/teach?what=home\|pot\|end\|done&pot=N` | manual set-up: jog, then save positions |
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

## Status

Both sketches compile for the ESP32 and ESP32-CAM, and the website, server and simulator were tested together in the browser. **Not yet verified on real hardware**: motor direction, IR polarity, pump polarity and the live Claude call (needs your API key). Use the bring-up checklist above.
