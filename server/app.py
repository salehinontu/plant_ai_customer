"""Plant Rail server: serves the website, talks to the ESP32 controller / ESP32-CAM and runs the AI diagnosis.

Run from the repo root:   python -m server
"""
import json
import logging
import mimetypes
import threading
import time
from contextlib import asynccontextmanager
from typing import Any, Optional

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import ai, config
from .db import DB
from .devices import Camera, CameraFeed, Device, DeviceError, MockCamera, MockDevice

log = logging.getLogger("plantrail")

db = DB()
device: Any = MockDevice() if config.MOCK else Device(config.ESP32_URL, config.ESP32_API_KEY)
camera: Any = MockCamera() if config.MOCK else Camera(config.CAM_URL)
feed = CameraFeed(camera)
if config.MOCK:
    feed.MIN_GAP_S = 1.0  # the simulator picture never changes

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


PROGRESS: dict[str, Any] = {"running": False, "step": 0, "total": 0, "text": "", "error": ""}


def run_diagnosis(photos: list[tuple[bytes, str, str]], source: str, pot: Optional[int], note: str = "",
                  lang: str = config.AI_LANG) -> dict[str, Any]:
    """photos = [(bytes, mime, label), ...]. Asks the AI, saves the photos and the result, returns the scan."""
    context: dict[str, Any] = {"pot": pot}
    dev = STATE["device"]
    if pot and dev:
        context["moisture"] = next((p["moisture"] for p in dev.get("pots", []) if p["id"] == pot), None)
    try:
        # the simulator's cartoon leaf is not a photo - never spend API credits on it
        result, demo = ai.diagnose(photos, context, note, lang,
                                   force_demo=any(m == "image/svg+xml" for _, m, _ in photos))
    except ai.AIError as e:
        raise HTTPException(502, str(e)) from e
    stamp = int(time.time() * 1000)
    names = []
    for i, (img, mime, _) in enumerate(photos):
        name = f"{stamp}_{i}{mimetypes.guess_extension(mime) or '.jpg'}"
        (config.SCANS_DIR / name).write_bytes(img)
        names.append(name)
    meta = {"images": names, "mimes": [m for _, m, _ in photos], "labels": [lb for _, _, lb in photos],
            "note": note.strip(), "lang": lang}
    scan_id = db.add_scan(source, pot, names[0], photos[0][1], result, demo, meta)
    return db.scan_dict(scan_id) or {}


# The plant on the turntable is looked at from four sides while the turntable does
# +180 deg, back to centre, -180 deg, back to centre (the cable never twists more than half a turn).
# (label, degrees to rotate first, take a photo afterwards?)
SWEEP_PLAN = [
    ("front (0°)", 0, True),
    ("turned +90°", 90, True),
    ("turned +180° (back)", 90, True),
    ("back to centre", -180, False),
    ("turned -90°", -90, True),
    ("turned -180°", -90, False),
    ("back to centre", 180, False),
]


def wait_turntable(timeout: float = 40.0) -> None:
    time.sleep(0.4)
    deadline = time.time() + timeout
    while time.time() < deadline:
        tt = (device.status() or {}).get("turntable") or {}
        if not tt.get("busy"):
            return
        time.sleep(0.4)
    raise DeviceError("The turntable did not stop moving in time", 504)


def sweep_scan(pot: Optional[int], note: str, lang: str) -> dict[str, Any]:
    photos: list[tuple[bytes, str, str]] = []
    turned = 0
    total = len(SWEEP_PLAN)
    PROGRESS.update(running=True, step=0, total=total, text="Starting", error="")
    try:
        for i, (label, deg, shoot) in enumerate(SWEEP_PLAN, 1):
            PROGRESS.update(step=i, text=f"{i}/{total}: {label}")
            if deg:
                device.command("turntable", {"deg": deg})
                turned += deg
                wait_turntable()
            if shoot:
                time.sleep(0.8)  # let the plant and the camera exposure settle
                img, mime = camera.snapshot(hq=True)
                photos.append((img, mime.split(";")[0], label))
        STATE["cam_online"] = True
        PROGRESS.update(step=total, text="Asking the AI…")
        return run_diagnosis(photos, "sweep", pot, note, lang)
    except Exception as e:  # noqa: BLE001
        PROGRESS["error"] = str(getattr(e, "detail", e))
        if turned:  # best effort: bring the turntable back to where it started
            try:
                device.command("turntable", {"deg": -turned})
                wait_turntable()
            except Exception:  # noqa: BLE001
                pass
        raise
    finally:
        PROGRESS["running"] = False


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
                    run_diagnosis([(img, mime.split(";")[0], "automatic photo")], "auto", None)
            except Exception as e:  # noqa: BLE001
                log.warning("automatic scan failed: %s", e)
        time.sleep(2.0)


@asynccontextmanager
async def lifespan(_: FastAPI):
    threading.Thread(target=poll_loop, daemon=True, name="poller").start()
    mode = "SIMULATED hardware" if config.MOCK else f"controller {config.ESP32_URL}"
    log.warning("Plant Rail ready - %s | AI: %s", mode, f"{ai.provider()} / {ai.model_name()}" if ai.has_key() else "demo mode (no API key)")
    yield


app = FastAPI(title="Plant Rail", lifespan=lifespan)


# ---------------------------------------------------------------------------
#  API
# ---------------------------------------------------------------------------
@app.exception_handler(DeviceError)
async def device_error_handler(_, exc: DeviceError):
    return JSONResponse({"detail": str(exc)}, status_code=exc.status)


@app.middleware("http")
async def always_fresh_site(request, call_next):
    """Make the browser re-check the website files on every load, so a new version is never mixed with an old one."""
    response = await call_next(request)
    if not request.url.path.startswith("/api/"):
        response.headers["Cache-Control"] = "no-cache"
    return response


@app.get("/api/status")
def api_status():
    return {
        "online": STATE["online"],
        "age_s": round(time.time() - STATE["ts"], 1) if STATE["ts"] else None,
        "device": STATE["device"],
        "cam": {"online": STATE["cam_online"], "stream_url": None if config.MOCK else config.CAM_STREAM_URL},
        "ai": {"enabled": ai.has_key(), "provider": ai.provider(), "model": ai.model_name(), "demo": not ai.has_key()},
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
def api_snapshot(hq: int = 0, flash: int = 0, after: int = 0):
    """Preview frame. Normally the cached newest frame (fast); `after=N` waits for a frame newer than id N.
    hq / flash take a fresh full-quality picture straight from the camera."""
    try:
        if hq or flash:
            img, mime = camera.snapshot(hq=bool(hq), flash=bool(flash))
            frame_id = 0
        else:
            img, mime, frame_id = feed.get(after)
    except DeviceError:
        STATE["cam_online"] = False
        raise
    STATE["cam_online"] = True
    return Response(img, media_type=mime.split(";")[0],
                    headers={"Cache-Control": "no-store", "X-Frame-Id": str(frame_id)})


def pick_lang(lang: Optional[str]) -> str:
    return "en" if (lang or config.AI_LANG) == "en" else "bn"


@app.post("/api/diagnose")
def api_diagnose(pot: Optional[int] = Form(None), note: str = Form(""), lang: Optional[str] = Form(None),
                 file: Optional[UploadFile] = File(None)):
    """One photo (from the camera, or uploaded) + the grower's note, in Bangla or English."""
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
            source, label = "upload", "photo uploaded by the grower"
        else:
            img, mime = camera.snapshot(hq=True)
            mime = mime.split(";")[0]
            source, label = "camera", "camera photo"
            STATE["cam_online"] = True
        if len(img) > config.MAX_IMAGE_BYTES:
            raise HTTPException(413, "Image is larger than 5 MB - please use a smaller photo")
        return run_diagnosis([(img, mime, label)], source, pot, note, pick_lang(lang))
    finally:
        scan_lock.release()


@app.post("/api/scan/sweep")
def api_sweep(pot: Optional[int] = Form(2), note: str = Form(""), lang: Optional[str] = Form(None)):
    """Turn the plant through +180 / -180 deg, photograph it from four sides and diagnose it."""
    if pot is not None and pot not in (1, 2, 3):
        raise HTTPException(400, "pot must be 1, 2 or 3")
    if not scan_lock.acquire(blocking=False):
        raise HTTPException(409, "A scan is already running")
    try:
        return sweep_scan(pot, note, pick_lang(lang))
    finally:
        scan_lock.release()


@app.get("/api/scan/progress")
def api_progress():
    return dict(PROGRESS)


class AskBody(BaseModel):
    question: str
    scan_id: Optional[int] = None
    lang: Optional[str] = None


@app.post("/api/ask")
def api_ask(body: AskBody):
    """Follow-up question about a diagnosis (typed or dictated, Bangla or English)."""
    if not body.question.strip():
        raise HTTPException(400, "Please write or say a question")
    if body.scan_id:
        row = db.get_scan(body.scan_id)
    else:
        latest = db.list_scans(1)
        row = db.get_scan(latest[0]["id"]) if latest else None
    if not row:
        raise HTTPException(404, "Scan a plant first, then ask about the result")
    try:
        answer, demo = ai.ask(body.question, json.loads(row["result"]), pick_lang(body.lang), force_demo=config.MOCK)
    except ai.AIError as e:
        raise HTTPException(502, str(e)) from e
    return {"answer": answer, "demo": demo}


@app.get("/api/scans")
def api_scans(limit: int = 24):
    return {"scans": db.list_scans(max(1, min(limit, 100)))}


@app.get("/api/scans/{scan_id}/image")
def api_scan_image(scan_id: int, i: int = 0):
    row = db.get_scan(scan_id)
    if not row:
        raise HTTPException(404, "No such scan")
    meta = db.scan_meta(row)
    names = meta.get("images") or [row["image"]]
    mimes = meta.get("mimes") or [row["mime"]]
    if i < 0 or i >= len(names):
        raise HTTPException(404, "No such photo")
    path = config.SCANS_DIR / names[i]
    if not path.is_file():
        raise HTTPException(404, "Image file is missing")
    return FileResponse(path, media_type=mimes[i])


@app.get("/api/history/moisture")
def api_moisture(hours: float = 24):
    return {"points": db.moisture_series(max(0.1, min(hours, 24 * 30)))}


@app.get("/api/history/events")
def api_events(limit: int = 60):
    return {"events": db.list_events(max(1, min(limit, 300)))}


# website (must be mounted last so it does not shadow /api)
app.mount("/", StaticFiles(directory=config.WEB_DIR, html=True), name="web")
