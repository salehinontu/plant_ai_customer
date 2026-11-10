#pragma once
// ============================================================================
//  Plant Rail - main controller configuration (ESP32 DevKit, 38-pin)
//  Everything you may need to change lives in this file.
// ============================================================================

#define FW_VERSION "1.0.0"

// ---------------------------------------------------------------------------
//  PIN MAP
// ---------------------------------------------------------------------------
// Upper motor (rack carriage) - ULN2003 driver #1
#define RAIL_IN1 18
#define RAIL_IN2 19
#define RAIL_IN3 21
#define RAIL_IN4 22

// Lower motor (turntable, 360 deg) - ULN2003 driver #2
#define TURN_IN1 13
#define TURN_IN2 12   // GPIO12 is a boot strapping pin: if the board will not boot, unplug IN2 while flashing
#define TURN_IN3 14
#define TURN_IN4 27

// Capacitive soil sensors. ADC1 pins only (ADC2 cannot be read while Wi-Fi is on).
const uint8_t SOIL_PINS[3] = {32, 33, 34};

// ---- New pins (all free on the DevKit, none are strapping / flash / UART pins) ----
#define PIN_US_TRIG 25   // HC-SR04 TRIG  (output)
#define PIN_US_ECHO 35   // HC-SR04 ECHO  (input-only pin) - MUST go through a 5V -> 3.3V divider
#define PIN_IR_HOME 4    // IR sensor 1 at the Home end of the rail
#define PIN_IR_END  23   // IR sensor 2 at the far end of the rail
#define PIN_PUMP    26   // relay / MOSFET module input that switches the water pump

// ---------------------------------------------------------------------------
//  HARDWARE BEHAVIOUR
// ---------------------------------------------------------------------------
// Carriage direction: positive steps must move the carriage from Home towards End.
// If the carriage runs the wrong way during the "Direction test", flip this.
// (true here: on this build the carriage moved the wrong way with false.)
#define INVERT_RAIL_DIR true
#define INVERT_TURN_DIR false

// Typical IR obstacle modules pull OUT low when the carriage flag is in front of them.
#define IR_ACTIVE_LOW true

// Most 1-channel relay boards are active-LOW (IN low = pump ON).
// After flashing, the pump must be OFF while idle. If it is ON, flip this.
#define PUMP_ACTIVE_LOW true

// Home the carriage automatically at power-up (leave false for the first bring-up).
// The first watering job always homes first anyway.
#define AUTO_HOME_ON_BOOT false

// ---------------------------------------------------------------------------
//  MOTION
// ---------------------------------------------------------------------------
#define STEPS_PER_REV_HALF 4096      // 28BYJ-48 in half-step mode (turntable: steps for 360 deg)
#define RAIL_ACCEL         600.0f    // steps / s^2
#define HOME_SPEED         500.0f    // steps / s while seeking the Home sensor
#define HOME_SLOW_SPEED    150.0f    // steps / s for the final, precise approach (the zero is always the same edge)
#define HOME_BACKOFF_MAX   4000L     // max steps allowed to step off a sensor again (flag too small / sensor stuck on)
#define IR_DEBOUNCE_MS     4         // an IR reading must stay the same this long before it counts
#define TURN_SPEED         600.0f
#define TURN_ACCEL         500.0f
// Turntable sweep: +SWEEP_DEG, back to centre, -SWEEP_DEG, back to centre.
// 180 deg each way = a full 360 deg look at the plant, and the cable never twists more than half a turn.
#define SWEEP_DEG          180
#define SWEEP_PAUSE_MS     800       // hold at every stop (time for the camera)
#define MAX_TRAVEL_STEPS   60000L    // give up homing / scanning after this many steps (rail length safety)

// ---------------------------------------------------------------------------
//  ULTRASONIC POT SCAN
// ---------------------------------------------------------------------------
#define US_TIMEOUT_US     8000UL     // ~1.3 m max range
#define SCAN_SAMPLE_STEPS 40         // one distance reading every N steps while scanning
#define MAX_SCAN          2000       // buffer size -> MAX_SCAN * SCAN_SAMPLE_STEPS steps of rail
#define MERGE_GAP         4          // join pot segments separated by up to N bad samples (leaves, echoes)
#define MIN_RUN           3          // a pot must span at least N samples
#define EXPECTED_POTS     3

// ---------------------------------------------------------------------------
//  WATERING SAFETY
// ---------------------------------------------------------------------------
#define PUMP_MAX_S            30     // hard cap for one pump run
#define MAX_WATER_PER_DAY     6      // per pot, protects against a dead soil sensor flooding a pot
#define FIRST_CHECK_DELAY_MS  45000UL

// ---------------------------------------------------------------------------
//  NETWORK
// ---------------------------------------------------------------------------
#define HOSTNAME "plantrail"
