"""Clients for the main ESP32 controller and the ESP32-CAM, plus a simulator for hardware-free demos."""
import math
import random
import threading
import time
from typing import Any

import requests

from . import config


class DeviceError(Exception):
    """The controller / camera could not be reached, or it refused the command."""

    def __init__(self, message: str, status: int = 502):
        super().__init__(message)
        self.status = status


# ---------------------------------------------------------------------------
#  Real hardware
# ---------------------------------------------------------------------------
class Device:
    def __init__(self, base: str, api_key: str):
        self.base = base
        self.http = requests.Session()
        self.headers = {"X-Api-Key": api_key} if api_key else {}

    def _get(self, path: str, params: dict | None = None, timeout: float = 3.0) -> dict[str, Any]:
        try:
            r = self.http.get(self.base + path, params=params, timeout=timeout)
            r.raise_for_status()
            return r.json()
        except (requests.RequestException, ValueError) as e:
            raise DeviceError(f"Controller not reachable ({e.__class__.__name__})") from e

    def status(self) -> dict[str, Any]:
        return self._get("/api/status")

    def scan(self) -> dict[str, Any]:
        return self._get("/api/scan", timeout=6.0)

    def events(self, since: int) -> dict[str, Any]:
        return self._get("/api/events", {"since": since})

    def command(self, action: str, params: dict[str, Any]) -> dict[str, Any]:
        try:
            r = self.http.post(
                f"{self.base}/api/{action}", data={k: str(v) for k, v in params.items()},
                headers=self.headers, timeout=5.0,
            )
        except requests.RequestException as e:
            raise DeviceError(f"Controller not reachable ({e.__class__.__name__})") from e
        try:
            body = r.json()
        except ValueError:
            raise DeviceError(f"Unexpected reply from controller (HTTP {r.status_code})")
        if not r.ok or not body.get("ok", False):
            raise DeviceError(body.get("error", f"HTTP {r.status_code}"), status=409 if r.status_code < 500 else 502)
        return body


class Camera:
    def __init__(self, base: str):
        self.base = base

    def snapshot(self, hq: bool = False, flash: bool = False) -> tuple[bytes, str]:
        params = {}
        if hq:
            params["hq"] = 1
        if flash:
            params["flash"] = 1
        try:
            r = requests.get(f"{self.base}/capture", params=params, timeout=12 if hq else 6)
            r.raise_for_status()
        except requests.RequestException as e:
            raise DeviceError(f"Camera not reachable ({e.__class__.__name__})") from e
        return r.content, r.headers.get("Content-Type", "image/jpeg")


# ---------------------------------------------------------------------------
#  Simulator (MOCK_DEVICE=1) - same JSON shapes as the firmware
# ---------------------------------------------------------------------------
class MockCamera:
    def snapshot(self, hq: bool = False, flash: bool = False) -> tuple[bytes, str]:
        return (config.WEB_DIR / "assets" / "mock-leaf.svg").read_bytes(), "image/svg+xml"


class MockDevice:
    RAIL = 41000
    SPEED = 5200.0  # steps/s - fast so a demo does not take minutes
    BASE_CM = 24.8
    POT_CM = 17.4
    POT_HALF = 1300

    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.boot = time.time()
        self.last = self.boot
        self.pos = 0.0
        self.homed = True
        self.calibrated = True
        self.pot_pos = [8200, 20500, 32800]
        self.moist = [58.0, 23.0, 41.0]
        self.auto = True
        self.cfg = dict(check_min=2, soak_min=5, nozzle_offset=0, delta_cm=3.0, pot_closer=True,
                        park=True, rail_speed=700, scan_speed=400)
        self.pots = [dict(threshold=35, water_secs=5, enabled=True, dry=2900, wet=1300) for _ in range(3)]
        self.state, self.message, self.error = "idle", "Ready", ""
        self.queue: list[dict[str, Any]] = []
        self.cur: dict[str, Any] | None = None
        self.pump_until = 0.0
        self.ev: list[dict[str, Any]] = []
        self.ev_n = 0
        self.last_water: list[float | None] = [None, None, None]
        self.count = [0, 0, 0]
        self.pending = [False, False, False]
        self.next_check = time.time() + 15
        self.turn_deg = 0
        self.sweep_t0 = 0.0
        self.sweep_center = 0
        self.park_pending = False
        self.rng = random.Random(7)

    # ----- simulation -----
    def _event(self, type_: str, pot: int = 0, secs: int = 0, moisture: int = 0, msg: str = "") -> None:
        self.ev_n += 1
        self.ev.append(dict(id=self.ev_n, t=int(time.time() - self.boot), type=type_,
                            pot=pot, secs=secs, moisture=moisture, msg=msg))
        self.ev = self.ev[-24:]

    def _eid(self) -> int:
        return self.ev_n

    def _pct(self, i: int) -> int:
        return int(round(max(0.0, min(100.0, self.moist[i]))))

    def _fail(self, msg: str) -> None:
        self.queue.clear()
        self.cur = None
        self.pending = [False] * 3
        self.state, self.error, self.message = "error", msg, "Error"
        self._event("error", msg=msg)

    def _finish(self, msg: str) -> None:
        self.cur = None
        self.state, self.message = "idle", msg

    def _over_pot(self) -> bool:
        return any(abs(self.pos - p) < self.POT_HALF for p in self.pot_pos)

    def _start(self, c: dict[str, Any]) -> None:
        self.cur, c["phase"], c["t0"] = c, 0, time.time()
        kind = c["type"]
        self.state = {"home": "homing", "calibrate": "calibrating", "goto": "moving",
                      "pump": "watering", "jog": "moving"}[kind]
        self.message = {"home": "Homing", "calibrate": "Scanning rail", "goto": f"Moving to pot {c.get('pot', 0) + 1}",
                        "pump": f"Watering pot {c.get('pot', 0) + 1}", "jog": "Jog"}[kind]
        if kind == "calibrate":
            self.pos = 0.0

    def _step(self, dt: float) -> None:
        now = time.time()
        if self.state == "error":
            return
        if self.pump_until and now >= self.pump_until:
            self.pump_until = 0.0
        if self.pump_until and self.cur and self.cur["type"] == "pump":
            self.moist[self.cur["pot"]] = min(95.0, self.moist[self.cur["pot"]] + 5.5 * dt)
        for i in range(3):
            self.moist[i] = max(2.0, self.moist[i] - 0.012 * dt)

        if self.auto and self.calibrated and now >= self.next_check:
            self.next_check = now + self.cfg["check_min"] * 60
            n = 0
            for i in range(3):
                if self.pots[i]["enabled"] and not self.pending[i] and self._pct(i) <= self.pots[i]["threshold"]:
                    self._queue_water(i, self.pots[i]["water_secs"])
                    n += 1
            if n:
                self._event("auto", moisture=n, msg=f"{n} pot(s) below threshold")

        if not self.cur:
            if self.queue:
                self._start(self.queue.pop(0))
            elif self.park_pending and self.cfg["park"]:
                self.park_pending = False
                self.queue.append(dict(type="home"))
            return

        c = self.cur
        move = self.SPEED * dt
        if c["type"] == "home":
            self.pos = max(0.0, self.pos - move)
            if self.pos <= 0:
                self.homed = True
                self._finish("At home")
        elif c["type"] == "calibrate":
            self.pos = min(float(self.RAIL), self.pos + move * 0.5)
            if self.pos >= self.RAIL:
                self.calibrated = True
                self.park_pending = True
                self._event("calibrated", msg=f"Rail {self.RAIL} steps")
                self._finish("Calibrated")
        elif c["type"] == "goto":
            target = self.pot_pos[c["pot"]] + self.cfg["nozzle_offset"]
            self.pos += math.copysign(min(move, abs(target - self.pos)), target - self.pos)
            if abs(target - self.pos) < 1:
                self._finish("At pot")
        elif c["type"] == "jog":
            target = c.setdefault("target", self.pos + c["steps"])
            self.pos = max(0.0, min(float(self.RAIL), self.pos + math.copysign(min(move * 0.3, abs(target - self.pos)), target - self.pos)))
            if abs(target - self.pos) < 1:
                self._finish("Jog done")
        elif c["type"] == "pump":
            if c["phase"] == 0 and now - c["t0"] > 0.5:
                if not c["force"] and not self._over_pot():
                    self._fail("No pot detected under the nozzle. Water skipped.")
                    return
                self._event("pump", c["pot"] + 1, c["secs"], self._pct(c["pot"]))
                self.pump_until = now + c["secs"]
                c["phase"] = 1
            elif c["phase"] == 1 and not self.pump_until:
                i = c["pot"]
                self.last_water[i], self.pending[i] = now, False
                self.count[i] += 1
                self._event("water", i + 1, c["secs"], self._pct(i))
                self.park_pending = True
                self._finish("Watered")

    def _tick(self) -> None:
        now = time.time()
        dt, self.last = min(now - self.last, 5.0), now
        while dt > 0:
            h = min(dt, 0.1)
            self._step(h)
            dt -= h

    def _queue_water(self, i: int, secs: int, force: bool = False) -> None:
        if not self.homed:
            self.queue.append(dict(type="home"))
        self.queue.append(dict(type="goto", pot=i))
        self.queue.append(dict(type="pump", pot=i, secs=secs, force=force))
        self.pending[i] = True

    SWEEP_PHASE_S = 2.0  # seconds per leg: +180, back, -180, back

    def _turntable(self) -> dict[str, Any]:
        if self.sweep_t0:
            t = time.time() - self.sweep_t0
            leg = int(t // self.SWEEP_PHASE_S)
            if leg < 4:
                stops = [0, 180, 0, -180, 0]
                frac = (t % self.SWEEP_PHASE_S) / self.SWEEP_PHASE_S
                self.turn_deg = self.sweep_center + round(stops[leg] + (stops[leg + 1] - stops[leg]) * frac)
                return dict(busy=True, sweeping=True, deg=self.turn_deg)
            self.sweep_t0 = 0.0
            self.turn_deg = self.sweep_center
        return dict(busy=False, sweeping=False, deg=self.turn_deg)

    # ----- same interface as Device -----
    def status(self) -> dict[str, Any]:
        with self.lock:
            self._tick()
            moving = self.state in ("homing", "moving", "calibrating")
            dist = -1.0 if moving else round((self.POT_CM if self._over_pot() else self.BASE_CM) + self.rng.gauss(0, 0.12), 1)
            pots = []
            for i in range(3):
                p = self.pots[i]
                raw = int(p["dry"] - (p["dry"] - p["wet"]) * self.moist[i] / 100.0)
                pots.append(dict(
                    id=i + 1, raw=raw, moisture=self._pct(i), sensor_ok=True, enabled=p["enabled"],
                    threshold=p["threshold"], water_secs=p["water_secs"], pos=self.pot_pos[i], dry=p["dry"],
                    wet=p["wet"], pending=self.pending[i],
                    last_water_s=-1 if self.last_water[i] is None else int(time.time() - self.last_water[i]),
                    count_today=self.count[i]))
            return dict(
                fw="mock", uptime_s=int(time.time() - self.boot), rssi=-52, ip="127.0.0.1", state=self.state,
                message=self.message, error=self.error, homed=self.homed, calibrated=self.calibrated,
                pos=int(self.pos), rail_steps=self.RAIL, ir_home=self.pos <= 0, ir_end=self.pos >= self.RAIL,
                distance_cm=dist, baseline_cm=self.BASE_CM, pump=self.pump_until > 0, auto=self.auto,
                queue=len(self.queue) + (1 if self.cur else 0), active_pot=(self.cur or {}).get("pot", -1) + 1,
                last_event_id=self._eid(), next_check_s=int(self.next_check - time.time()) if self.auto else -1,
                settings=dict(self.cfg), turntable=self._turntable(), pots=pots)

    def events(self, since: int) -> dict[str, Any]:
        with self.lock:
            return dict(events=[e for e in self.ev if e["id"] > since], last_id=self._eid())

    def scan(self) -> dict[str, Any]:
        rng = random.Random(3)
        step, n = 40, self.RAIL // 40
        dist = []
        for i in range(n):
            d = self.BASE_CM * 10
            if any(abs(i * step - p) < self.POT_HALF for p in self.pot_pos):
                d = self.POT_CM * 10
            d += rng.gauss(0, 1.4)
            dist.append(0 if rng.random() < 0.01 else int(d))
        pots = [dict(s=int((p - self.POT_HALF) / step), e=int((p + self.POT_HALF) / step)) for p in self.pot_pos]
        return dict(step=step, n=n, baseline_cm=self.BASE_CM, threshold_cm=self.cfg["delta_cm"],
                    pot_closer=True, found=3, pots=pots, dist=dist)

    def command(self, action: str, params: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            self._tick()
            self._turntable()  # lets a finished sweep clear itself
            if self.state == "error" and action not in ("stop",):
                self.state, self.error, self.message = "idle", "", "Idle"

            def pot() -> int:
                p = int(params.get("pot", 0))
                if p < 1 or p > 3:
                    raise DeviceError("pot must be 1, 2 or 3", 400)
                return p - 1

            if action == "home":
                self.queue.append(dict(type="home"))
                return dict(ok=True, msg="Homing queued")
            if action == "calibrate":
                self.queue += [dict(type="home"), dict(type="calibrate")]
                return dict(ok=True, msg="Calibration queued")
            if action == "goto":
                i = pot()
                self.queue.append(dict(type="goto", pot=i))
                return dict(ok=True, msg=f"Moving to pot {i + 1}")
            if action == "water":
                i = pot()
                if self.pending[i]:
                    raise DeviceError(f"Pot {i + 1} is already queued", 409)
                secs = int(params.get("secs") or self.pots[i]["water_secs"])
                self._queue_water(i, max(1, min(30, secs)), str(params.get("force")) == "1")
                return dict(ok=True, msg=f"Watering pot {i + 1} queued")
            if action == "check":
                n = 0
                for i in range(3):
                    if self.pots[i]["enabled"] and not self.pending[i] and self._pct(i) <= self.pots[i]["threshold"]:
                        self._queue_water(i, self.pots[i]["water_secs"])
                        n += 1
                return dict(ok=True, msg=f"{n} pot(s) queued" if n else "All pots are moist enough")
            if action == "stop":
                self.queue.clear()
                self.sweep_t0 = 0.0
                self.cur, self.pump_until = None, 0.0
                self.pending = [False] * 3
                self.state, self.message, self.error = "idle", "Stopped", ""
                return dict(ok=True, msg="Stopped")
            if action == "pump":
                self.pump_until = time.time() + max(1, min(10, int(params.get("secs", 2))))
                return dict(ok=True, msg="Pump test")
            if action == "jog":
                self.queue.append(dict(type="jog", steps=int(params.get("steps", 0))))
                return dict(ok=True, msg="Jog queued")
            if action == "auto":
                self.auto = str(params.get("on")) == "1"
                return dict(ok=True, msg="Auto watering " + ("ON" if self.auto else "OFF"))
            if action == "turntable":
                if self.sweep_t0:
                    raise DeviceError("Turntable is already moving", 409)
                self.turn_deg += int(params.get("deg", 0))
                return dict(ok=True, msg=f"Rotating {params.get('deg')} deg")
            if action == "sweep":
                if self.sweep_t0:
                    raise DeviceError("Turntable is already moving", 409)
                self.sweep_center, self.sweep_t0 = self.turn_deg, time.time()
                return dict(ok=True, msg="Turntable sweep started")
            if action == "soilcal":
                i = pot()
                key = "dry" if params.get("point") == "dry" else "wet"
                p = self.pots[i]
                p[key] = int(p["dry"] - (p["dry"] - p["wet"]) * self.moist[i] / 100.0)
                return dict(ok=True, msg=f"Saved {key} for pot {i + 1}")
            if action == "settings":
                for k in ("check_min", "soak_min", "nozzle_offset", "rail_speed", "scan_speed"):
                    if k in params:
                        self.cfg[k] = int(params[k])
                if "delta_cm" in params:
                    self.cfg["delta_cm"] = float(params["delta_cm"])
                for k in ("pot_closer", "park"):
                    if k in params:
                        self.cfg[k] = str(params[k]) == "1"
                for i in range(3):
                    if f"t{i + 1}" in params:
                        self.pots[i]["threshold"] = int(params[f"t{i + 1}"])
                    if f"s{i + 1}" in params:
                        self.pots[i]["water_secs"] = int(params[f"s{i + 1}"])
                    if f"e{i + 1}" in params:
                        self.pots[i]["enabled"] = str(params[f"e{i + 1}"]) == "1"
                return dict(ok=True, msg="Settings saved")
            raise DeviceError(f"Unknown action {action}", 404)
