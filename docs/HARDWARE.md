# Hardware guide

Board: **ESP32 DevKit (38-pin, ESP-WROOM-32)** for the main controller, plus a separate **ESP32-CAM (AI-Thinker)** for the camera.
The two boards are *not wired together*. Both join the same Wi-Fi; the Python server talks to both.

```
                    Wi-Fi / LAN
  ┌──────────────────────────────────────────┐
  │                                          │
  ▼                                          ▼
ESP32 DevKit  (rail, pots, pump)         ESP32-CAM  (photos)
  │  ▲                                       │
  │  └──── REST /api/*  ◄────┐               │  /capture  /stream
  ▼                          │               ▼
 motors, sensors, pump    Python server  ◄───┘   ──►  Claude vision (diagnosis)
                              │
                              ▼
                       website (browser)
```

## 1. Complete pin map (main ESP32)

### Already in use (from your wiring)

| Function | Signal | GPIO |
|---|---|---|
| Upper motor (rack carriage) ULN2003 | IN1 / IN2 / IN3 / IN4 | 18 / 19 / 21 / 22 |
| Lower motor (turntable) ULN2003 | IN1 / IN2 / IN3 / IN4 | 13 / 12 / 14 / 27 |
| Soil sensor, pot 1 / 2 / 3 | analog signal | 32 / 33 / 34 |

### New pins (chosen for you)

| Function | Signal | GPIO | Notes |
|---|---|---|---|
| **Ultrasonic HC-SR04** | TRIG | **25** | output |
| **Ultrasonic HC-SR04** | ECHO | **35** | input-only pin. **ECHO is 5 V: use the divider in §2** |
| **IR sensor 1 (Home end)** | OUT | **4** | internal pull-up enabled |
| **IR sensor 2 (End)** | OUT | **23** | internal pull-up enabled |
| **Water pump** relay / MOSFET module | IN | **26** | you need one to switch the pump |

Still free for later: 5, 15, 16, 17, 2, 36 (VP), 39 (VN).

### Why these pins

* They are not flash pins (6-11), not UART0 (1, 3) and not boot-strapping pins (0, 2, 12, 15), so nothing can stop the board from booting.
* GPIO 34-39 are **input-only**; ECHO is only ever an input, so it uses 35 and the output pins stay free for outputs.
* Your soil sensors are on **ADC1** (32, 33, 34). That is correct: the ESP32's ADC2 pins (0, 2, 4, 12-15, 25-27) cannot be read while Wi-Fi is on. The new pins are only used as digital I/O, so they are fine.
* **GPIO 12 (your lower motor IN2) is a strapping pin.** If the board refuses to boot or flash, unplug that one wire, flash, and plug it back in.

## 2. Wiring each new part

### HC-SR04 ultrasonic sensor (mounted on the carriage, looking down)

| HC-SR04 | Connect to |
|---|---|
| VCC | 5 V (the 5 V supply or the ESP32 `VIN`/`5V` pin) |
| GND | GND |
| TRIG | GPIO 25 |
| ECHO | GPIO 35 **through a divider** |

**Power the HC-SR04 from 5 V, not 3.3 V.** On 3.3 V many modules do not work properly and return a constant short pulse (about 53 µs, "1 cm") whatever is in front of them.

ECHO outputs 5 V but ESP32 pins are 3.3 V only. Build a divider:

```
ECHO ──[ 1 kΩ ]──┬── GPIO 35
                 │
              [ 2 kΩ ]      (or two 1 kΩ resistors in series)
                 │
                GND
```

### IR sensors (limit switches)

Typical 3-pin IR obstacle modules (FC-51 style):

| IR module | Connect to |
|---|---|
| VCC | **3.3 V** (so the OUT level is 3.3 V, safe for the ESP32) |
| GND | GND |
| OUT | GPIO 4 (IR 1, Home) / GPIO 23 (IR 2, End) |

* The module pulls OUT **low** when it sees something. This is `IR_ACTIVE_LOW true` in `config.h`.
* Fix a small white / reflective flag to the carriage so it passes in front of each module at the very end of travel. Black 3D-printed parts reflect almost no infrared, so the flag must be white (paper, tape, foil). Make the flag **2-3 cm long in the direction of travel**: a very short flag gives a detection zone only a millimetre wide, and the sensor then flickers at its edge. A micro limit switch between the pin and GND works too, with no code change. Turn the module's trimpot until its LED lights only when the flag is there.
* IR 1 sits at the **Home** end, IR 2 at the **End**. The firmware treats positive steps as Home → End.

### Water pump

Never power a pump from an ESP32 pin. Use a relay (or MOSFET) module:

| Relay module | Connect to |
|---|---|
| VCC / GND | 5 V / GND |
| IN | GPIO 26 |
| COM / NO | in series with the pump's positive supply wire |

* The pump gets its own supply (or the 5 V supply if it is a small 5 V pump under about 500 mA). Add a 470-1000 µF capacitor across that supply to avoid brown-outs.
* **Use COM and NO** (normally open), never NC. On NC the pump runs whenever the relay is off, i.e. always.
* Most relay boards are **active-low** (relay ON when IN is pulled LOW). Three relay modes exist (**Care > Behaviour > Relay type**, or `PUMP_MODE` in `config.h`): **1** active-low, OFF = pin driven HIGH (default); **2** active-low, OFF = pin *released* - only for a board with a real pull-up on IN, or with your own 10 k resistor between IN and VCC (without a pull-up the released line floats and can stay LOW, so the relay never drops out after a pump test); **0** active-high board. If the pump never starts, try another mode; if the relay LED stays on after the pump stopped, use mode 1 (or mode 2 plus the resistor).
* Finding out why a pump "is always on": unplug the pump's wire from the relay and look at the relay board. **Red LED / click while idle** = the board is being told ON (wrong relay type, wrong wire on IN, or the board needs a different jumper: some have a high/low trigger jumper). **LED off but the pump still runs** = the pump is on NC or wired around the relay.
* Use the "Pump test" button on the Care page for the first test, with the pump in a cup of water.
* A DC pump on a MOSFET needs a flyback diode across the pump.

* **The controller restarts (website says "Controller offline") when the pump starts:** one USB port / cable cannot deliver the pump's start-up current, the 5 V sags and the ESP32 resets ("Brownout detector was triggered" in the Serial Monitor). Give the pump its own charger (only COM, NO and the pump are on it), or use a 5 V 2 A wall charger with a short thick cable, a 1000 µF capacitor across the ESP32 5 V / GND, and a 1N4007 diode across the pump (stripe towards +).

### Power overview

| What | From |
|---|---|
| Both ULN2003 boards (VCC +) | the separate 5 V / 2 A supply |
| ULN2003 GND | that supply's GND |
| **That GND ↔ ESP32 GND** | **must be joined (common ground)**: the ESP32 signals to the ULN2003 boards need it |
| Water pump | **its own supply** (phone charger / battery), wired only through the relay contacts COM + NO. A **relay needs no common GND** with the ESP32 (its contacts are isolated), and leaving it out keeps the pump's electrical noise away from the ESP32. A MOSFET/transistor switch instead of a relay *does* need a common GND. |
| ESP32 | USB or its own 5 V |
| Soil sensors | ESP32 3.3 V |
| IR modules | ESP32 3.3 V |
| HC-SR04, relay module | 5 V |

## 3. ESP32-CAM

* Separate board with its own 5 V supply (**at least 1 A**, a weak supply gives random resets and brown-out messages).
* Flashing: use an ESP32-CAM-MB base, or an FTDI adapter with GPIO0 tied to GND while uploading (remove the jumper afterwards).
* Arduino IDE board: **AI Thinker ESP32-CAM**.
* Point the camera at the plant. The bright white LED on GPIO 4 is the flash and is only used when a snapshot asks for it.

## 4. Mechanics that matter for the ultrasonic scan

* The sensor must look **straight down** at the pots; the lids (white discs in your diagram) are a good flat target.
* HC-SR04 range is about 2 cm to 4 m. The pot lid and the empty base must differ by **at least ~3 cm**, otherwise lower "Pot detection" on the Care page.
* Leaves can reflect badly. The firmware joins short gaps, so a few bad readings inside a pot are fine.
* The nozzle is usually a few centimetres away from the sensor. If water lands beside the pot, change **Nozzle offset** (steps, positive = towards End) and press "Go to pot" to check.
* The lower motor turns the plant. It uses the same 28BYJ-48 half-step drive (4096 steps = 360°), so one `360° sweep` is +180°, back, -180°, back. If your gearing differs, change `STEPS_PER_REV_HALF` in `config.h`.
* The first time, use the **Direction test** buttons on the Care page: positive must move the carriage towards the End sensor. If not, set `INVERT_RAIL_DIR true` in `config.h`.

## 5. Safety

* Water and mains-powered supplies do not mix: keep the electronics above the water line and use a drip loop on cables.
* The pump is capped at 30 s per run, at most 6 waterings per pot per day, and never runs unless the ultrasonic sensor confirms a pot is under the nozzle. These are limits in firmware (`config.h`), not guarantees. Keep an eye on the first days.
* The **Stop** button (website) and the **End** IR sensor both halt the carriage immediately.
