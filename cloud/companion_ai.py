"""场景判断引擎：规则降级、AI Provider、上下文、安全策略。

设计约束：
- 规则引擎与 AI 引擎使用不同 engine 标记，禁止互相冒充。
- AI 只输出建议，命令必须经过字段校验、动作白名单和冷却策略。
- 凭据只从环境变量或本地忽略的配置文件读取，不写入源码或日志。
- 设备当前不支持 play_prompt，speech 只作为未来语音内容占位。
"""

import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from collections import deque
from dataclasses import dataclass, field

SCHEMA_VERSION = 1
ALLOWED_ACTIONS = {"noop", "request_status", "play_chime"}
ALLOWED_SCENES = {
    "calm",
    "low_light",
    "light_restored",
    "device_picked_up",
    "key_interaction",
    "motion",
    "sleepy",
    "status_stale",
}
ALLOWED_INTENTS = {
    "observe",
    "offer_assistance",
    "acknowledge",
    "request_refresh",
    "gentle_reminder",
    "greet",
    "no_action",
}
MAX_REASON_LEN = 200
MAX_SPEECH_LEN = 200
EVENT_CONTEXT_LIMIT = 10
EVENT_CONTEXT_MAX_AGE_S = 120.0
EVENT_FRESH_S = 15.0
STATUS_FRESH_S = 30.0
DECISION_DEDUPE_S = 30.0
DEFAULT_COOLDOWNS = {"noop": 0.0, "request_status": 8.0, "play_chime": 60.0}


class ProviderError(Exception):
    """AI Provider 不可用、超时或返回异常。"""


class AIThrottled(Exception):
    """调用间隔或频率限制生效。"""


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _text(value, field_name, max_len, required=True, allow_empty=True):
    if value is None and not required:
        return ""
    if not isinstance(value, str):
        raise ValueError(f"{field_name}_not_string")
    if len(value) > max_len:
        raise ValueError(f"{field_name}_too_long")
    if not allow_empty and not value:
        raise ValueError(f"{field_name}_empty")
    return value


def parse_ai_decision(text):
    """解析模型文本输出并执行严格字段校验。"""
    if not isinstance(text, str) or not text.strip():
        raise ValueError("empty_response")
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        raise ValueError("no_json_object")
    try:
        obj = json.loads(text[start:end + 1])
    except Exception as exc:
        raise ValueError("invalid_json") from exc
    return validate_ai_decision(obj)


def validate_ai_decision(obj):
    if not isinstance(obj, dict):
        raise ValueError("not_object")
    if obj.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("bad_schema_version")
    if obj.get("engine") != "ai":
        raise ValueError("bad_engine")
    scene = _text(obj.get("scene"), "scene", 32, allow_empty=False)
    if scene not in ALLOWED_SCENES:
        raise ValueError("unknown_scene")
    intent = _text(obj.get("intent"), "intent", 32, allow_empty=False)
    if intent not in ALLOWED_INTENTS:
        raise ValueError("unknown_intent")
    confidence = obj.get("confidence")
    if not _is_number(confidence) or confidence < 0.0 or confidence > 1.0:
        raise ValueError("bad_confidence")
    reason = _text(obj.get("reason", ""), "reason", MAX_REASON_LEN,
                   required=False)
    action = obj.get("action")
    if action not in ALLOWED_ACTIONS:
        raise ValueError("unsupported_action")
    speech = _text(obj.get("speech", ""), "speech", MAX_SPEECH_LEN,
                   required=False)
    if obj.get("speech_delivery") != "unsupported":
        raise ValueError("bad_speech_delivery")
    ttl = obj.get("ttl")
    if not isinstance(ttl, int) or isinstance(ttl, bool) or ttl < 5 or ttl > 60:
        raise ValueError("bad_ttl")

    return {
        "schema_version": SCHEMA_VERSION,
        "engine": "ai",
        "scene": scene,
        "intent": intent,
        "confidence": float(confidence),
        "reason": reason,
        "action": action,
        "speech": speech,
        "speech_delivery": "unsupported",
        "ttl": ttl,
    }


def decide_rule(state, events, now):
    """确定性规则引擎，不是 AI；AI 不可用时作为降级模式。"""
    status_age = None
    if state:
        status_age = max(0.0, now - state.get("_received_at", now))
    recent = [e for e in events
              if now - e.get("_received_at", now) <= EVENT_FRESH_S]
    event = recent[-1] if recent else None
    etype = event.get("event_type") if event else None

    decision = {
        "schema_version": SCHEMA_VERSION,
        "engine": "rules",
        "scene": "calm",
        "intent": "observe",
        "confidence": 0.2,
        "reason": "无显著事件或状态变化",
        "action": "noop",
        "speech": "",
        "speech_delivery": "unsupported",
        "ttl": 5,
    }

    if status_age is None or status_age > STATUS_FRESH_S:
        decision.update(scene="status_stale", intent="request_refresh",
                        confidence=0.8,
                        reason="缺少新鲜状态，需要设备重新上报",
                        action="request_status", ttl=10)
    elif state.get("state") == "犯困":
        decision.update(scene="sleepy", intent="gentle_reminder",
                        confidence=0.85, reason="设备处于犯困状态",
                        action="play_chime", ttl=10)
    elif etype == "dark":
        decision.update(scene="low_light", intent="observe",
                        confidence=0.6, reason="收到暗光事件",
                        action="noop", ttl=5)
    elif etype == "bright":
        decision.update(scene="light_restored", intent="observe",
                        confidence=0.6, reason="收到光照恢复事件",
                        action="noop", ttl=5)
    elif etype == "pickup":
        decision.update(scene="device_picked_up", intent="acknowledge",
                        confidence=0.6, reason="设备被拿起",
                        action="request_status", ttl=10)
    elif etype == "key":
        decision.update(scene="key_interaction", intent="acknowledge",
                        confidence=0.6, reason="检测到按键交互",
                        action="request_status", ttl=10)
    elif etype == "shake":
        decision.update(scene="motion", intent="acknowledge",
                        confidence=0.5, reason="检测到晃动",
                        action="noop", ttl=5)
    elif etype == "hand_near":
        decision.update(scene="motion", intent="greet",
                        confidence=0.4, reason="检测到手部靠近",
                        action="noop", ttl=5)

    if decision["action"] not in ALLOWED_ACTIONS:
        decision.update(action="noop", ttl=5)
    return decision


class SceneEngine:
    name = "base"

    def decide(self, prompt_context, now):
        raise NotImplementedError


class RuleEngine(SceneEngine):
    name = "rules"

    def decide(self, prompt_context, now):
        status = prompt_context.get("status")
        if status is not None:
            status = dict(status)
            status["_received_at"] = now - prompt_context["status_age_s"]
        events = []
        for item in prompt_context.get("events", []):
            event = dict(item)
            event["_received_at"] = now - item.get("age_s", EVENT_CONTEXT_MAX_AGE_S)
            events.append(event)
        return decide_rule(status, events, now)


@dataclass
class AIConfig:
    mode: str = "auto"
    base_url: str = ""
    model: str = ""
    api_key: str = ""
    timeout_s: float = 12.0
    min_interval_s: float = 20.0
    max_calls_per_min: int = 6
    reevaluate_s: float = 300.0
    fallback_rules: bool = True

    @property
    def available(self):
        return bool(self.base_url and self.model and self.api_key)

    def safe_summary(self):
        endpoint = ""
        if self.base_url:
            try:
                parsed = urllib.parse.urlsplit(self.base_url)
                endpoint = f"{parsed.scheme}://{parsed.hostname or 'unknown'}"
                if parsed.port:
                    endpoint += f":{parsed.port}"
            except ValueError:
                endpoint = "configured"
        return {
            "mode": self.mode,
            "available": self.available,
            "endpoint": endpoint,
            "model": self.model,
            "timeout_s": self.timeout_s,
            "min_interval_s": self.min_interval_s,
            "max_calls_per_min": self.max_calls_per_min,
            "reevaluate_s": self.reevaluate_s,
            "fallback_rules": self.fallback_rules,
        }


def _load_json_config(path):
    if not path or not os.path.exists(path):
        return {}
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except Exception as exc:
        raise ValueError("ai_config_invalid_json") from exc
    if not isinstance(data, dict):
        raise ValueError("ai_config_not_object")
    return data


def _as_bool(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return bool(value)


def load_ai_config(path=None, env=None, mode_override=None):
    """从本地配置文件和环境变量加载配置；环境变量优先。"""
    env = os.environ if env is None else env
    data = _load_json_config(path or env.get("COMPANION_AI_CONFIG", ""))

    def pick(env_key, file_key, default):
        value = env.get(env_key)
        if value not in (None, ""):
            return value
        return data.get(file_key, default)

    mode = mode_override or pick("COMPANION_AI_MODE", "mode", "auto")
    if mode not in ("auto", "rules", "ai"):
        raise ValueError("bad_ai_mode")
    cfg = AIConfig(
        mode=mode,
        base_url=str(pick("COMPANION_AI_BASE_URL", "base_url", "")).strip(),
        model=str(pick("COMPANION_AI_MODEL", "model", "")).strip(),
        api_key=str(pick("COMPANION_AI_API_KEY", "api_key", "")).strip(),
        timeout_s=float(pick("COMPANION_AI_TIMEOUT_S", "timeout_s", 12.0)),
        min_interval_s=float(pick("COMPANION_AI_MIN_INTERVAL_S",
                                  "min_interval_s", 20.0)),
        max_calls_per_min=int(pick("COMPANION_AI_MAX_CALLS_PER_MIN",
                                   "max_calls_per_min", 6)),
        reevaluate_s=float(pick("COMPANION_AI_REEVALUATE_S",
                                "reevaluate_s", 300.0)),
        fallback_rules=_as_bool(pick("COMPANION_AI_FALLBACK_RULES",
                                     "fallback_rules", True)),
    )
    if cfg.timeout_s <= 0 or cfg.min_interval_s < 0 or cfg.max_calls_per_min <= 0:
        raise ValueError("bad_ai_limits")
    return cfg


class OpenAICompatibleProvider:
    """OpenAI Chat Completions 兼容接口，仅用标准库。"""

    def __init__(self, config):
        self.config = config

    def complete(self, system_prompt, user_payload):
        url = self.config.base_url.rstrip("/")
        if not url.endswith("/chat/completions"):
            url += "/chat/completions"
        body = json.dumps({
            "model": self.config.model,
            "temperature": 0,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": json.dumps(
                    user_payload, ensure_ascii=False, separators=(",", ":"))},
            ],
        }).encode("utf-8")
        request = urllib.request.Request(
            url, data=body, method="POST",
            headers={
                "Content-Type": "application/json",
                "Authorization": "Bearer " + self.config.api_key,
            },
        )
        try:
            with urllib.request.urlopen(request,
                                        timeout=self.config.timeout_s) as resp:
                raw = resp.read(65536)
        except urllib.error.HTTPError as exc:
            raise ProviderError(f"http_{exc.code}") from exc
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            reason = getattr(exc, "reason", exc)
            if isinstance(reason, TimeoutError) or "timed out" in str(reason):
                raise ProviderError("timeout") from exc
            raise ProviderError("network") from exc
        try:
            data = json.loads(raw.decode("utf-8"))
            content = data["choices"][0]["message"]["content"]
        except Exception as exc:
            raise ProviderError("bad_response") from exc
        if not isinstance(content, str) or not content.strip():
            raise ProviderError("empty_content")
        return content


AI_SYSTEM_PROMPT = """你是情感陪伴设备的场景分类器，只输出一个 JSON 对象，不要输出解释或 Markdown。
字段要求：
- schema_version: 1
- engine: "ai"
- scene: calm|low_light|light_restored|device_picked_up|key_interaction|motion|sleepy|status_stale
- intent: observe|offer_assistance|acknowledge|request_refresh|gentle_reminder|greet|no_action
- confidence: 0 到 1 的数字
- reason: 不超过 80 字，基于输入事实，不要推断用户情绪或心理状态
- action: noop|request_status|play_chime，禁止 play_prompt
- speech: 空字符串
- speech_delivery: "unsupported"
- ttl: 5 到 60 的整数秒
如果上下文不足、状态过期或无法确定场景，action 必须是 noop。
只根据给定事件和状态判断，不要虚构传感器读数。"""


class AIEngine(SceneEngine):
    name = "ai"

    def __init__(self, config, provider=None, clock=None):
        self.config = config
        self.provider = provider or OpenAICompatibleProvider(config)
        self.clock = clock or time.monotonic
        self.call_times = deque(maxlen=max(config.max_calls_per_min, 1))

    def can_call(self, now):
        if self.call_times and now - self.call_times[-1] < self.config.min_interval_s:
            return False
        cutoff = now - 60.0
        recent = [t for t in self.call_times if t >= cutoff]
        return len(recent) < self.config.max_calls_per_min

    def decide(self, prompt_context, now):
        if not self.config.available:
            raise ProviderError("not_configured")
        if not self.can_call(now):
            raise AIThrottled("rate_limited")
        # 尝试即计入频率限制，避免 Provider 故障时被事件持续触发。
        self.call_times.append(now)
        text = self.provider.complete(AI_SYSTEM_PROMPT, prompt_context)
        return parse_ai_decision(text)


@dataclass
class DeviceContext:
    status: dict = None
    status_at: float = 0.0
    status_wall_at: float = 0.0
    events: deque = field(default_factory=lambda: deque(maxlen=20))
    event_keys: deque = field(default_factory=lambda: deque(maxlen=100))
    last_decision: dict = None
    last_decision_at: float = 0.0
    last_ai_at: float = 0.0


class ContextStore:
    """有界设备上下文；事件按 sequence 去重，过期状态不当作当前状态。"""

    def __init__(self, max_devices=8, event_limit=20):
        self.max_devices = max_devices
        self.event_limit = event_limit
        self._devices = {}
        self._order = deque()

    def _get(self, device_id, now):
        ctx = self._devices.get(device_id)
        if ctx is None:
            ctx = DeviceContext(events=deque(maxlen=self.event_limit))
            self._devices[device_id] = ctx
            self._order.append(device_id)
            while len(self._order) > self.max_devices:
                old = self._order.popleft()
                self._devices.pop(old, None)
        return ctx

    def update_status(self, device_id, payload, now, wall_at=None):
        ctx = self._get(device_id, now)
        ctx.status = dict(payload)
        ctx.status_at = now
        ctx.status_wall_at = float(wall_at or 0.0)

    def add_event(self, device_id, payload, now, wall_at=None):
        ctx = self._get(device_id, now)
        dedupe = self._event_key(payload)
        if dedupe in ctx.event_keys:
            return False
        ctx.event_keys.append(dedupe)
        item = dict(payload)
        item["_received_at"] = now
        item["_wall_at"] = float(wall_at or 0.0)
        ctx.events.append(item)
        return True

    @staticmethod
    def _event_key(payload):
        seq = payload.get("sequence")
        if seq is not None:
            return ("seq", seq)
        return ("fallback", payload.get("device_id"),
                payload.get("event_type"), payload.get("timestamp"))

    def build(self, device_id, trigger, now):
        ctx = self._get(device_id, now)
        status = None
        status_age = None
        status_fresh = False
        if ctx.status is not None:
            status_age = max(0.0, now - ctx.status_at)
            status_fresh = status_age <= STATUS_FRESH_S
            if status_fresh:
                status = self._status_subset(ctx.status)

        events = []
        for item in reversed(ctx.events):
            age = max(0.0, now - item.get("_received_at", now))
            if age > EVENT_CONTEXT_MAX_AGE_S:
                continue
            events.append({
                "event_type": item.get("event_type"),
                "age_s": round(age, 1),
                "sequence": item.get("sequence"),
                "state": (item.get("data") or {}).get("state"),
                "als": (item.get("data") or {}).get("als"),
                "ps": (item.get("data") or {}).get("ps"),
                "dark": (item.get("data") or {}).get("dark"),
            })
            if len(events) >= EVENT_CONTEXT_LIMIT:
                break
        events.reverse()

        last_decision = None
        if ctx.last_decision is not None:
            last_decision = {
                "scene": ctx.last_decision.get("scene"),
                "intent": ctx.last_decision.get("intent"),
                "action": ctx.last_decision.get("action"),
                "engine": ctx.last_decision.get("engine"),
                "age_s": round(max(0.0, now - ctx.last_decision_at), 1),
            }

        prompt = {
            "trigger": trigger,
            "status_fresh": status_fresh,
            "status_age_s": None if status_age is None else round(status_age, 1),
            "status": status,
            "events": events,
            "last_decision": last_decision,
        }
        return prompt

    def record_decision(self, device_id, decision, now, engine_name):
        ctx = self._get(device_id, now)
        ctx.last_decision = dict(decision)
        ctx.last_decision_at = now
        if engine_name == "ai":
            ctx.last_ai_at = now

    def last_decision_age(self, device_id, now):
        ctx = self._get(device_id, now)
        if ctx.last_decision_at == 0:
            return None
        return now - ctx.last_decision_at

    def export_snapshot(self, now_wall=None, max_events=None):
        """导出只读缓存快照，供本地 MCP 服务读取；不包含任何凭据。"""
        limit = self.event_limit if max_events is None else max_events
        devices = {}
        for device_id, ctx in self._devices.items():
            entry = {"events": []}
            if ctx.status is not None:
                status = dict(ctx.status)
                status.pop("_received_at", None)
                entry["status"] = status
                entry["status_received_at"] = round(ctx.status_wall_at, 3)
            for item in list(ctx.events)[-limit:][::-1]:
                data = dict(item.get("data") or {})
                entry["events"].append({
                    "event_type": item.get("event_type"),
                    "sequence": item.get("sequence"),
                    "timestamp": item.get("timestamp"),
                    "received_at": round(float(item.get("_wall_at") or 0.0), 3),
                    "data": data,
                })
            devices[device_id] = entry
        return {
            "schema_version": SCHEMA_VERSION,
            "generated_at": round(float(now_wall if now_wall is not None
                                        else time.time()), 3),
            "devices": devices,
        }

    @staticmethod
    def _status_subset(payload):
        sensors = payload.get("sensors") or {}
        return {
            "state": payload.get("state"),
            "state_code": payload.get("state_code"),
            "sleepy_remaining_sec": payload.get("sleepy_remaining_sec"),
            "wifi_connected": payload.get("wifi_connected"),
            "last_event": payload.get("last_event"),
            "last_event_age_ms": payload.get("last_event_age_ms"),
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
        }


class CommandSafety:
    """动作白名单、置信度门槛、每设备冷却和重复决策去重。"""

    def __init__(self, cooldowns=None, dedupe_window_s=DECISION_DEDUPE_S,
                 min_chime_confidence=0.75, clock=None):
        self.cooldowns = dict(DEFAULT_COOLDOWNS)
        if cooldowns:
            self.cooldowns.update(cooldowns)
        self.dedupe_window_s = dedupe_window_s
        self.min_chime_confidence = min_chime_confidence
        self.clock = clock or time.monotonic
        self.last_action_at = {}
        self.last_signature = {}
        self.last_signature_at = {}

    def check(self, device_id, decision, now):
        action = decision.get("action")
        if action not in ALLOWED_ACTIONS:
            return False, "action_not_allowed"
        if action == "play_chime":
            if float(decision.get("confidence", 0)) < self.min_chime_confidence:
                return False, "low_confidence"
        cooldown = self.cooldowns.get(action, 0.0)
        last = self.last_action_at.get((device_id, action))
        if last is not None and now - last < cooldown:
            return False, "cooldown"
        signature = (decision.get("scene"), decision.get("intent"), action)
        key = (device_id, signature)
        last_sig = self.last_signature_at.get(key)
        if (action != "noop" and last_sig is not None and
                now - last_sig < self.dedupe_window_s):
            return False, "duplicate_decision"
        return True, "ok"

    def mark(self, device_id, decision, now):
        action = decision.get("action")
        self.last_action_at[(device_id, action)] = now
        signature = (decision.get("scene"), decision.get("intent"), action)
        self.last_signature[(device_id, signature)] = now
        self.last_signature_at[(device_id, signature)] = now


class AIDecisionWorker:
    """单线程、容量 1 的 AI 决策队列，防止并发与内存无限增长。"""

    def __init__(self, engine, on_result, on_error):
        self.engine = engine
        self.on_result = on_result
        self.on_error = on_error
        self.queue = deque()
        self.lock = threading.Lock()
        self.stopping = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def submit(self, device_id, prompt_context, now):
        with self.lock:
            if self.stopping or len(self.queue) >= 1:
                return False
            self.queue.append((device_id, prompt_context, now))
        return True

    def _run(self):
        while True:
            with self.lock:
                if self.stopping:
                    return
                item = self.queue.popleft() if self.queue else None
            if item is None:
                time.sleep(0.05)
                continue
            device_id, prompt_context, submitted_at = item
            now = time.monotonic()
            try:
                decision = self.engine.decide(prompt_context, now)
                self.on_result(device_id, decision, prompt_context, now)
            except Exception as exc:
                self.on_error(device_id, exc, prompt_context, now,
                              time.monotonic() - submitted_at)

    def stop(self):
        with self.lock:
            self.stopping = True
        self.thread.join(timeout=3)
