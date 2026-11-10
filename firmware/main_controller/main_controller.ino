// ============================================================================
//  Plant Rail - main controller
//
//  * Rack carriage (stepper) runs along a rail between two IR sensors (Home / End).
//  * An ultrasonic sensor on the carriage finds the 3 pots during a calibration scan.
//  * 3 capacitive soil sensors decide when a pot is dry; the carriage then drives to
//    that pot, re-checks with the ultrasonic sensor that a pot is really below the
//    nozzle, runs the pump for N seconds and returns Home.
//  * A small JSON REST API (port 80) is used by the Python server / website.
//
//  Libraries: none besides the ESP32 Arduino core (WiFi, WebServer, ESPmDNS, Preferences).
//  Board:     "ESP32 Dev Module"
// ============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <algorithm>

#include "config.h"
#include "secrets.h"
#include "half_stepper.h"

// ----------------------------------------------------------------------------
//  Types
// ----------------------------------------------------------------------------
struct Settings {
  uint32_t magic = 0x504C4E54;  // "PLNT"
  bool autoMode = true;
  bool park = true;             // return Home after a job
  bool potCloser = true;        // pots are CLOSER to the sensor than the base plate
  bool calibrated = false;
  uint16_t checkMin = 30;       // how often the soil is checked (minutes)
  uint16_t soakMin = 15;        // wait before the same pot may be watered again
  uint16_t railSpeed = 700;     // steps/s while travelling
  uint16_t scanSpeed = 400;     // steps/s while scanning
  int16_t nozzleOffset = 0;     // steps between the ultrasonic sensor and the water nozzle
  float deltaCm = 3.0f;         // distance change that means "pot below me"
  float baselineCm = 0;
  int32_t railSteps = 0;
  int32_t potPos[3] = {0, 0, 0};
  uint8_t dryPct[3] = {35, 35, 35};      // water when moisture <= this
  uint8_t waterSecs[3] = {5, 5, 5};
  bool potEnabled[3] = {true, true, true};
  int16_t soilDry[3] = {2900, 2900, 2900};  // ADC value in air
  int16_t soilWet[3] = {1300, 1300, 1300};  // ADC value in water
  bool verifyPot = true;        // check with the ultrasonic sensor that a pot is below the nozzle before pumping
  float potCm[3] = {0, 0, 0};   // distance from the sensor to each pot lid, measured while scanning
};

enum State : uint8_t { ST_IDLE, ST_HOMING, ST_CALIBRATING, ST_MOVING, ST_WATERING, ST_ERROR };
enum CmdType : uint8_t { CMD_HOME, CMD_CALIBRATE, CMD_GOTO, CMD_PUMP, CMD_JOG };

struct Cmd {
  CmdType type;
  uint8_t pot;      // 0..2
  uint16_t secs;
  bool force;       // skip the "is a pot below me" check
  int32_t arg;      // jog steps
};

struct Event {
  uint32_t id;
  uint32_t t;       // uptime seconds
  char type[12];
  uint8_t pot;      // 1..3, 0 = none
  uint16_t secs;
  int16_t moisture;
  char msg[64];
};

// ----------------------------------------------------------------------------
//  Globals
// ----------------------------------------------------------------------------
#define QUEUE_MAX 12
#define EVENT_MAX 24

WebServer server(80);
Preferences prefs;
Settings cfg;

HalfStepper rail(RAIL_IN1, RAIL_IN2, RAIL_IN3, RAIL_IN4, INVERT_RAIL_DIR);
HalfStepper turn(TURN_IN1, TURN_IN2, TURN_IN3, TURN_IN4, INVERT_TURN_DIR);

State state = ST_IDLE;
String statusMsg = "Booting";
String errorMsg = "";
bool homed = false;
bool parkPending = false;
uint8_t activePot = 0;  // 1..3 while a pot is being served

Cmd queue[QUEUE_MAX];
uint8_t qCount = 0;
Cmd cur;
bool curActive = false;
uint8_t phase = 0;
uint32_t phaseT0 = 0;
long cmdStartPos = 0;
bool endArmed = false;
bool potPending[3] = {false, false, false};

// soil
float soilEma[3] = {0, 0, 0};
bool soilInit = false;
uint32_t lastSoilMs = 0;
uint32_t lastWaterMs[3] = {0, 0, 0};
bool everWatered[3] = {false, false, false};
uint8_t waterCountToday[3] = {0, 0, 0};
uint32_t dayStartMs = 0;

// pump
bool pumpOn = false;
uint32_t pumpOffAt = 0;

// ultrasonic
float distNow = -1;
uint32_t distStamp = 0;

// scan data (kept in RAM for the website chart)
uint16_t scanMm[MAX_SCAN];
uint16_t scanN = 0;
uint16_t scanValid = 0;       // readings with a real echo during the current scan
static uint16_t sortBuf[MAX_SCAN];
static uint8_t flagBuf[MAX_SCAN];
long nextSamplePos = 0;
float scanBaselineMm = 0, scanThrMm = 0;
int scanPotS[EXPECTED_POTS], scanPotE[EXPECTED_POTS];
uint8_t scanPotsFound = 0;

// scheduler
uint32_t nextCheckMs = 0;
uint32_t lastWifiTry = 0;

// turntable
bool turnActive = false;
bool sweeping = false;        // +SWEEP_DEG, centre, -SWEEP_DEG, centre
bool sweepMoving = false;
uint8_t sweepStep = 0;
long sweepCenter = 0;
uint32_t sweepPauseUntil = 0;

// events
Event events[EVENT_MAX];
uint32_t evNext = 1;

// ----------------------------------------------------------------------------
//  Small helpers
// ----------------------------------------------------------------------------
static inline bool irHome() { return digitalRead(PIN_IR_HOME) == (IR_ACTIVE_LOW ? LOW : HIGH); }
static inline bool irEnd()  { return digitalRead(PIN_IR_END)  == (IR_ACTIVE_LOW ? LOW : HIGH); }

void saveSettings() { prefs.putBytes("cfg", &cfg, sizeof(cfg)); }

void loadSettings() {
  prefs.begin("plantrail", false);
  if (prefs.getBytesLength("cfg") == sizeof(Settings)) {
    Settings tmp;
    prefs.getBytes("cfg", &tmp, sizeof(tmp));
    if (tmp.magic == cfg.magic) cfg = tmp;
  }
}

void addEvent(const char* type, uint8_t pot, uint16_t secs, int16_t moisture, const String& msg) {
  Event& e = events[(evNext - 1) % EVENT_MAX];
  e.id = evNext++;
  e.t = millis() / 1000;
  strlcpy(e.type, type, sizeof(e.type));
  e.pot = pot;
  e.secs = secs;
  e.moisture = moisture;
  strlcpy(e.msg, msg.c_str(), sizeof(e.msg));
}

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

// --- tiny JSON builder (every kv* call leaves a trailing comma, close*() removes it) ---
void trimComma(String& j) { if (j.length() && j[j.length() - 1] == ',') j.remove(j.length() - 1); }
void kvi(String& j, const char* k, long v) { j += '"'; j += k; j += "\":"; j += String(v); j += ','; }
void kvf(String& j, const char* k, float v, uint8_t d = 1) { j += '"'; j += k; j += "\":"; j += String(v, (unsigned int)d); j += ','; }
void kvb(String& j, const char* k, bool v) { j += '"'; j += k; j += "\":"; j += (v ? "true" : "false"); j += ','; }
void kvs(String& j, const char* k, const String& v) { j += '"'; j += k; j += "\":\""; j += jsonEscape(v); j += "\","; }

// ----------------------------------------------------------------------------
//  Pump
// ----------------------------------------------------------------------------
void pumpSet(bool on) {
  pumpOn = on;
  digitalWrite(PIN_PUMP, (on != PUMP_ACTIVE_LOW) ? HIGH : LOW);
}

void pumpFor(uint32_t ms) {
  if (ms > (uint32_t)PUMP_MAX_S * 1000UL) ms = (uint32_t)PUMP_MAX_S * 1000UL;
  pumpOffAt = millis() + ms;
  pumpSet(true);
}

void pumpService() {
  if (pumpOn && (int32_t)(millis() - pumpOffAt) >= 0) pumpSet(false);
}

// ----------------------------------------------------------------------------
//  Soil
// ----------------------------------------------------------------------------
int readSoilRawOnce(uint8_t i) {
  uint32_t sum = 0;
  for (uint8_t k = 0; k < 16; k++) { sum += analogRead(SOIL_PINS[i]); delayMicroseconds(150); }
  return (int)(sum / 16);
}

void soilService() {
  if (millis() - lastSoilMs < 1000) return;
  lastSoilMs = millis();
  for (uint8_t i = 0; i < 3; i++) {
    int raw = readSoilRawOnce(i);
    if (!soilInit) soilEma[i] = raw;
    else soilEma[i] += 0.25f * (raw - soilEma[i]);
  }
  soilInit = true;
  if (millis() - dayStartMs >= 86400000UL) {  // new 24 h window for the daily watering cap
    dayStartMs = millis();
    for (uint8_t i = 0; i < 3; i++) waterCountToday[i] = 0;
  }
}

bool soilOk(uint8_t i) { return soilEma[i] >= 150 && soilEma[i] <= 3950; }

int soilPct(uint8_t i) {
  float span = (float)cfg.soilDry[i] - (float)cfg.soilWet[i];
  if (span < 100) return 0;
  float p = ((float)cfg.soilDry[i] - soilEma[i]) * 100.0f / span;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return (int)(p + 0.5f);
}

// ----------------------------------------------------------------------------
//  Ultrasonic
// ----------------------------------------------------------------------------
float pingCm() {
  digitalWrite(PIN_US_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_US_TRIG, LOW);
  unsigned long us = pulseIn(PIN_US_ECHO, HIGH, US_TIMEOUT_US);
  if (us == 0) return -1.0f;
  float cm = us * 0.01715f;  // speed of sound / 2
  if (cm < 2.0f || cm > 400.0f) return -1.0f;
  return cm;
}

float pingMedianCm(uint8_t n) {
  float v[9];
  uint8_t k = 0;
  if (n > 9) n = 9;
  for (uint8_t i = 0; i < n; i++) {
    float d = pingCm();
    if (d > 0) v[k++] = d;
    delay(35);
  }
  if (k == 0) return -1.0f;
  for (uint8_t i = 1; i < k; i++) {  // insertion sort (k <= 9)
    float x = v[i];
    int8_t j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return v[k / 2];
}

void distanceService() {
  if (state != ST_IDLE || rail.isRunning() || turn.isRunning()) return;
  if (millis() - distStamp < 500) return;
  distStamp = millis();
  distNow = pingCm();
}

// Is a pot below the sensor? (distance clearly differs from the empty-base reading)
bool potBelow(float cm) {
  if (cm <= 0 || cfg.baselineCm <= 0) return false;
  float half = cfg.deltaCm * 0.5f;
  return cfg.potCloser ? (cm <= cfg.baselineCm - half) : (cm >= cfg.baselineCm + half);
}

// ----------------------------------------------------------------------------
//  Queue / sequencer
// ----------------------------------------------------------------------------
bool qPush(CmdType t, uint8_t pot = 0, uint16_t secs = 0, bool force = false, int32_t arg = 0) {
  if (qCount >= QUEUE_MAX) return false;
  queue[qCount++] = Cmd{t, pot, secs, force, arg};
  return true;
}

Cmd qPop() {
  Cmd c = queue[0];
  for (uint8_t i = 1; i < qCount; i++) queue[i - 1] = queue[i];
  qCount--;
  return c;
}

void clearPending() { for (uint8_t i = 0; i < 3; i++) potPending[i] = false; }

void clearError() {
  if (state == ST_ERROR) { state = ST_IDLE; statusMsg = "Idle"; }
  errorMsg = "";
}

void fail(const String& msg) {
  // after a failed homing / scan the position reference cannot be trusted any more
  if (curActive && (cur.type == CMD_HOME || cur.type == CMD_CALIBRATE)) homed = false;
  rail.stopNow();
  rail.release();
  pumpSet(false);
  qCount = 0;
  curActive = false;
  clearPending();
  activePot = 0;
  parkPending = false;
  state = ST_ERROR;
  errorMsg = msg;
  statusMsg = "Error";
  addEvent("error", 0, 0, 0, msg);
  Serial.println("[ERROR] " + msg);
}

void stopAll() {
  rail.stopNow();
  rail.release();
  pumpSet(false);
  qCount = 0;
  curActive = false;
  clearPending();
  activePot = 0;
  parkPending = false;
  state = ST_IDLE;
  statusMsg = "Stopped";
  errorMsg = "";
}

void finishCmd(const char* msg) {
  curActive = false;
  activePot = 0;
  rail.release();
  state = ST_IDLE;
  statusMsg = msg;
}

long potTarget(uint8_t pot) {
  long t = (long)cfg.potPos[pot] + cfg.nozzleOffset;
  if (t < 0) t = 0;
  if (cfg.railSteps > 0 && t > cfg.railSteps) t = cfg.railSteps;
  return t;
}

uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) std::swap(a, b);
  if (b > c) std::swap(b, c);
  if (a > b) std::swap(a, b);
  return b;
}

// Find the pots in scanMm[]. Returns false and fills err when the scan does not look right.
bool analyzeScan(String& err) {
  scanPotsFound = 0;
  if (scanN < 20) { err = "Scan too short - check the End IR sensor"; return false; }

  uint16_t n = 0;
  for (uint16_t i = 0; i < scanN; i++) if (scanMm[i]) sortBuf[n++] = scanMm[i];
  if (n < 20) { err = "No ultrasonic echo - check TRIG/ECHO wiring and the 5V->3.3V divider"; return false; }
  std::sort(sortBuf, sortBuf + n);

  // The "empty base" level is the far end of the distribution when pots are closer than the base,
  // and the near end when pots sit lower than the base.
  scanBaselineMm = cfg.potCloser ? sortBuf[(uint32_t)n * 90 / 100] : sortBuf[(uint32_t)n * 10 / 100];
  scanThrMm = cfg.deltaCm * 10.0f;

  for (uint16_t i = 0; i < scanN; i++) {
    uint16_t a = scanMm[i > 0 ? i - 1 : i], b = scanMm[i], c = scanMm[i + 1 < scanN ? i + 1 : i];
    uint16_t m = median3(a, b, c);
    bool dev = m && (cfg.potCloser ? (m < scanBaselineMm - scanThrMm) : (m > scanBaselineMm + scanThrMm));
    flagBuf[i] = dev ? 1 : 0;
  }

  uint8_t total = 0;
  int i = 0;
  while (i < (int)scanN) {
    if (!flagBuf[i]) { i++; continue; }
    int s = i, e = i, gap = 0, j = i + 1;
    while (j < (int)scanN) {
      if (flagBuf[j]) { e = j; gap = 0; }
      else if (++gap > MERGE_GAP) break;
      j++;
    }
    if (e - s + 1 >= MIN_RUN) {
      if (total < EXPECTED_POTS) { scanPotS[total] = s; scanPotE[total] = e; }
      total++;
    }
    i = e + 1;
  }
  scanPotsFound = total;
  if (total != EXPECTED_POTS) {
    err = "Found " + String(total) + " pot(s), expected " + String(EXPECTED_POTS) +
          ". Open the scan chart and tune the distance change / pot direction.";
    return false;
  }
  for (uint8_t k = 0; k < EXPECTED_POTS; k++)
    cfg.potPos[k] = ((long)(scanPotS[k] + scanPotE[k]) * SCAN_SAMPLE_STEPS) / 2;
  cfg.baselineCm = scanBaselineMm / 10.0f;

  // typical (median) distance to each pot lid
  for (uint8_t k = 0; k < EXPECTED_POTS; k++) {
    uint16_t m = 0;
    for (int i = scanPotS[k]; i <= scanPotE[k]; i++) if (scanMm[i]) sortBuf[m++] = scanMm[i];
    if (m) {
      std::sort(sortBuf, sortBuf + m);
      cfg.potCm[k] = sortBuf[m / 2] / 10.0f;
    } else {
      cfg.potCm[k] = 0;
    }
  }
  return true;
}

void startCmd() {
  phase = 0;
  phaseT0 = millis();
  switch (cur.type) {
    case CMD_HOME:
      state = ST_HOMING;
      statusMsg = "Homing";
      if (irHome()) {  // already at the Home sensor
        rail.stopNow();
        rail.setPosition(0);
        homed = true;
        finishCmd("At home");
        return;
      }
      endArmed = !irEnd();
      cmdStartPos = rail.position();
      rail.setAcceleration(RAIL_ACCEL);
      rail.runAt(-HOME_SPEED);
      break;

    case CMD_CALIBRATE:
      state = ST_CALIBRATING;
      statusMsg = "Scanning rail";
      if (!homed || !irHome()) { fail("Calibration must start at the Home sensor"); return; }
      scanN = 0;
      scanValid = 0;
      nextSamplePos = 0;
      rail.setAcceleration(RAIL_ACCEL);
      rail.runAt((float)cfg.scanSpeed);
      break;

    case CMD_GOTO: {
      if (!cfg.calibrated) { fail("Run the rail calibration first"); return; }
      if (!homed) { fail("Carriage is not homed"); return; }
      state = ST_MOVING;
      activePot = cur.pot + 1;
      statusMsg = "Moving to pot " + String(activePot);
      rail.setMaxSpeed((float)cfg.railSpeed);
      rail.setAcceleration(RAIL_ACCEL);
      rail.moveTo(potTarget(cur.pot));
      break;
    }

    case CMD_PUMP:
      state = ST_WATERING;
      activePot = cur.pot + 1;
      statusMsg = "Watering pot " + String(activePot);
      break;

    case CMD_JOG:
      state = ST_MOVING;
      statusMsg = "Jog";
      rail.setMaxSpeed(500);
      rail.setAcceleration(RAIL_ACCEL);
      rail.moveTo(rail.position() + cur.arg);
      break;
  }
}

void stepCmd() {
  switch (cur.type) {
    case CMD_HOME: {
      if (irHome()) {
        rail.stopNow();
        rail.setPosition(0);
        homed = true;
        finishCmd("At home");
        return;
      }
      if (!irEnd()) endArmed = true;
      if (endArmed && irEnd()) { fail("Hit the End sensor while homing - flip INVERT_RAIL_DIR in config.h"); return; }
      if (labs(rail.position() - cmdStartPos) > MAX_TRAVEL_STEPS) { fail("Home sensor not found - check IR 1 wiring"); return; }
      break;
    }

    case CMD_CALIBRATE: {
      long pos = rail.position();
      if (pos >= nextSamplePos && scanN < MAX_SCAN) {
        float d = pingCm();
        scanMm[scanN++] = d > 0 ? (uint16_t)(d * 10.0f + 0.5f) : 0;
        if (d > 0) scanValid++;
        nextSamplePos += SCAN_SAMPLE_STEPS;
        if (scanN == 20 && scanValid == 0) {  // do not waste minutes on a scan that cannot work
          rail.stopNow();
          fail("No ultrasonic echo in the first 20 readings. Check VCC, TRIG (GPIO25), ECHO (GPIO35) and the divider, then open /api/ultrasonic");
          return;
        }
      }
      if (irEnd()) {
        rail.stopNow();
        cfg.railSteps = rail.position();
        String err;
        if (!analyzeScan(err)) { fail(err); return; }
        cfg.calibrated = true;
        cfg.verifyPot = true;  // the ultrasonic sensor just worked, so use it
        saveSettings();
        addEvent("calibrated", 0, 0, 0, "Rail " + String(cfg.railSteps) + " steps");
        parkPending = true;
        finishCmd("Calibrated");
        return;
      }
      if (scanN >= MAX_SCAN) { rail.stopNow(); fail("Scan buffer full - raise SCAN_SAMPLE_STEPS in config.h"); return; }
      if (pos > MAX_TRAVEL_STEPS) { fail("End sensor not found - check IR 2 wiring"); return; }
      break;
    }

    case CMD_GOTO: {
      int dir = rail.direction();
      if ((dir > 0 && irEnd()) || (dir < 0 && irHome())) { fail("Limit sensor hit while moving - recalibrate the rail"); return; }
      if (!rail.isRunning()) finishCmd("At pot");
      break;
    }

    case CMD_PUMP: {
      if (phase == 0) {  // let the carriage stop shaking
        if (millis() - phaseT0 >= 500) phase = 1;
      } else if (phase == 1) {
        if (!cur.force && cfg.verifyPot) {
          float d = pingMedianCm(5);
          if (!potBelow(d)) {
            fail("No pot detected under the nozzle (distance " + (d > 0 ? String(d, 1) + " cm" : String("n/a")) +
                 "). Water skipped.");
            return;
          }
        }
        addEvent("pump", cur.pot + 1, cur.secs, soilPct(cur.pot), "");
        pumpFor((uint32_t)cur.secs * 1000UL);
        phase = 2;
      } else if (phase == 2) {
        if (!pumpOn) {
          lastWaterMs[cur.pot] = millis();
          everWatered[cur.pot] = true;
          if (waterCountToday[cur.pot] < 255) waterCountToday[cur.pot]++;
          potPending[cur.pot] = false;
          addEvent("water", cur.pot + 1, cur.secs, soilPct(cur.pot), "");
          parkPending = true;
          finishCmd("Watered");
        }
      }
      break;
    }

    case CMD_JOG: {
      int dir = rail.direction();
      if ((dir > 0 && irEnd()) || (dir < 0 && irHome())) { rail.stopNow(); finishCmd("Jog stopped at sensor"); return; }
      if (!rail.isRunning()) finishCmd("Jog done");
      break;
    }
  }
}

void sequencerService() {
  if (state == ST_ERROR) return;
  if (!curActive) {
    if (qCount == 0) {
      if (parkPending && cfg.park) { qPush(CMD_HOME); }
      parkPending = false;
      return;
    }
    cur = qPop();
    curActive = true;
    startCmd();
    if (!curActive) return;  // finished or failed inside startCmd()
  }
  stepCmd();
}

// ----------------------------------------------------------------------------
//  Job helpers (used by the API and the scheduler)
// ----------------------------------------------------------------------------
bool enqueueWater(uint8_t pot, uint16_t secs, bool force, String& why) {
  if (!cfg.calibrated) { why = "Calibrate the rail first"; return false; }
  if (potPending[pot]) { why = "Pot " + String(pot + 1) + " is already queued"; return false; }
  if (QUEUE_MAX - qCount < 3) { why = "Queue is full"; return false; }
  if (secs < 1) secs = 1;
  if (secs > PUMP_MAX_S) secs = PUMP_MAX_S;
  if (!homed) qPush(CMD_HOME);
  qPush(CMD_GOTO, pot);
  qPush(CMD_PUMP, pot, secs, force);
  potPending[pot] = true;
  return true;
}

int autoCheck() {
  int queued = 0;
  String why;
  uint32_t soakMs = (uint32_t)cfg.soakMin * 60000UL;
  for (uint8_t i = 0; i < 3; i++) {
    if (!cfg.potEnabled[i] || !soilOk(i) || potPending[i]) continue;
    if (soilPct(i) > cfg.dryPct[i]) continue;
    if (everWatered[i] && millis() - lastWaterMs[i] < soakMs) continue;
    if (waterCountToday[i] >= MAX_WATER_PER_DAY) continue;
    if (enqueueWater(i, cfg.waterSecs[i], false, why)) queued++;
  }
  return queued;
}

void schedulerService() {
  if (!cfg.autoMode || !cfg.calibrated) return;
  if ((int32_t)(millis() - nextCheckMs) < 0) return;
  nextCheckMs = millis() + (uint32_t)cfg.checkMin * 60000UL;
  if (state == ST_ERROR) return;
  int q = autoCheck();
  if (q) addEvent("auto", 0, 0, q, String(q) + " pot(s) below threshold");
}

// ----------------------------------------------------------------------------
//  Turntable (lower motor)
// ----------------------------------------------------------------------------
long turnDegrees() { return (turn.position() * 360L) / STEPS_PER_REV_HALF; }

void turnService() {
  turn.update();
  if (turn.isRunning()) return;

  if (sweeping) {
    if (sweepMoving) {  // just arrived at a stop: hold for a moment
      sweepMoving = false;
      sweepPauseUntil = millis() + SWEEP_PAUSE_MS;
    }
    if ((int32_t)(millis() - sweepPauseUntil) < 0) return;
    if (sweepStep < 4) {
      static const int8_t mul[4] = {1, 0, -1, 0};  // +180, back, -180, back
      turn.moveTo(sweepCenter + (long)mul[sweepStep] * ((long)SWEEP_DEG * STEPS_PER_REV_HALF / 360));
      sweepStep++;
      sweepMoving = true;
      return;
    }
    sweeping = false;  // finished, back at the centre
  }

  if (turnActive) { turn.release(); turnActive = false; }
}

// ----------------------------------------------------------------------------
//  HTTP API
// ----------------------------------------------------------------------------
void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void replyOk(const String& msg = "") {
  String j = "{\"ok\":true";
  if (msg.length()) j += ",\"msg\":\"" + jsonEscape(msg) + "\"";
  j += "}";
  sendJson(200, j);
}

void replyErr(int code, const String& msg) {
  sendJson(code, "{\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}");
}

bool authOk() {
  if (strlen(API_KEY) == 0) return true;
  if (server.header("X-Api-Key") == API_KEY) return true;
  replyErr(401, "Unauthorized");
  return false;
}

// "pot" argument 1..3 -> index 0..2, or -1 after replying with an error
int potArg() {
  int p = server.arg("pot").toInt();
  if (p < 1 || p > 3) { replyErr(400, "pot must be 1, 2 or 3"); return -1; }
  return p - 1;
}

const char* stateName() {
  switch (state) {
    case ST_IDLE: return "idle";
    case ST_HOMING: return "homing";
    case ST_CALIBRATING: return "calibrating";
    case ST_MOVING: return "moving";
    case ST_WATERING: return "watering";
    default: return "error";
  }
}

void handleStatus() {
  String j;
  j.reserve(2400);
  j += '{';
  kvs(j, "fw", FW_VERSION);
  kvi(j, "uptime_s", millis() / 1000);
  kvi(j, "rssi", WiFi.RSSI());
  kvs(j, "ip", WiFi.localIP().toString());
  kvs(j, "state", stateName());
  kvs(j, "message", statusMsg);
  kvs(j, "error", errorMsg);
  kvb(j, "homed", homed);
  kvb(j, "calibrated", cfg.calibrated);
  kvi(j, "pos", rail.position());
  kvi(j, "rail_steps", cfg.railSteps);
  kvb(j, "ir_home", irHome());
  kvb(j, "ir_end", irEnd());
  bool fresh = (distNow > 0) && (millis() - distStamp < 3000);
  kvf(j, "distance_cm", fresh ? distNow : -1.0f);
  kvf(j, "baseline_cm", cfg.baselineCm);
  kvb(j, "pump", pumpOn);
  kvb(j, "auto", cfg.autoMode);
  kvi(j, "queue", qCount + (curActive ? 1 : 0));
  kvi(j, "active_pot", activePot);
  kvi(j, "last_event_id", evNext - 1);
  kvi(j, "next_check_s", cfg.autoMode ? (long)((int32_t)(nextCheckMs - millis()) / 1000) : -1);

  j += "\"settings\":{";
  kvi(j, "check_min", cfg.checkMin);
  kvi(j, "soak_min", cfg.soakMin);
  kvi(j, "nozzle_offset", cfg.nozzleOffset);
  kvf(j, "delta_cm", cfg.deltaCm);
  kvb(j, "pot_closer", cfg.potCloser);
  kvb(j, "park", cfg.park);
  kvb(j, "verify", cfg.verifyPot);
  kvi(j, "rail_speed", cfg.railSpeed);
  kvi(j, "scan_speed", cfg.scanSpeed);
  trimComma(j);
  j += "},";

  j += "\"turntable\":{";
  kvb(j, "busy", turnActive);
  kvb(j, "sweeping", sweeping);
  kvi(j, "deg", turnDegrees());
  trimComma(j);
  j += "},";

  j += "\"pots\":[";
  for (uint8_t i = 0; i < 3; i++) {
    j += '{';
    kvi(j, "id", i + 1);
    kvi(j, "raw", (long)(soilEma[i] + 0.5f));
    kvi(j, "moisture", soilPct(i));
    kvb(j, "sensor_ok", soilOk(i));
    kvb(j, "enabled", cfg.potEnabled[i]);
    kvi(j, "threshold", cfg.dryPct[i]);
    kvi(j, "water_secs", cfg.waterSecs[i]);
    kvi(j, "pos", cfg.potPos[i]);
    kvf(j, "dist_cm", cfg.potCm[i]);
    kvi(j, "dry", cfg.soilDry[i]);
    kvi(j, "wet", cfg.soilWet[i]);
    kvb(j, "pending", potPending[i]);
    kvi(j, "last_water_s", everWatered[i] ? (long)((millis() - lastWaterMs[i]) / 1000) : -1);
    kvi(j, "count_today", waterCountToday[i]);
    trimComma(j);
    j += "},";
  }
  trimComma(j);
  j += "]}";
  sendJson(200, j);
}

void handleScan() {
  String j;
  j.reserve(scanN * 5 + 400);
  j += '{';
  kvi(j, "step", SCAN_SAMPLE_STEPS);
  kvi(j, "n", scanN);
  kvf(j, "baseline_cm", scanBaselineMm / 10.0f);
  kvf(j, "threshold_cm", scanThrMm / 10.0f);
  kvb(j, "pot_closer", cfg.potCloser);
  kvi(j, "found", scanPotsFound);
  j += "\"pots\":[";
  for (uint8_t k = 0; k < scanPotsFound && k < EXPECTED_POTS; k++) {
    j += "{\"s\":" + String(scanPotS[k]) + ",\"e\":" + String(scanPotE[k]) + "},";
  }
  trimComma(j);
  j += "],\"dist\":[";
  for (uint16_t i = 0; i < scanN; i++) {
    j += String(scanMm[i]);
    if (i + 1 < scanN) j += ',';
  }
  j += "]}";
  sendJson(200, j);
}

void handleEvents() {
  uint32_t since = (uint32_t)server.arg("since").toInt();
  uint32_t first = since + 1;
  if (evNext > EVENT_MAX && first < evNext - EVENT_MAX) first = evNext - EVENT_MAX;
  String j = "{\"events\":[";
  for (uint32_t id = first; id < evNext; id++) {
    const Event& e = events[(id - 1) % EVENT_MAX];
    j += '{';
    kvi(j, "id", e.id);
    kvi(j, "t", e.t);
    kvs(j, "type", e.type);
    kvi(j, "pot", e.pot);
    kvi(j, "secs", e.secs);
    kvi(j, "moisture", e.moisture);
    kvs(j, "msg", e.msg);
    trimComma(j);
    j += "},";
  }
  trimComma(j);
  j += "],\"last_id\":" + String(evNext - 1) + "}";
  sendJson(200, j);
}

// Wiring diagnostic: open http://<ip>/api/ultrasonic in a browser while the carriage is idle.
void handleUltrasonicTest() {
  String j = "{";
  kvi(j, "trig_pin", PIN_US_TRIG);
  kvi(j, "echo_pin", PIN_US_ECHO);
  kvi(j, "echo_idle", digitalRead(PIN_US_ECHO));  // must be 0 before a ping
  j += "\"pulses_us\":[";
  uint8_t hits = 0;
  for (uint8_t i = 0; i < 5; i++) {
    digitalWrite(PIN_US_TRIG, LOW);
    delayMicroseconds(4);
    digitalWrite(PIN_US_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_US_TRIG, LOW);
    unsigned long us = pulseIn(PIN_US_ECHO, HIGH, 30000UL);  // 0 = no pulse within 30 ms
    if (us) hits++;
    j += String(us);
    if (i < 4) j += ',';
    delay(60);
  }
  j += "],";
  kvi(j, "hits", hits);
  kvi(j, "echo_after", digitalRead(PIN_US_ECHO));
  trimComma(j);
  j += "}";
  sendJson(200, j);
}

void handleHome() {
  if (!authOk()) return;
  clearError();
  if (!qPush(CMD_HOME)) return replyErr(409, "Queue is full");
  replyOk("Homing queued");
}

void handleCalibrate() {
  if (!authOk()) return;
  clearError();
  if (QUEUE_MAX - qCount < 3) return replyErr(409, "Queue is full");
  qPush(CMD_HOME);  // always start from a fresh zero
  qPush(CMD_CALIBRATE);
  replyOk("Calibration queued");
}

void handleGoto() {
  if (!authOk()) return;
  int p = potArg();
  if (p < 0) return;
  clearError();
  if (!cfg.calibrated) return replyErr(409, "Calibrate the rail first");
  if (!homed) qPush(CMD_HOME);
  qPush(CMD_GOTO, p);
  replyOk("Moving to pot " + String(p + 1));
}

void handleWater() {
  if (!authOk()) return;
  int p = potArg();
  if (p < 0) return;
  clearError();
  int secs = server.hasArg("secs") ? server.arg("secs").toInt() : cfg.waterSecs[p];
  String why;
  if (!enqueueWater(p, secs, server.arg("force") == "1", why)) return replyErr(409, why);
  replyOk("Watering pot " + String(p + 1) + " queued");
}

void handleCheck() {
  if (!authOk()) return;
  clearError();
  if (!cfg.calibrated) return replyErr(409, "Calibrate the rail first");
  int q = autoCheck();
  replyOk(q ? String(q) + " pot(s) queued" : "All pots are moist enough");
}

void handleStop() {
  if (!authOk()) return;
  stopAll();
  turn.stopNow();
  turn.release();
  turnActive = false;
  sweeping = false;
  replyOk("Stopped");
}

void handlePump() {  // bench test: run the pump without moving the carriage
  if (!authOk()) return;
  if (state != ST_IDLE || curActive) return replyErr(409, "Busy - wait for the current job");
  int secs = server.arg("secs").toInt();
  if (secs < 1) secs = 1;
  if (secs > 10) secs = 10;
  pumpFor((uint32_t)secs * 1000UL);
  replyOk("Pump test " + String(secs) + " s");
}

void handleJog() {  // direction test while bringing the rail up
  if (!authOk()) return;
  if (state != ST_IDLE || curActive) return replyErr(409, "Busy - wait for the current job");
  long steps = server.arg("steps").toInt();
  if (steps == 0 || steps > 5000 || steps < -5000) return replyErr(400, "steps must be between -5000 and 5000");
  clearError();
  qPush(CMD_JOG, 0, 0, false, steps);
  replyOk("Jog " + String(steps) + " steps (positive = towards End)");
}

void handleAuto() {
  if (!authOk()) return;
  cfg.autoMode = server.arg("on") == "1";
  if (cfg.autoMode) nextCheckMs = millis() + 5000;
  saveSettings();
  replyOk(cfg.autoMode ? "Auto watering ON" : "Auto watering OFF");
}

void handleTurntable() {
  if (!authOk()) return;
  if (turnActive) return replyErr(409, "Turntable is already rotating");
  long deg = server.arg("deg").toInt();
  if (deg == 0 || deg > 720 || deg < -720) return replyErr(400, "deg must be between -720 and 720");
  turn.setMaxSpeed(TURN_SPEED);
  turn.setAcceleration(TURN_ACCEL);
  turn.moveTo(turn.position() + (deg * STEPS_PER_REV_HALF) / 360);
  turnActive = true;
  replyOk("Rotating " + String(deg) + " deg");
}

void handleSweep() {  // +180 deg, back to centre, -180 deg, back to centre
  if (!authOk()) return;
  if (turnActive) return replyErr(409, "Turntable is already moving");
  turn.setMaxSpeed(TURN_SPEED);
  turn.setAcceleration(TURN_ACCEL);
  sweepCenter = turn.position();  // the sweep is symmetric around wherever the turntable stands now
  sweepStep = 0;
  sweepMoving = false;
  sweepPauseUntil = millis();
  sweeping = true;
  turnActive = true;
  replyOk("Turntable sweep started");
}

// Manual set-up that needs neither the ultrasonic sensor nor the End IR sensor:
// jog the carriage, then save positions. what = home | pot | end | done
void handleTeach() {
  if (!authOk()) return;
  if (state != ST_IDLE || curActive || rail.isRunning()) return replyErr(409, "Busy - wait for the carriage to stop");
  String what = server.arg("what");
  long pos = rail.position();

  if (what == "home") {
    rail.setPosition(0);
    homed = true;
    return replyOk("Home position set here (0)");
  }
  if (!homed) return replyErr(409, "Set the Home position first");

  if (what == "pot") {
    int p = potArg();
    if (p < 0) return;
    if (pos <= 0) return replyErr(409, "Move the carriage away from Home (towards End) first");
    cfg.potPos[p] = pos;
    float dcm = pingMedianCm(3);  // distance to this pot, if the ultrasonic sensor works
    cfg.potCm[p] = dcm > 0 ? dcm : 0;
    saveSettings();
    return replyOk("Pot " + String(p + 1) + " saved at " + String(pos) + " steps" +
                   (dcm > 0 ? ", distance " + String(dcm, 1) + " cm" : ""));
  }
  if (what == "end") {
    if (pos <= 0) return replyErr(409, "Move the carriage towards End first");
    cfg.railSteps = pos;
    saveSettings();
    return replyOk("End saved at " + String(pos) + " steps");
  }
  if (what == "done") {
    if (cfg.potPos[0] <= 0 || cfg.potPos[0] >= cfg.potPos[1] || cfg.potPos[1] >= cfg.potPos[2])
      return replyErr(409, "Save pot 1, pot 2 and pot 3 in that order, each further from Home than the last");
    if (cfg.railSteps < cfg.potPos[2] + 500) cfg.railSteps = cfg.potPos[2] + 500;
    cfg.calibrated = true;
    cfg.verifyPot = false;  // no ultrasonic check unless you switch it on in the settings
    saveSettings();
    return replyOk("Teaching finished: 3 pots saved");
  }
  replyErr(400, "what must be home, pot, end or done");
}

void handleSoilCal() {
  if (!authOk()) return;
  int p = potArg();
  if (p < 0) return;
  String point = server.arg("point");
  int v = (int)(soilEma[p] + 0.5f);
  if (point == "dry") cfg.soilDry[p] = v;
  else if (point == "wet") cfg.soilWet[p] = v;
  else return replyErr(400, "point must be dry or wet");
  saveSettings();
  replyOk("Saved " + point + " = " + String(v) + " for pot " + String(p + 1));
}

void handleSettings() {
  if (!authOk()) return;
  auto num = [](const char* k, long lo, long hi, long& out) {
    if (!server.hasArg(k)) return false;
    long v = server.arg(k).toInt();
    out = v < lo ? lo : (v > hi ? hi : v);
    return true;
  };
  long v;
  if (num("check_min", 1, 1440, v)) cfg.checkMin = v;
  if (num("soak_min", 1, 1440, v)) cfg.soakMin = v;
  if (num("nozzle_offset", -3000, 3000, v)) cfg.nozzleOffset = v;
  if (num("rail_speed", 100, 1000, v)) cfg.railSpeed = v;
  if (num("scan_speed", 100, 600, v)) cfg.scanSpeed = v;
  if (num("pot_closer", 0, 1, v)) cfg.potCloser = v;
  if (num("park", 0, 1, v)) cfg.park = v;
  if (num("verify", 0, 1, v)) cfg.verifyPot = v;
  if (server.hasArg("delta_cm")) {
    float d = server.arg("delta_cm").toFloat();
    cfg.deltaCm = d < 0.5f ? 0.5f : (d > 30.0f ? 30.0f : d);
  }
  const char* tk[3] = {"t1", "t2", "t3"};
  const char* sk[3] = {"s1", "s2", "s3"};
  const char* ek[3] = {"e1", "e2", "e3"};
  for (uint8_t i = 0; i < 3; i++) {
    if (num(tk[i], 5, 95, v)) cfg.dryPct[i] = v;
    if (num(sk[i], 1, PUMP_MAX_S, v)) cfg.waterSecs[i] = v;
    if (num(ek[i], 0, 1, v)) cfg.potEnabled[i] = v;
  }
  saveSettings();
  replyOk("Settings saved");
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "X-Api-Key,Content-Type");
    server.send(204);
    return;
  }
  replyErr(404, "Not found");
}

void setupServer() {
  static const char* hdrs[] = {"X-Api-Key"};
  server.collectHeaders(hdrs, 1);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.on("/api/ultrasonic", HTTP_GET, handleUltrasonicTest);
  server.on("/api/home", HTTP_POST, handleHome);
  server.on("/api/calibrate", HTTP_POST, handleCalibrate);
  server.on("/api/goto", HTTP_POST, handleGoto);
  server.on("/api/water", HTTP_POST, handleWater);
  server.on("/api/check", HTTP_POST, handleCheck);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/pump", HTTP_POST, handlePump);
  server.on("/api/jog", HTTP_POST, handleJog);
  server.on("/api/teach", HTTP_POST, handleTeach);
  server.on("/api/auto", HTTP_POST, handleAuto);
  server.on("/api/turntable", HTTP_POST, handleTurntable);
  server.on("/api/sweep", HTTP_POST, handleSweep);
  server.on("/api/soilcal", HTTP_POST, handleSoilCal);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ----------------------------------------------------------------------------
//  Wi-Fi
// ----------------------------------------------------------------------------
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Wi-Fi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nIP: " + WiFi.localIP().toString());
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("\nWi-Fi not connected yet - the controller keeps working and will retry");
  }
}

void wifiService() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < 15000) return;
  lastWifiTry = millis();
  WiFi.reconnect();
}

// ----------------------------------------------------------------------------
//  Arduino entry points
// ----------------------------------------------------------------------------
void setup() {
  // pump first, so it can never glitch on at boot
  pinMode(PIN_PUMP, OUTPUT);
  pumpSet(false);

  Serial.begin(115200);
  delay(200);
  Serial.println("\nPlant Rail controller " FW_VERSION);

  pinMode(PIN_US_TRIG, OUTPUT);
  pinMode(PIN_US_ECHO, INPUT);
  pinMode(PIN_IR_HOME, INPUT_PULLUP);
  pinMode(PIN_IR_END, INPUT_PULLUP);

  analogReadResolution(12);
  for (uint8_t i = 0; i < 3; i++) {
    pinMode(SOIL_PINS[i], INPUT);
    analogSetPinAttenuation(SOIL_PINS[i], ADC_11db);
  }

  rail.begin();
  turn.begin();
  rail.release();
  turn.release();
  loadSettings();

  connectWifi();
  setupServer();

  dayStartMs = millis();
  nextCheckMs = millis() + FIRST_CHECK_DELAY_MS;
  state = ST_IDLE;
  statusMsg = "Ready";
  if (AUTO_HOME_ON_BOOT) qPush(CMD_HOME);
}

void loop() {
  server.handleClient();
  rail.update();
  turnService();
  pumpService();
  sequencerService();
  soilService();
  distanceService();
  schedulerService();
  wifiService();
}
