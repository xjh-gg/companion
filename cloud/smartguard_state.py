"""SmartGuard MCP 只读查询：读取消费者写入的本地快照。

约束：
- 不启动 MQTT 连接，不发布命令，不操作 GPIO。
- 没有缓存时明确返回不可用，不伪造设备状态。
- 输出保持紧凑，尽量不超过 1024 字节。
"""

import json
import os
import time
from datetime import datetime, timezone

SCHEMA_VERSION = 1
STATUS_FRESH_S = 30.0
CACHE_STALE_S = 60.0
MAX_EVENT_LIMIT = 10
MAX_RESULT_BYTES = 1024
DEFAULT_STATE_FILE = os.path.join(os.path.dirname(__file__), "state",
                                  "smartguard_snapshot.json")

DATA_SOURCE = "smartguard_consumer_cache"
LIMITATIONS = ("仅反映 MQTT 消费者最近一次缓存，不直接连接设备；"
               "过期数据以 age_seconds/fresh 字段标记，不保证实时。")


class SnapshotUnavailable(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def state_file_path():
    return (os.environ.get("SMARTGUARD_STATE_FILE") or DEFAULT_STATE_FILE)


def _read_snapshot(path=None):
    path = path or state_file_path()
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except FileNotFoundError:
        raise SnapshotUnavailable("no_cache_file")
    except (OSError, ValueError):
        raise SnapshotUnavailable("cache_invalid")
    if not isinstance(data, dict) or data.get("schema_version") != SCHEMA_VERSION:
        raise SnapshotUnavailable("cache_invalid")
    if not isinstance(data.get("devices"), dict):
        raise SnapshotUnavailable("cache_invalid")
    if not isinstance(data.get("generated_at"), (int, float)):
        raise SnapshotUnavailable("cache_invalid")
    return data


def _error(reason, device_id=None, snapshot=None):
    result = {
        "available": False,
        "reason": reason,
        "data_source": DATA_SOURCE,
        "limitations": LIMITATIONS,
    }
    if device_id:
        result["device_id"] = device_id
    if snapshot is not None:
        result["cache_generated_at"] = _iso(snapshot.get("generated_at"))
        result["cache_age_seconds"] = _age(snapshot.get("generated_at"))
    return result


def _iso(timestamp):
    if not timestamp or timestamp <= 0:
        return None
    # Windows 的本地时间转换对 1970 附近时间戳可能抛 OSError，先按 UTC 再转本地。
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).astimezone(
    ).isoformat(timespec="seconds")


def _age(timestamp, now=None):
    if not timestamp or timestamp <= 0:
        return None
    now = time.time() if now is None else now
    return round(max(0.0, now - timestamp), 1)


def _json_bytes(payload):
    return len(json.dumps(payload, ensure_ascii=False,
                          separators=(",", ":")).encode("utf-8"))


def _last_event_received(entry):
    events = (entry or {}).get("events") or []
    if not events or not isinstance(events[0], dict):
        return 0.0
    return float(events[0].get("received_at") or 0.0)


def _select_device(snapshot, device_id=None):
    devices = snapshot["devices"]
    if device_id:
        if device_id not in devices:
            return None, None, "device_not_found"
        return device_id, devices[device_id], None
    if not devices:
        return None, None, "no_cached_devices"
    best_id = max(
        devices,
        key=lambda key: (
            float(((devices[key] or {}).get("status_received_at") or 0)),
            _last_event_received(devices[key]),
        ),
    )
    return best_id, devices[best_id], None


def query_status(device_id=None, now=None, path=None):
    """返回最近一次缓存的设备状态摘要。"""
    now = time.time() if now is None else now
    try:
        snapshot = _read_snapshot(path)
    except SnapshotUnavailable as exc:
        return _error(exc.reason)

    dev_id, entry, reason = _select_device(snapshot, device_id)
    if reason:
        return _error(reason, device_id)
    entry = entry or {}
    status = entry.get("status")
    if not isinstance(status, dict):
        result = _error("no_status_cached", dev_id, snapshot)
        result["note"] = "消费者已收到事件，但尚未缓存状态"
        return result

    received_at = entry.get("status_received_at") or 0
    age = _age(received_at, now)
    fresh = age is not None and age <= STATUS_FRESH_S
    sensors = status.get("sensors") or {}
    result = {
        "available": True,
        "device_id": dev_id,
        "state": status.get("state"),
        "state_code": status.get("state_code"),
        "device_uptime_ms": status.get("timestamp"),
        "observed_at": _iso(received_at),
        "age_seconds": age,
        "fresh": fresh,
        "sensors": {
            "als": sensors.get("als"),
            "ps": sensors.get("ps"),
            "gx": sensors.get("gx"),
            "gy": sensors.get("gy"),
            "gz": sensors.get("gz"),
            "ap_ok": sensors.get("ap_ok"),
            "qma_ok": sensors.get("qma_ok"),
            "dark": sensors.get("dark"),
        },
        "last_event": status.get("last_event"),
        "sleepy_remaining_sec": status.get("sleepy_remaining_sec"),
        "wifi_connected": status.get("wifi_connected"),
        "cache_generated_at": _iso(snapshot.get("generated_at")),
        "cache_age_seconds": _age(snapshot.get("generated_at"), now),
        "data_source": DATA_SOURCE,
        "limitations": LIMITATIONS,
    }
    if not fresh:
        result["reason"] = "status_stale"
        result["note"] = "状态已超过 30 秒，仅代表最近一次缓存，不代表设备当前状态"
    if _json_bytes(result) <= MAX_RESULT_BYTES:
        return result

    compact = {
        "available": True,
        "device_id": dev_id,
        "state": result["state"],
        "device_uptime_ms": result["device_uptime_ms"],
        "observed_at": result["observed_at"],
        "age_seconds": result["age_seconds"],
        "fresh": result["fresh"],
        "sensors": result["sensors"],
        "data_source": DATA_SOURCE,
        "limitations": LIMITATIONS,
    }
    if not fresh:
        compact["reason"] = "status_stale"
        compact["note"] = "状态已超过 30 秒，仅代表最近一次缓存，不代表设备当前状态"
    return compact


def query_events(device_id=None, limit=5, now=None, path=None):
    """返回最近缓存事件，最新在前；limit 范围 1-10。"""
    if not isinstance(limit, int) or isinstance(limit, bool):
        raise ValueError("limit_not_integer")
    if limit < 1 or limit > MAX_EVENT_LIMIT:
        raise ValueError("limit_out_of_range")
    now = time.time() if now is None else now

    try:
        snapshot = _read_snapshot(path)
    except SnapshotUnavailable as exc:
        result = _error(exc.reason)
        result["events"] = []
        result["count"] = 0
        return result

    dev_id, entry, reason = _select_device(snapshot, device_id)
    if reason:
        result = _error(reason, device_id)
        result["events"] = []
        result["count"] = 0
        return result

    all_events = (entry or {}).get("events") or []
    cache_age = _age(snapshot.get("generated_at"), now)
    selected = []
    for item in all_events[:limit]:
        if not isinstance(item, dict):
            continue
        data = item.get("data") or {}
        selected.append({
            "event_type": item.get("event_type"),
            "sequence": item.get("sequence"),
            "observed_at": _iso(item.get("received_at")),
            "age_seconds": _age(item.get("received_at"), now),
            "state": data.get("state"),
            "als": data.get("als"),
            "ps": data.get("ps"),
            "dark": data.get("dark"),
        })

    result = {
        "available": True,
        "device_id": dev_id,
        "count": len(selected),
        "total_cached": len(all_events),
        "events": selected,
        "cache_generated_at": _iso(snapshot.get("generated_at")),
        "cache_age_seconds": cache_age,
        "cache_stale": cache_age is not None and cache_age > CACHE_STALE_S,
        "data_source": DATA_SOURCE,
        "limitations": LIMITATIONS,
    }
    if not selected:
        result["note"] = "暂无缓存事件"

    truncated = False
    while selected and _json_bytes(result) > MAX_RESULT_BYTES:
        selected.pop()
        result["count"] = len(selected)
        result["events"] = selected
        truncated = True
    if truncated:
        result["truncated"] = True
        result["note"] = "结果超过 1024 字节，已从最旧事件开始截断"
        while selected and _json_bytes(result) > MAX_RESULT_BYTES:
            selected.pop()
            result["count"] = len(selected)
            result["events"] = selected
    return result
