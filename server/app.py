"""Plant Rail server: serves the website, talks to the ESP32 controller / ESP32-CAM and runs the AI diagnosis.

Run from the repo root:   python -m server
"""
import logging
import mimetypes
import threading
import time
from contextlib import asynccontextmanager
from typing import Any, Optional

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles

from . import ai, config
from .db import DB
from .devices import Camera, Device, DeviceError, MockCamera, MockDevice

log = logging.getLogger("plantrail")

db = DB()
device: Any = MockDevice() if config.MOCK else Device(config.ESP32_URL, config.ESP32_API_KEY)
camera: Any = MockCamera() if config.MOCK else Camera(config.CAM_URL)

ACTIONS = {"home", "calibrate", "goto", "water", "check", "stop", "pump", "jog", "auto", "turntable",
           "sweep", "teach", "soilcal", "settings"}
ALLOWED_IMAGES = {"image/jpeg", "image/png", "image/webp", "image/gif"}

STATE: dict[str, Any] = {"device": None, "online": False, "fails": 0, "ts": 0.0, "cam_online": None,
                         "last_event": None}
scan_lock = threading.Lock()


# ---------------------------------------------------------------------------
#  Background poller: keeps the latest controller status, logs moisture and events
# ---------------------------------------------------------------------------
def ingest_events(st: dict[str, Any]) -> None:
    last_id = st.get("last_event_id", 0)
    if STATE["last_event"] is None or last_id < STATE["last_event"]:  # first contact, or the controller rebooted
        STATE["last_event"] = last_id
        return
    if last_id == STATE["last_event"]:
        return
    data = device.events(STATE["last_event"])
    uptime = st.get("uptime_s", 0)
    now = time.time()
    for e in data.get("events", []):
        ts = int(now - max(0, uptime - e["t"]))
        db.add_event(ts, e["type"], e["pot"], e["secs"], e["moisture"], e.get("msg", ""))
    STATE["last_event"] = data.get("last_id", last_id)


def run_diagnosis(image: bytes, mime: str, source: str, pot: Optional[int]) -> dict[str, Any]:
    context: dict[str, Any] = {"pot": pot}
    dev = STATE["device"]
    if pot and dev:
        context["moisture"] = next((p["moisture"] for p in dev.get("pots", []) if p["id"] == pot), None)
    try:
        # the simulator's cartoon leaf is not a photo - never spend API credits on it
        result, demo = ai.diagnose(image, mime, context, force_demo=mime == "image/svg+xml")
    except ai.AIError as e:
        raise HTTPException(502, str(e)) from e
    name = f"{int(time.time() * 1000)}{mimetypes.guess_extension(mime) or '.jpg'}"
    (config.SCANS_DIR / name).write_bytes(image)
    scan_id = db.add_scan(source, pot, name, mime, result, demo)
    return db.scan_dict(scan_id) or {}


def poll_loop() -> None:
    last_log = 0.0
    last_auto = time.time()
    while True:
        try:
            st = device.status()
            STATE.update(device=st, online=True, fails=0, ts=time.time())
            ingest_events(st)
            if time.time() - last_log >= 60:
                db.log_moisture([p["moisture"] for p in st.get("pots", [])])
                last_log = time.time()
        except Exception as e:  # noqa: BLE001 - the poller must never die
            STATE["fails"] += 1
            if STATE["fails"] >= 3:
                STATE["online"] = False
            log.debug("poll failed: %s", e)

        if config.AUTO_SCAN_HOURS > 0 and time.time() - last_auto >= config.AUTO_SCAN_HOURS * 3600:
            last_auto = time.time()
            try:
                img, mime = camera.snapshot(hq=True)
                with scan_lock:
                    run_diagnosis(img, mime.split(";")[0], "auto", None)
            except Exception as e:  # noqa: BLE001
                log.warning("automatic scan failed: %s", e)
        time.sleep(2.0)


@asynccontextmanager
async def lifespan(_: FastAPI):
    threading.Thread(target=poll_loop, daemon=True, name="poller").start()
    mode = "SIMULATED hardware" if config.MOCK else f"controller {config.ESP32_URL}"
    log.warning("Plant Rail ready - %s | AI: %s", mode, f"{config.CLAUDE_MODEL}" if ai.has_key() else "demo mode (no API key)")
    yield


app = FastAPI(title="Plant Rail", lifespan=lifespan)


# ---------------------------------------------------------------------------
#  API
# ---------------------------------------------------------------------------
@app.exception_handler(DeviceError)
async def device_error_handler(_, exc: DeviceError):
    return JSONResponse({"detail": str(exc)}, status_code=exc.status)


@app.get("/api/status")
def api_status():
    return {
        "online": STATE["online"],
        "age_s": round(time.time() - STATE["ts"], 1) if STATE["ts"] else None,
        "device": STATE["device"],
        "cam": {"online": STATE["cam_online"], "stream_url": None if config.MOCK else config.CAM_STREAM_URL},
        "ai": {"enabled": ai.has_key(), "model": config.CLAUDE_MODEL, "demo": not ai.has_key()},
        "mock": config.MOCK,
        "auto_scan_hours": config.AUTO_SCAN_HOURS,
        "server_time": int(time.time()),
    }


@app.post("/api/device/{action}")
def api_device(action: str, params: dict[str, Any] | None = None):
    if action not in ACTIONS:
        raise HTTPException(404, f"Unknown action '{action}'")
    reply = device.command(action, params or {})
    try:  # refresh immediately so the website reacts without waiting for the next poll
        STATE.update(device=device.status(), online=True, fails=0, ts=time.time())
    except DeviceError:
        pass
    return reply


@app.get("/api/device/scan")
def api_scan():
    return device.scan()


@app.get("/api/camera/snapshot")
def api_snapshot(hq: int = 0, flash: int = 0):
    try:
        img, mime = camera.snapshot(hq=bool(hq), flash=bool(flash))
    except DeviceError:
        STATE["cam_online"] = False
        raise
    STATE["cam_online"] = True
    return Response(img, media_type=mime.split(";")[0], headers={"Cache-Control": "no-store"})


@app.post("/api/diagnose")
def api_diagnose(pot: Optional[int] = Form(None), file: Optional[UploadFile] = File(None)):
    if pot is not None and pot not in (1, 2, 3):
        raise HTTPException(400, "pot must be 1, 2 or 3")
    if not scan_lock.acquire(blocking=False):
        raise HTTPException(409, "A scan is already running")
    try:
        if file is not None and file.filename:
            mime = (file.content_type or "").split(";")[0]
            if mime not in ALLOWED_IMAGES:
                raise HTTPException(415, "Please upload a JPEG, PNG, WebP or GIF image")
            img = file.file.read(config.MAX_IMAGE_BYTES + 1)
            source = "upload"
        else:
            img, mime = camera.snapshot(hq=True)
            mime = mime.split(";")[0]
            source = "camera"
            STATE["cam_online"] = True
        if len(img) > config.MAX_IMAGE_BYTES:
            raise HTTPException(413, "Image is larger than 5 MB - please use a smaller photo")
        return run_diagnosis(img, mime, source, pot)
    finally:
        scan_lock.release()


@app.get("/api/scans")
def api_scans(limit: int = 24):
    return {"scans": db.list_scans(max(1, min(limit, 100)))}


@app.get("/api/scans/{scan_id}/image")
def api_scan_image(scan_id: int):
    row = db.get_scan(scan_id)
    if not row:
        raise HTTPException(404, "No such scan")
    path = config.SCANS_DIR / row["image"]
    if not path.is_file():
        raise HTTPException(404, "Image file is missing")
    return FileResponse(path, media_type=row["mime"])


@app.get("/api/history/moisture")
def api_moisture(hours: float = 24):
    return {"points": db.moisture_series(max(0.1, min(hours, 24 * 30)))}


@app.get("/api/history/events")
def api_events(limit: int = 60):
    return {"events": db.list_events(max(1, min(limit, 300)))}


# website (must be mounted last so it does not shadow /api)
app.mount("/", StaticFiles(directory=config.WEB_DIR, html=True), name="web")
