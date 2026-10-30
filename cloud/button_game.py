"""按键互动游戏的跨进程状态机（SQLite）。

MCP Server 创建/取消游戏，MQTT 消费者读取并判定真实按键事件。
只使用固定反馈枚举，不接受任意文本或任意设备动作。
"""

import os
import random
import sqlite3
import time
import uuid
from datetime import datetime, timezone

BUTTON_COUNT = 4
BUTTON_LABELS = ("KEY0", "KEY1", "KEY2", "KEY3")
FEEDBACKS = ("correct", "wrong", "timeout")
FEEDBACK_TEXT = {
    "correct": "你真棒，按对啦！",
    "wrong": "没关系，再试一次！",
    "timeout": "我们下次继续玩吧！",
}
MIN_TIMEOUT_S = 15
MAX_TIMEOUT_S = 120
DEFAULT_TIMEOUT_S = 60
DEFAULT_DB = os.path.join(os.path.dirname(__file__), "state", "button_game.db")

STATE_WAITING = "WAITING_FOR_BUTTON"
STATE_COMPLETED = "COMPLETED"
STATE_EXPIRED = "EXPIRED"
STATE_CANCELLED = "CANCELLED"
STATE_SUPERSEDED = "SUPERSEDED"

SCHEMA = """
CREATE TABLE IF NOT EXISTS games (
    game_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    target_button INTEGER NOT NULL,
    state TEXT NOT NULL,
    created_at REAL NOT NULL,
    expires_at REAL NOT NULL,
    device_uptime_at_start INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    result TEXT,
    updated_at REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_games_waiting
    ON games(device_id, state, created_at DESC);
CREATE TABLE IF NOT EXISTS game_events (
    game_id TEXT NOT NULL,
    event_key TEXT NOT NULL,
    processed_at REAL NOT NULL,
    outcome TEXT NOT NULL,
    PRIMARY KEY (game_id, event_key)
);
"""


def _iso(timestamp):
    if not timestamp:
        return None
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).astimezone(
    ).isoformat(timespec="seconds")


class ButtonGameStore:
    def __init__(self, path=None, clock=None, random_source=None):
        self.path = path or os.environ.get("SMARTGUARD_GAME_DB") or DEFAULT_DB
        self.clock = clock or time.time
        self.random = random_source or random.SystemRandom()
        directory = os.path.dirname(self.path)
        if directory:
            os.makedirs(directory, exist_ok=True)
        self._ensure_schema()

    def _connect(self):
        conn = sqlite3.connect(self.path, timeout=3.0)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA busy_timeout=3000")
        return conn

    def _ensure_schema(self):
        conn = self._connect()
        try:
            conn.execute("PRAGMA journal_mode=WAL")
            conn.executescript(SCHEMA)
            conn.execute("PRAGMA user_version=1")
            conn.commit()
        finally:
            conn.close()

    def start(self, device_id, target_button=None, timeout_seconds=DEFAULT_TIMEOUT_S,
              device_uptime_ms=0, now=None):
        if not isinstance(device_id, str) or not device_id:
            raise ValueError("device_id_required")
        if target_button is None:
            target_button = self.random.randrange(BUTTON_COUNT)
        if not isinstance(target_button, int) or isinstance(target_button, bool) \
                or target_button < 0 or target_button >= BUTTON_COUNT:
            raise ValueError("target_button_out_of_range")
        if not isinstance(timeout_seconds, int) or isinstance(timeout_seconds, bool) \
                or timeout_seconds < MIN_TIMEOUT_S or timeout_seconds > MAX_TIMEOUT_S:
            raise ValueError("timeout_out_of_range")

        now = self.clock() if now is None else now
        game_id = "btn-" + uuid.uuid4().hex[:12]
        expires_at = now + timeout_seconds
        conn = self._connect()
        try:
            conn.execute("BEGIN IMMEDIATE")
            conn.execute(
                "UPDATE games SET state=?, updated_at=? "
                "WHERE device_id=? AND state=?",
                (STATE_SUPERSEDED, now, device_id, STATE_WAITING))
            conn.execute(
                "INSERT INTO games (game_id, device_id, target_button, state, "
                "created_at, expires_at, device_uptime_at_start, attempts, "
                "result, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, 0, NULL, ?)",
                (game_id, device_id, target_button, STATE_WAITING, now,
                 expires_at, int(device_uptime_ms or 0), now))
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()
        row = self._get_game(game_id)
        return self._public(row)

    def cancel(self, device_id=None, game_id=None, now=None):
        now = self.clock() if now is None else now
        conn = self._connect()
        try:
            conn.execute("BEGIN IMMEDIATE")
            if game_id:
                row = conn.execute(
                    "SELECT * FROM games WHERE game_id=? AND state=?",
                    (game_id, STATE_WAITING)).fetchone()
            elif device_id:
                row = conn.execute(
                    "SELECT * FROM games WHERE device_id=? AND state=? "
                    "ORDER BY created_at DESC, rowid DESC LIMIT 1",
                    (device_id, STATE_WAITING)).fetchone()
            else:
                row = conn.execute(
                    "SELECT * FROM games WHERE state=? "
                    "ORDER BY created_at DESC, rowid DESC LIMIT 1",
                    (STATE_WAITING,)).fetchone()
            if row is None:
                conn.commit()
                return {"cancelled": False, "reason": "no_active_game"}
            conn.execute(
                "UPDATE games SET state=?, updated_at=? WHERE game_id=?",
                (STATE_CANCELLED, now, row["game_id"]))
            conn.commit()
            game = self._public(row)
            game["state"] = STATE_CANCELLED
            return {"cancelled": True, "game": game}
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()

    def expire_due(self, now=None):
        """把所有到期游戏标记为 EXPIRED，每个游戏只返回一次 timeout 反馈。"""
        now = self.clock() if now is None else now
        conn = self._connect()
        expired = []
        try:
            conn.execute("BEGIN IMMEDIATE")
            rows = conn.execute(
                "SELECT * FROM games WHERE state=? AND expires_at<=?",
                (STATE_WAITING, now)).fetchall()
            for row in rows:
                conn.execute(
                    "UPDATE games SET state=?, result='TIMEOUT', updated_at=? "
                    "WHERE game_id=?",
                    (STATE_EXPIRED, now, row["game_id"]))
                game = self._public(row)
                game.update(state=STATE_EXPIRED, result="TIMEOUT",
                            feedback="timeout")
                expired.append(game)
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()
        return expired

    def handle_key(self, device_id, event_key, key_index, event_uptime_ms,
                   now=None):
        """处理一次真实按键；返回 feedback 时表示应发布一次固定语音。"""
        if not isinstance(key_index, int) or isinstance(key_index, bool) \
                or key_index < 0 or key_index >= BUTTON_COUNT:
            return {"handled": False, "reason": "invalid_key_index"}
        if not event_key:
            return {"handled": False, "reason": "missing_event_key"}

        now = self.clock() if now is None else now
        conn = self._connect()
        try:
            conn.execute("BEGIN IMMEDIATE")
            row = conn.execute(
                "SELECT * FROM games WHERE device_id=? AND state=? "
                "ORDER BY created_at DESC, rowid DESC LIMIT 1",
                (device_id, STATE_WAITING)).fetchone()
            if row is None:
                conn.commit()
                return {"handled": False, "reason": "no_active_game"}

            if now >= row["expires_at"]:
                conn.execute(
                    "UPDATE games SET state=?, result='TIMEOUT', updated_at=? "
                    "WHERE game_id=?",
                    (STATE_EXPIRED, now, row["game_id"]))
                conn.commit()
                game = self._public(row)
                game.update(state=STATE_EXPIRED, result="TIMEOUT",
                            feedback="timeout")
                return {"handled": False, "reason": "game_expired",
                        "feedback": "timeout", "game": game}

            try:
                conn.execute(
                    "INSERT INTO game_events (game_id, event_key, processed_at, "
                    "outcome) VALUES (?, ?, ?, ?)",
                    (row["game_id"], str(event_key), now, "pending"))
            except sqlite3.IntegrityError:
                conn.commit()
                return {"handled": False, "reason": "duplicate_event"}

            start_uptime = int(row["device_uptime_at_start"] or 0)
            try:
                uptime = int(event_uptime_ms)
            except (TypeError, ValueError):
                uptime = None
            if start_uptime > 0 and (uptime is None or
                                     uptime < start_uptime - 1000):
                conn.execute(
                    "UPDATE game_events SET outcome='stale' "
                    "WHERE game_id=? AND event_key=?",
                    (row["game_id"], str(event_key)))
                conn.commit()
                return {"handled": False, "reason": "stale_event"}

            target = int(row["target_button"])
            if key_index == target:
                conn.execute(
                    "UPDATE games SET state=?, result='CORRECT', updated_at=? "
                    "WHERE game_id=?",
                    (STATE_COMPLETED, now, row["game_id"]))
                conn.execute(
                    "UPDATE game_events SET outcome='correct' "
                    "WHERE game_id=? AND event_key=?",
                    (row["game_id"], str(event_key)))
                outcome = "correct"
                state = STATE_COMPLETED
            else:
                conn.execute(
                    "UPDATE games SET attempts=attempts+1, updated_at=? "
                    "WHERE game_id=?",
                    (now, row["game_id"]))
                conn.execute(
                    "UPDATE game_events SET outcome='wrong' "
                    "WHERE game_id=? AND event_key=?",
                    (row["game_id"], str(event_key)))
                outcome = "wrong"
                state = STATE_WAITING
            conn.commit()
            game = self._public(row)
            game.update(state=state, attempts=int(row["attempts"]) +
                        (1 if outcome == "wrong" else 0))
            return {
                "handled": True,
                "game_id": row["game_id"],
                "outcome": outcome,
                "feedback": outcome,
                "feedback_text": FEEDBACK_TEXT[outcome],
                "target_button": target,
                "target_label": BUTTON_LABELS[target],
                "key_index": key_index,
                "key_label": BUTTON_LABELS[key_index],
                "state": state,
                "game": game,
            }
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()

    def latest(self, device_id=None):
        conn = self._connect()
        try:
            if device_id:
                row = conn.execute(
                    "SELECT * FROM games WHERE device_id=? "
                    "ORDER BY created_at DESC, rowid DESC LIMIT 1",
                    (device_id,)).fetchone()
            else:
                row = conn.execute(
                    "SELECT * FROM games "
                    "ORDER BY created_at DESC, rowid DESC LIMIT 1"
                ).fetchone()
        finally:
            conn.close()
        return None if row is None else self._public(row)

    def _get_game(self, game_id):
        conn = self._connect()
        try:
            row = conn.execute("SELECT * FROM games WHERE game_id=?",
                               (game_id,)).fetchone()
        finally:
            conn.close()
        if row is None:
            raise RuntimeError("game_not_found")
        return row

    @staticmethod
    def _public(row):
        target = int(row["target_button"])
        label = BUTTON_LABELS[target]
        return {
            "game_id": row["game_id"],
            "device_id": row["device_id"],
            "target_button": target,
            "target_label": label,
            "state": row["state"],
            "attempts": int(row["attempts"]),
            "result": row["result"],
            "timeout_seconds": round(float(row["expires_at"]) -
                                     float(row["created_at"]), 1),
            "created_at": _iso(row["created_at"]),
            "expires_at": _iso(row["expires_at"]),
            "instruction": f"我们来玩个小游戏吧，请按一下按键 {target}"
                           f"（{label}）。",
            "feedback_text": {
                "correct": FEEDBACK_TEXT["correct"],
                "wrong": FEEDBACK_TEXT["wrong"],
                "timeout": FEEDBACK_TEXT["timeout"],
            },
        }
