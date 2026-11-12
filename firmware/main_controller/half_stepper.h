#pragma once
#include <Arduino.h>

// Small driver for a 28BYJ-48 (or any 4-wire unipolar stepper) behind a ULN2003 board.
// Half-step drive, linear acceleration, no external library needed.
//
//   - Wire the ULN2003 IN1..IN4 pins in coil order (IN1, IN2, IN3, IN4) - no pin swapping needed.
//   - update() must be called very regularly. The sketch calls it from a 250 us hardware timer, so the
//     stepping keeps its rhythm while Wi-Fi / the web server / sensor code are busy. (Calling it from
//     loop() made the motor stall: a 28BYJ-48 cannot restart at speed after a 10 ms pause.)
//   - All methods are safe to call from loop() while update() runs in the timer task.
//   - Position is counted in half-steps. Positive = the direction picked by 'reversed'.
class HalfStepper {
 public:
  HalfStepper(uint8_t in1, uint8_t in2, uint8_t in3, uint8_t in4, bool reversed = false)
      : pins_{in1, in2, in3, in4}, reversed_(reversed) {}

  void begin() {
    for (uint8_t i = 0; i < 4; i++) {
      pinMode(pins_[i], OUTPUT);
      digitalWrite(pins_[i], LOW);
    }
  }

  void setMaxSpeed(float stepsPerSec) { maxV_ = stepsPerSec; }
  void setAcceleration(float stepsPerSec2) { acc_ = stepsPerSec2; }

  // Accelerated move to an absolute position.
  void moveTo(long target) {
    portENTER_CRITICAL(&mux_);
    target_ = target;
    if (target_ == pos_) {
      mode_ = IDLE;
    } else {
      dir_ = (target_ > pos_) ? 1 : -1;
      startRamp(GOTO);
    }
    portEXIT_CRITICAL(&mux_);
  }

  // Run until stopNow() is called. Positive speed = positive direction.
  void runAt(float stepsPerSec) {
    if (stepsPerSec == 0) { stopNow(); return; }
    portENTER_CRITICAL(&mux_);
    runV_ = fabsf(stepsPerSec);
    dir_ = (stepsPerSec > 0) ? 1 : -1;
    if (mode_ != RUN) startRamp(RUN);
    portEXIT_CRITICAL(&mux_);
  }

  void stopNow() {
    portENTER_CRITICAL(&mux_);
    mode_ = IDLE;
    v_ = 0;
    portEXIT_CRITICAL(&mux_);
  }

  // De-energise the coils (cool and quiet). The 28BYJ-48 gearbox holds position by itself.
  void release() {
    portENTER_CRITICAL(&mux_);
    for (uint8_t i = 0; i < 4; i++) digitalWrite(pins_[i], LOW);
    portEXIT_CRITICAL(&mux_);
  }

  long position() const { return pos_; }
  void setPosition(long p) {
    portENTER_CRITICAL(&mux_);
    pos_ = p;
    portEXIT_CRITICAL(&mux_);
  }
  long distanceToGo() const { return mode_ == GOTO ? target_ - pos_ : 0; }
  bool isRunning() const { return mode_ != IDLE; }
  int direction() const { return dir_; }

  void update() {
    portENTER_CRITICAL(&mux_);
    step();
    portEXIT_CRITICAL(&mux_);
  }

 private:
  enum Mode : uint8_t { IDLE, GOTO, RUN };

  static constexpr float MIN_V = 120.0f;   // start / final-approach speed (steps/s)

  void startRamp(Mode m) {
    mode_ = m;
    v_ = MIN_V;
    lastUpdateUs_ = lastStepUs_ = micros();
  }

  void step() {
    if (mode_ == IDLE) return;
    uint32_t now = micros();
    float dt = (now - lastUpdateUs_) * 1e-6f;
    lastUpdateUs_ = now;

    float goal;
    if (mode_ == GOTO) {
      long remaining = labs(target_ - pos_);
      if (remaining == 0) { mode_ = IDLE; v_ = 0; return; }
      goal = sqrtf(2.0f * acc_ * (float)remaining);   // fastest speed that still lets us stop in time
      if (goal > maxV_) goal = maxV_;
    } else {
      goal = runV_;
    }
    if (goal < MIN_V) goal = MIN_V;

    float dv = acc_ * dt;
    if (v_ < goal) { v_ += dv; if (v_ > goal) v_ = goal; }
    else           { v_ -= dv; if (v_ < goal) v_ = goal; }
    if (v_ < MIN_V) v_ = MIN_V;

    uint32_t interval = (uint32_t)(1000000.0f / v_);
    if ((uint32_t)(now - lastStepUs_) >= interval) {
      lastStepUs_ = now;
      pos_ += dir_;
      apply();
    }
  }

  void apply() {
    static const uint8_t SEQ[8] = {0b0001, 0b0011, 0b0010, 0b0110, 0b0100, 0b1100, 0b1000, 0b1001};
    uint8_t idx = (uint8_t)(pos_ & 7);
    if (reversed_) idx = 7 - idx;
    uint8_t m = SEQ[idx];
    for (uint8_t i = 0; i < 4; i++) digitalWrite(pins_[i], (m >> i) & 1);
  }

  uint8_t pins_[4];
  bool reversed_;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  volatile Mode mode_ = IDLE;
  volatile long pos_ = 0;
  long target_ = 0;
  volatile int dir_ = 1;
  float v_ = 0, maxV_ = 600, acc_ = 500, runV_ = 300;
  uint32_t lastStepUs_ = 0, lastUpdateUs_ = 0;
};
