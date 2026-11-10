"""Server settings, read from environment variables / the .env file in the repo root."""
import os
from pathlib import Path

from dotenv import load_dotenv

ROOT = Path(__file__).resolve().parent.parent
load_dotenv(ROOT / ".env")


def env(name: str, default: str = "") -> str:
    return os.environ.get(name, default).strip()


ESP32_URL = env("ESP32_URL", "http://plantrail.local").rstrip("/")
ESP32_API_KEY = env("ESP32_API_KEY")
CAM_URL = env("CAM_URL", "http://plantcam.local").rstrip("/")
CAM_STREAM_URL = env("CAM_STREAM_URL", "http://plantcam.local:81/stream")

ANTHROPIC_API_KEY = env("ANTHROPIC_API_KEY")
CLAUDE_MODEL = env("CLAUDE_MODEL", "claude-opus-5-5")
AI_LANG = "en" if env("AI_LANGUAGE", "bn").lower() in ("en", "english") else "bn"  # default answer language
AUTO_SCAN_HOURS = float(env("AUTO_SCAN_HOURS", "0") or 0)

HOST = env("HOST", "0.0.0.0")
PORT = int(env("PORT", "8000") or 8000)
MOCK = env("MOCK_DEVICE", "0") == "1"

WEB_DIR = ROOT / "web"
DATA_DIR = ROOT / "data"
# the simulator keeps its own history, so demo data never mixes with real readings
SCANS_DIR = DATA_DIR / ("scans_demo" if MOCK else "scans")
DB_PATH = DATA_DIR / ("demo.db" if MOCK else "plantrail.db")

MAX_IMAGE_BYTES = 5 * 1024 * 1024  # Claude's per-image limit
