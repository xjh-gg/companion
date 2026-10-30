import argparse
import json
import os
import time
import uuid

import paho.mqtt.client as mqtt

from companion_ai import (
    AIDecisionWorker,
    AIConfig,
    AIEngine,
    AIThrottled,
    CommandSafety,
    ContextStore,
    ProviderError,
    RuleEngine,
    load_ai_config,
)
from button_game import ButtonGameStore

DEFAULT_SECRETS = os.path.join(
    os.path.dirname(__file__), "..", "tools", "mqtt_broker", ".secrets.json"
)
DEFAULT_AI_CONFIG = os.path.join(os.path.dirname(__file__), "ai_config.json")
DEFAULT_STATE_FILE = os.path.join(os.path.dirname(__file__), "state",
                                  "smartguard_snapshot.json")


def mask(device_id):
    if not device_id:
        return "<unknown>"
    return device_id[:10] + "..." + device_id[-4:]


def topic_device_matches(topic, device_id, message_type):
    return topic == f"companion/{device_id}/{message_type}"


class Consumer:
    def __init__(self, args):
        self.args = args
        self.device_id = None
        self.negative_done = False

        secrets = json.load(open(args.secrets, encoding="utf-8"))
        self.host = args.host or secrets["host"]
        self.port = args.port or secrets["port"]
        cloud = secrets["cloud"]
        self.client = mqtt.Client(
            client_id="companion-cloud-" + uuid.uuid4().hex[:6],
            protocol=mqtt.MQTTv311,
        )
        self.client.username_pw_set(cloud["username"], cloud["password"])
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_disconnect = self.on_disconnect

        self.contexts = ContextStore(max_devices=8, event_limit=20)
        self.rule_engine = RuleEngine()
        self.safety = CommandSafety()
        self.state_file = (args.state_file or
                           os.environ.get("SMARTGUARD_STATE_FILE") or
                           DEFAULT_STATE_FILE)
        self.game_store = ButtonGameStore()

        try:
            config_path = (args.ai_config or
                           os.environ.get("COMPANION_AI_CONFIG") or
                           DEFAULT_AI_CONFIG)
            self.ai_config = load_ai_config(config_path, mode_override=args.mode)
        except ValueError as exc:
            print("AI_CONFIG_ERROR", str(exc), flush=True)
            self.ai_config = AIConfig(mode="rules")
        self.ai_engine = None
        self.ai_worker = None
        if self.ai_config.mode in ("auto", "ai") and self.ai_config.available:
            self.ai_engine = AIEngine(self.ai_config)
            self.ai_worker = AIDecisionWorker(
                self.ai_engine, self.on_ai_result, self.on_ai_error)
        print("AI_CONFIG", json.dumps(self.ai_config.safe_summary(),
                                      ensure_ascii=False), flush=True)
        if self.ai_config.mode == "ai" and self.ai_engine is None:
            print("AI_UNAVAILABLE fallback=rules", flush=True)

    # ---------- MQTT ----------
    def on_connect(self, client, userdata, flags, rc):
        print("CLOUD_CONNECTED", rc, flush=True)
        client.subscribe("companion/+/status", qos=1)
        client.subscribe("companion/+/event", qos=1)

    def on_disconnect(self, client, userdata, rc):
        print("CLOUD_DISCONNECTED", rc, flush=True)

    def publish_command(self, action, ttl, command_id=None, device_id=None,
                        intent="test", payload_device_id=None, feedback=None):
        target = device_id or self.device_id
        if not target:
            print("CMD_SKIPPED no_device_id", flush=True)
            return None
        command_id = command_id or ("cmd-" + uuid.uuid4().hex)
        payload = {
            "schema_version": 1,
            "device_id": payload_device_id or target,
            "command_id": command_id,
            "ttl": ttl,
            "action": action,
            "intent": intent,
        }
        if feedback is not None:
            payload["feedback"] = feedback
        topic = f"companion/{target}/command"
        info = self.client.publish(topic, json.dumps(payload), qos=1)
        print("CMD_SENT", action, "id", command_id[:8], "rc", info.rc, flush=True)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            print("CMD_PUBLISH_FAIL", action, "rc", info.rc, flush=True)
        return command_id

    def run_negative_tests(self):
        if self.negative_done or not self.device_id:
            return
        self.negative_done = True
        print("NEGATIVE_TESTS_START", flush=True)
        self.publish_command("noop", 5, payload_device_id="wrong-device")
        self.publish_command("play_prompt", 5)
        self.publish_command("noop", 0)
        dup_id = "dup-" + uuid.uuid4().hex
        self.publish_command("noop", 5, command_id=dup_id)
        time.sleep(0.4)
        self.publish_command("noop", 5, command_id=dup_id)
        time.sleep(0.4)
        self.publish_command("request_status", 10, intent="negative_test_done")

    def on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception as exc:
            print("INVALID_JSON", type(exc).__name__, flush=True)
            return

        if payload.get("schema_version") != 1:
            print("BAD_SCHEMA", flush=True)
            return
        device_id = payload.get("device_id")
        if not device_id:
            print("MISSING_DEVICE_ID", flush=True)
            return
        self.device_id = device_id
        mtype = payload.get("message_type")
        now_mono = time.monotonic()
        now_wall = time.time()

        if mtype in ("status", "event") and not topic_device_matches(
                msg.topic, device_id, mtype):
            print("TOPIC_DEVICE_MISMATCH", msg.topic,
                  mask(device_id), flush=True)
            return

        if mtype == "status":
            self.contexts.update_status(device_id, payload, now_mono, now_wall)
            self._write_snapshot()
            sensors = payload.get("sensors", {})
            print(
                "STATUS",
                mask(device_id),
                payload.get("state"),
                "als", sensors.get("als"),
                "ps", sensors.get("ps"),
                "ap_ok", sensors.get("ap_ok"),
                "qma_ok", sensors.get("qma_ok"),
                "seq", payload.get("sequence"),
                flush=True,
            )
            if self.args.negative_tests:
                self.run_negative_tests()
            if self._ai_enabled():
                age = self.contexts.last_decision_age(device_id, now_mono)
                if age is None or age >= self.ai_config.reevaluate_s:
                    self.evaluate(device_id, {"type": "status"}, now_mono)
        elif mtype == "event":
            if not self.contexts.add_event(device_id, payload, now_mono,
                                           now_wall):
                print("EVENT_DUPLICATE", mask(device_id),
                      payload.get("sequence"), flush=True)
                return
            self._write_snapshot()
            etype = payload.get("event_type")
            print(
                "EVENT",
                mask(device_id),
                etype,
                "state", payload.get("data", {}).get("state"),
                "seq", payload.get("sequence"),
                flush=True,
            )
            if etype == "key":
                self._handle_game_key(device_id, payload, now_wall)
            self.evaluate(device_id, {"type": "event", "event_type": etype},
                          now_mono)
        else:
            print("UNKNOWN_MESSAGE_TYPE", mtype, flush=True)

    # ---------- 场景判断 ----------
    def _ai_enabled(self):
        return self.ai_worker is not None

    def evaluate(self, device_id, trigger, now_mono):
        prompt_context = self.contexts.build(device_id, trigger, now_mono)
        if self._ai_enabled():
            if self.ai_worker.submit(device_id, prompt_context, now_mono):
                print("AI_QUEUED", mask(device_id),
                      trigger.get("event_type", trigger.get("type")),
                      flush=True)
            else:
                print("AI_BUSY_DROP", mask(device_id), flush=True)
                decision = self.rule_engine.decide(prompt_context, now_mono)
                self.contexts.record_decision(device_id, decision, now_mono,
                                              "rules")
                print("RULE_DECISION", json.dumps(decision, ensure_ascii=False),
                      flush=True)
                self.apply_decision(device_id, decision)
            return
        decision = self.rule_engine.decide(prompt_context, now_mono)
        self.contexts.record_decision(device_id, decision, now_mono, "rules")
        print("RULE_DECISION", json.dumps(decision, ensure_ascii=False),
              flush=True)
        self.apply_decision(device_id, decision)

    def on_ai_result(self, device_id, decision, prompt_context, now_mono):
        self.contexts.record_decision(device_id, decision, now_mono, "ai")
        print("SCENE_DECISION", json.dumps(decision, ensure_ascii=False),
              flush=True)
        self.apply_decision(device_id, decision)

    def on_ai_error(self, device_id, exc, prompt_context, now_mono, elapsed_s):
        kind = type(exc).__name__
        detail = str(exc)
        if isinstance(exc, AIThrottled):
            print("AI_THROTTLED", detail, flush=True)
        elif isinstance(exc, ProviderError):
            print("AI_ERROR", kind, detail,
                  "elapsed_s", round(elapsed_s, 2), flush=True)
        else:
            print("AI_ERROR", kind, detail,
                  "elapsed_s", round(elapsed_s, 2), flush=True)
        if not self.ai_config.fallback_rules:
            print("AI_FALLBACK none", flush=True)
            return
        decision = self.rule_engine.decide(prompt_context, time.monotonic())
        self.contexts.record_decision(device_id, decision,
                                      time.monotonic(), "rules")
        print("AI_FALLBACK engine=rules",
              json.dumps(decision, ensure_ascii=False), flush=True)
        self.apply_decision(device_id, decision)

    def apply_decision(self, device_id, decision):
        action = decision.get("action")
        now_mono = time.monotonic()
        allowed, reason = self.safety.check(device_id, decision, now_mono)
        if not allowed:
            print("ACTION_BLOCKED", action, reason, flush=True)
            return
        if action == "noop" and not self.args.publish_noop:
            print("DECISION_ONLY", decision.get("scene"), flush=True)
            return
        if self.args.no_command:
            print("ACTION_SKIPPED no_command", action, flush=True)
            return
        self.publish_command(action, decision.get("ttl", 5),
                             device_id=device_id,
                             intent=decision.get("intent", "observe"))
        self.safety.mark(device_id, decision, now_mono)

    def _write_snapshot(self):
        """原子写入只读缓存快照，供 smartguard_mcp.py 查询；失败不影响 MQTT。"""
        try:
            snapshot = self.contexts.export_snapshot()
            directory = os.path.dirname(self.state_file)
            if directory:
                os.makedirs(directory, exist_ok=True)
            temp_path = (self.state_file +
                         f".tmp.{os.getpid()}.{uuid.uuid4().hex[:6]}")
            with open(temp_path, "w", encoding="utf-8") as handle:
                json.dump(snapshot, handle, ensure_ascii=False,
                          separators=(",", ":"))
            os.replace(temp_path, self.state_file)
        except Exception as exc:
            print("STATE_SNAPSHOT_WRITE_FAIL", type(exc).__name__, flush=True)

    # ---------- 按键游戏 ----------
    def _handle_game_key(self, device_id, payload, now_wall):
        data = payload.get("data") or {}
        key_index = data.get("key_index")
        if not isinstance(key_index, int) or isinstance(key_index, bool) \
                or key_index < 0 or key_index > 3:
            print("GAME_INPUT_UNAVAILABLE missing_key_index",
                  mask(device_id), flush=True)
            return
        sequence = payload.get("sequence")
        event_key = (f"seq:{sequence}" if sequence is not None
                     else f"ts:{payload.get('timestamp')}")
        result = self.game_store.handle_key(
            device_id, event_key, key_index, payload.get("timestamp"),
            now=now_wall)
        if result.get("handled"):
            print("GAME_RESULT", result["game_id"], "target",
                  result["target_label"], "pressed", result["key_label"],
                  "outcome", result["outcome"], flush=True)
        elif result.get("reason") == "game_expired":
            print("GAME_TIMEOUT", result["game"]["game_id"],
                  "feedback=timeout", flush=True)
        elif result.get("reason") == "duplicate_event":
            print("GAME_EVENT_IGNORED duplicate_event",
                  mask(device_id), flush=True)
        elif result.get("reason") == "stale_event":
            print("GAME_EVENT_IGNORED stale_event",
                  mask(device_id), flush=True)
        elif result.get("reason") != "no_active_game":
            print("GAME_EVENT_IGNORED", result.get("reason"),
                  mask(device_id), flush=True)
        if result.get("feedback"):
            self._publish_game_feedback(device_id, result)

    def _publish_game_feedback(self, device_id, result):
        feedback = result.get("feedback")
        if feedback not in ("correct", "wrong", "timeout"):
            print("GAME_FEEDBACK_INVALID", feedback, flush=True)
            return
        game_id = result.get("game_id") or (result.get("game") or {}).get(
            "game_id")
        if self.args.no_command:
            print("GAME_FEEDBACK_SKIPPED no_command", game_id, feedback,
                  flush=True)
            return
        print("GAME_FEEDBACK", game_id, feedback,
              "text", result.get("feedback_text")
              if isinstance(result.get("feedback_text"), str)
              else (result.get("feedback_text") or {}).get(feedback, ""),
              flush=True)
        self.publish_command(
            action="play_feedback", ttl=15, device_id=device_id,
            intent=f"button_game_{feedback}", feedback=feedback)

    def tick(self):
        """由主循环调用：处理游戏超时，每次超时只发布一次反馈。"""
        for game in self.game_store.expire_due():
            print("GAME_TIMEOUT", game["game_id"], "feedback=timeout",
                  flush=True)
            self._publish_game_feedback(game["device_id"], game)

    def start(self):
        self.client.connect(self.host, self.port, keepalive=30)
        self.client.loop_start()

    def stop(self):
        if self.ai_worker is not None:
            self.ai_worker.stop()
        self.client.loop_stop()
        self.client.disconnect()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--secrets", default=DEFAULT_SECRETS)
    parser.add_argument("--ai-config", default=None,
                        help="默认 cloud/ai_config.json，可用 COMPANION_AI_CONFIG 覆盖")
    parser.add_argument("--mode", choices=("auto", "rules", "ai"), default=None,
                        help="auto: 有 AI 配置则用 AI，否则规则；rules: 强制规则")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--state-file", default=None,
                        help="MCP 只读快照路径，默认 cloud/state/smartguard_snapshot.json")
    parser.add_argument("--duration", type=int, default=60,
                        help="0 表示持续运行直到 Ctrl+C")
    parser.add_argument("--no-command", action="store_true",
                        help="只输出场景决策，不向设备发布命令")
    parser.add_argument("--publish-noop", action="store_true",
                        help="允许把 noop 也发布到设备（默认只观察）")
    parser.add_argument("--negative-tests", action="store_true")
    parser.add_argument("--auto-command", action="store_true",
                        help=argparse.SUPPRESS)
    args = parser.parse_args()

    consumer = Consumer(args)
    consumer.start()
    deadline = time.time() + args.duration if args.duration > 0 else None
    try:
        while deadline is None or time.time() < deadline:
            consumer.tick()
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("CONSUMER_INTERRUPTED", flush=True)
    finally:
        consumer.stop()
        print("CONSUMER_DONE", flush=True)


if __name__ == "__main__":
    main()
