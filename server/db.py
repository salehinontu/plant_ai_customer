"""Tiny SQLite store for moisture history, device events and diagnosis scans."""
import json
import sqlite3
import threading
import time
from typing import Any, Optional

from . import config


class DB:
    def __init__(self) -> None:
        config.DATA_DIR.mkdir(parents=True, exist_ok=True)
        config.SCANS_DIR.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(config.DB_PATH, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        with self.lock:
            self.conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS moisture (ts INTEGER NOT NULL, p1 INTEGER, p2 INTEGER, p3 INTEGER);
                CREATE INDEX IF NOT EXISTS moisture_ts ON moisture(ts);
                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, type TEXT, pot INTEGER,
                    secs INTEGER, moisture INTEGER, msg TEXT);
                CREATE TABLE IF NOT EXISTS scans (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL, source TEXT, pot INTEGER,
                    image TEXT, mime TEXT, result TEXT, health TEXT, plant TEXT, demo INTEGER);
                """
            )

    # ---- moisture ----
    def log_moisture(self, values: list[int]) -> None:
        v = (list(values) + [None, None, None])[:3]
        with self.lock, self.conn:
            self.conn.execute("INSERT INTO moisture VALUES (?,?,?,?)", (int(time.time()), *v))

    def moisture_series(self, hours: float, max_points: int = 400) -> list[dict[str, Any]]:
        since = int(time.time() - hours * 3600)
        with self.lock:
            rows = self.conn.execute(
                "SELECT ts,p1,p2,p3 FROM moisture WHERE ts>=? ORDER BY ts", (since,)
            ).fetchall()
        step = max(1, -(-len(rows) // max_points))
        return [{"ts": r["ts"], "p": [r["p1"], r["p2"], r["p3"]]} for r in rows[::step]]

    # ---- events ----
    def add_event(self, ts: int, type_: str, pot: int, secs: int, moisture: int, msg: str) -> None:
        with self.lock, self.conn:
            self.conn.execute(
                "INSERT INTO events (ts,type,pot,secs,moisture,msg) VALUES (?,?,?,?,?,?)",
                (ts, type_, pot, secs, moisture, msg),
            )

    def list_events(self, limit: int = 50) -> list[dict[str, Any]]:
        with self.lock:
            rows = self.conn.execute("SELECT * FROM events ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        return [dict(r) for r in rows]

    # ---- scans ----
    def add_scan(self, source: str, pot: Optional[int], image: str, mime: str, result: dict, demo: bool) -> int:
        plant = result.get("plant_common_name", "")
        health = result.get("health_status", "unknown")
        with self.lock, self.conn:
            cur = self.conn.execute(
                "INSERT INTO scans (ts,source,pot,image,mime,result,health,plant,demo) VALUES (?,?,?,?,?,?,?,?,?)",
                (int(time.time()), source, pot, image, mime, json.dumps(result), health, plant, int(demo)),
            )
            return int(cur.lastrowid)

    @staticmethod
    def _scan_row(r: sqlite3.Row) -> dict[str, Any]:
        return {
            "id": r["id"],
            "ts": r["ts"],
            "source": r["source"],
            "pot": r["pot"],
            "image_url": f"/api/scans/{r['id']}/image",
            "result": json.loads(r["result"]),
            "demo": bool(r["demo"]),
        }

    def list_scans(self, limit: int = 24) -> list[dict[str, Any]]:
        with self.lock:
            rows = self.conn.execute("SELECT * FROM scans ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        return [self._scan_row(r) for r in rows]

    def get_scan(self, scan_id: int) -> Optional[sqlite3.Row]:
        with self.lock:
            return self.conn.execute("SELECT * FROM scans WHERE id=?", (scan_id,)).fetchone()

    def scan_dict(self, scan_id: int) -> Optional[dict[str, Any]]:
        row = self.get_scan(scan_id)
        return self._scan_row(row) if row else None
