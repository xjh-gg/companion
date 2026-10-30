import json
import os
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import companion_ai as ai
import companion_consumer as consumer


def valid_ai_decision(**overrides):
    decision = {
        "schema_version": 1,
        "engine": "ai",
        "scene": "low_light",
        "intent": "offer_assistance",
        "confidence": 0.82,
        "reason": "环境光照持续偏低",
        "action": "noop",
        "speech": "",
        "speech_delivery": "unsupported",
        "ttl": 10,
    }
    decision.update(overrides)
    return decision


def status_payload(state="待机"):
    return {
        "schema_version": 1,
        "device_id": "dev-1",
        "message_type": "status",
        "sequence": 1,
        "state": state,
        "sleepy_remaining_sec": 0,
        "wifi_connected": True,
        "last_event": "none",
        "last_event_age_ms": 0,
        "sensors": {
            "als": 120, "ps": 0, "gx": 0.0, "gy": 0.0, "gz": 0.0,
            "ap_ok": True, "qma_ok": True, "dark": False,
        },
    }


def event_payload(event_type, sequence):
    return {
        "schema_version": 1,
        "device_id": "dev-1",
        "message_type": "event",
        "event_type": event_type,
        "sequence": sequence,
        "timestamp": 1000,
        "data": {"state": "待机", "als": 40, "ps": 0, "dark": True},
    }


class RuleEngineTests(unittest.TestCase):
    def test_dark_event_low_light(self):
        store = ai.ContextStore()
        store.update_status("dev-1", status_payload(), 100.0)
        store.add_event("dev-1", event_payload("dark", 2), 100.0)
        prompt = store.build("dev-1", {"type": "event", "event_type": "dark"},
                             100.0)
        decision = ai.RuleEngine().decide(prompt, 100.0)
        self.assertEqual(decision["engine"], "rules")
        self.assertEqual(decision["scene"], "low_light")
        self.assertEqual(decision["action"], "noop")

    def test_bright_event_light_restored(self):
        store = ai.ContextStore()
        store.update_status("dev-1", status_payload(), 100.0)
        store.add_event("dev-1", event_payload("bright", 2), 100.0)
        prompt = store.build("dev-1", {"type": "event", "event_type": "bright"},
                             100.0)
        decision = ai.RuleEngine().decide(prompt, 100.0)
        self.assertEqual(decision["scene"], "light_restored")

    def test_pickup_and_key_request_status(self):
        for etype, scene in (("pickup", "device_picked_up"),
                             ("key", "key_interaction")):
            with self.subTest(etype=etype):
                store = ai.ContextStore()
                store.update_status("dev-1", status_payload(), 100.0)
                store.add_event("dev-1", event_payload(etype, 2), 100.0)
                prompt = store.build("dev-1",
                                     {"type": "event", "event_type": etype},
                                     100.0)
                decision = ai.RuleEngine().decide(prompt, 100.0)
                self.assertEqual(decision["scene"], scene)
                self.assertEqual(decision["action"], "request_status")

    def test_sleepy_state_uses_chime(self):
        store = ai.ContextStore()
        store.update_status("dev-1", status_payload("犯困"), 100.0)
        prompt = store.build("dev-1", {"type": "status"}, 100.0)
        decision = ai.RuleEngine().decide(prompt, 100.0)
        self.assertEqual(decision["scene"], "sleepy")
        self.assertEqual(decision["action"], "play_chime")

    def test_missing_status_is_stale(self):
        store = ai.ContextStore()
        prompt = store.build("dev-1", {"type": "event", "event_type": "key"},
                             100.0)
        decision = ai.RuleEngine().decide(prompt, 100.0)
        self.assertEqual(decision["scene"], "status_stale")
        self.assertEqual(decision["action"], "request_status")


class ValidationTests(unittest.TestCase):
    def test_valid_decision(self):
        decision = ai.validate_ai_decision(valid_ai_decision())
        self.assertEqual(decision["engine"], "ai")
        self.assertEqual(decision["speech_delivery"], "unsupported")

    def test_fenced_json_is_accepted(self):
        text = "```json\n" + json.dumps(valid_ai_decision()) + "\n```"
        decision = ai.parse_ai_decision(text)
        self.assertEqual(decision["scene"], "low_light")

    def test_invalid_variants_rejected(self):
        cases = {
            "bad_schema": valid_ai_decision(schema_version=2),
            "bad_engine": valid_ai_decision(engine="rules"),
            "unknown_scene": valid_ai_decision(scene="user_depressed"),
            "unknown_intent": valid_ai_decision(intent="control_gpio"),
            "confidence_low": valid_ai_decision(confidence=-0.1),
            "confidence_high": valid_ai_decision(confidence=1.2),
            "confidence_string": valid_ai_decision(confidence="0.9"),
            "play_prompt": valid_ai_decision(action="play_prompt"),
            "shell": valid_ai_decision(action="shell"),
            "bad_delivery": valid_ai_decision(speech_delivery="tts"),
            "ttl_zero": valid_ai_decision(ttl=0),
            "ttl_bool": valid_ai_decision(ttl=True),
            "long_reason": valid_ai_decision(reason="x" * 201),
            "long_speech": valid_ai_decision(speech="x" * 201),
        }
        for name, payload in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    ai.validate_ai_decision(payload)

    def test_invalid_json_rejected(self):
        with self.assertRaises(ValueError):
            ai.parse_ai_decision("not json")


class FakeProvider:
    def __init__(self, response=None, error=None):
        self.response = response
        self.error = error
        self.calls = 0

    def complete(self, system_prompt, user_payload):
        self.calls += 1
        if self.error:
            raise self.error
        return self.response


class AIEngineTests(unittest.TestCase):
    def cfg(self, **overrides):
        data = dict(base_url="http://127.0.0.1:1/v1", model="m",
                    api_key="unit-test-key", timeout_s=1.0,
                    min_interval_s=20.0, max_calls_per_min=6)
        data.update(overrides)
        return ai.AIConfig(**data)

    def test_valid_ai_response(self):
        provider = FakeProvider(json.dumps(valid_ai_decision()))
        engine = ai.AIEngine(self.cfg(), provider=provider, clock=lambda: 0.0)
        decision = engine.decide({"trigger": {"type": "event"}}, 0.0)
        self.assertEqual(decision["engine"], "ai")
        self.assertEqual(provider.calls, 1)

    def test_invalid_json_raises_and_counts_call(self):
        provider = FakeProvider("oops")
        engine = ai.AIEngine(self.cfg(), provider=provider)
        with self.assertRaises(ValueError):
            engine.decide({}, 0.0)
        self.assertEqual(len(engine.call_times), 1)

    def test_provider_failure_counts_toward_rate_limit(self):
        provider = FakeProvider(error=ai.ProviderError("timeout"))
        engine = ai.AIEngine(self.cfg(min_interval_s=20.0), provider=provider)
        with self.assertRaises(ai.ProviderError):
            engine.decide({}, 100.0)
        with self.assertRaises(ai.AIThrottled):
            engine.decide({}, 101.0)

    def test_rate_limit(self):
        provider = FakeProvider(json.dumps(valid_ai_decision()))
        engine = ai.AIEngine(self.cfg(min_interval_s=20.0), provider=provider)
        engine.decide({}, 100.0)
        with self.assertRaises(ai.AIThrottled):
            engine.decide({}, 105.0)
        self.assertEqual(provider.calls, 1)

    def test_unconfigured_raises_provider_error(self):
        engine = ai.AIEngine(ai.AIConfig(mode="ai"))
        with self.assertRaises(ai.ProviderError):
            engine.decide({}, 0.0)


class ContextTests(unittest.TestCase):
    def test_event_dedupe_by_sequence(self):
        store = ai.ContextStore()
        self.assertTrue(store.add_event("dev-1", event_payload("key", 7), 10.0))
        self.assertFalse(store.add_event("dev-1", event_payload("key", 7), 11.0))

    def test_status_expires(self):
        store = ai.ContextStore()
        store.update_status("dev-1", status_payload(), 0.0)
        prompt = store.build("dev-1", {"type": "status"}, 31.0)
        self.assertFalse(prompt["status_fresh"])
        self.assertIsNone(prompt["status"])
        self.assertAlmostEqual(prompt["status_age_s"], 31.0)

    def test_old_events_filtered_and_limit_applied(self):
        store = ai.ContextStore()
        store.add_event("dev-1", event_payload("dark", 1), 0.0)
        for seq in range(2, 15):
            store.add_event("dev-1", event_payload("key", seq), 300.0)
        prompt = store.build("dev-1", {"type": "status"}, 301.0)
        self.assertEqual(len(prompt["events"]), ai.EVENT_CONTEXT_LIMIT)
        self.assertTrue(all(item["event_type"] != "dark"
                            for item in prompt["events"]))

    def test_only_old_events_are_dropped(self):
        store = ai.ContextStore()
        store.add_event("dev-1", event_payload("dark", 1), 0.0)
        prompt = store.build("dev-1", {"type": "status"}, 200.0)
        self.assertEqual(prompt["events"], [])

    def test_device_context_is_bounded(self):
        store = ai.ContextStore(max_devices=2)
        for index in range(3):
            store.update_status(f"dev-{index}", status_payload(), float(index))
        self.assertEqual(len(store._devices), 2)

    def test_export_snapshot_uses_wall_times(self):
        store = ai.ContextStore()
        store.update_status("dev-1", status_payload(), 10.0, 1000.5)
        store.add_event("dev-1", event_payload("key", 2), 11.0, 1001.5)
        store.add_event("dev-1", event_payload("pickup", 3), 12.0, 1002.5)
        snapshot = store.export_snapshot(now_wall=1003.0)
        device = snapshot["devices"]["dev-1"]
        self.assertEqual(snapshot["schema_version"], 1)
        self.assertAlmostEqual(snapshot["generated_at"], 1003.0)
        self.assertAlmostEqual(device["status_received_at"], 1000.5)
        self.assertEqual(device["status"]["state"], "待机")
        self.assertNotIn("_received_at", device["status"])
        self.assertEqual([item["sequence"] for item in device["events"]],
                         [3, 2])
        self.assertEqual(device["events"][0]["event_type"], "pickup")

    def test_export_snapshot_is_bounded(self):
        store = ai.ContextStore(event_limit=20)
        for seq in range(30):
            store.add_event("dev-1", event_payload("key", seq), float(seq),
                            float(seq) + 1000)
        snapshot = store.export_snapshot(now_wall=2000.0)
        events = snapshot["devices"]["dev-1"]["events"]
        self.assertEqual(len(events), 20)
        self.assertEqual(events[0]["sequence"], 29)
        self.assertEqual(events[-1]["sequence"], 10)


class SafetyTests(unittest.TestCase):
    def test_action_whitelist(self):
        safety = ai.CommandSafety()
        allowed, reason = safety.check(
            "dev-1", valid_ai_decision(action="play_prompt"), 0.0)
        self.assertFalse(allowed)
        self.assertEqual(reason, "action_not_allowed")

    def test_chime_requires_confidence(self):
        safety = ai.CommandSafety()
        allowed, reason = safety.check(
            "dev-1", valid_ai_decision(action="play_chime", confidence=0.5), 0.0)
        self.assertFalse(allowed)
        self.assertEqual(reason, "low_confidence")

    def test_cooldown_and_dedupe(self):
        safety = ai.CommandSafety()
        decision = valid_ai_decision(action="play_chime", confidence=0.9)
        self.assertTrue(safety.check("dev-1", decision, 0.0)[0])
        safety.mark("dev-1", decision, 0.0)
        self.assertEqual(safety.check("dev-1", decision, 30.0)[1], "cooldown")
        self.assertTrue(safety.check("dev-1", decision, 61.0)[0])

    def test_duplicate_request_status_decision(self):
        safety = ai.CommandSafety()
        decision = valid_ai_decision(scene="device_picked_up",
                                     intent="acknowledge",
                                     action="request_status")
        self.assertTrue(safety.check("dev-1", decision, 0.0)[0])
        safety.mark("dev-1", decision, 0.0)
        self.assertEqual(safety.check("dev-1", decision, 10.0)[1],
                         "duplicate_decision")
        self.assertTrue(safety.check("dev-1", decision, 31.0)[0])


class FakePublishInfo:
    def __init__(self, rc=0):
        self.rc = rc


class FakeClient:
    def __init__(self):
        self.messages = []
        self.rc = 0

    def publish(self, topic, payload, qos=1):
        self.messages.append((topic, json.loads(payload), qos))
        return FakePublishInfo(self.rc)


class CommandPublishTests(unittest.TestCase):
    def test_topic_device_match(self):
        self.assertTrue(consumer.topic_device_matches(
            "companion/dev-1/command", "dev-1", "command"))
        self.assertFalse(consumer.topic_device_matches(
            "companion/dev-2/event", "dev-1", "event"))

    def make_consumer(self):
        obj = consumer.Consumer.__new__(consumer.Consumer)
        obj.device_id = "dev-1"
        obj.client = FakeClient()
        return obj

    def test_topic_and_device_id_match(self):
        obj = self.make_consumer()
        obj.publish_command("request_status", 10, intent="acknowledge")
        topic, payload, qos = obj.client.messages[0]
        self.assertEqual(topic, "companion/dev-1/command")
        self.assertEqual(payload["device_id"], "dev-1")
        self.assertEqual(payload["action"], "request_status")
        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(qos, 1)

    def test_publish_failure_not_retried(self):
        obj = self.make_consumer()
        obj.client.rc = 4
        obj.publish_command("noop", 5)
        self.assertEqual(len(obj.client.messages), 1)


class ConfigTests(unittest.TestCase):
    def test_no_config_is_unavailable(self):
        cfg = ai.load_ai_config(None, env={})
        self.assertEqual(cfg.mode, "auto")
        self.assertFalse(cfg.available)

    def test_env_overrides_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".json",
                                        delete=False) as handle:
            json.dump({"base_url": "http://file/v1", "model": "file-model",
                       "api_key": "file-key", "timeout_s": 3}, handle)
            path = handle.name
        try:
            cfg = ai.load_ai_config(path, env={
                "COMPANION_AI_MODEL": "env-model",
                "COMPANION_AI_API_KEY": "env-key",
                "COMPANION_AI_MODE": "ai",
            })
            self.assertEqual(cfg.mode, "ai")
            self.assertEqual(cfg.model, "env-model")
            self.assertEqual(cfg.api_key, "env-key")
            self.assertEqual(cfg.base_url, "http://file/v1")
            self.assertEqual(cfg.timeout_s, 3.0)
            self.assertTrue(cfg.available)
        finally:
            os.unlink(path)

    def test_bad_mode_rejected(self):
        with self.assertRaises(ValueError):
            ai.load_ai_config(None, env={"COMPANION_AI_MODE": "magic"})

    def test_false_string_disables_rule_fallback(self):
        cfg = ai.load_ai_config(
            None, env={"COMPANION_AI_FALLBACK_RULES": "false"})
        self.assertFalse(cfg.fallback_rules)


class _ProviderHandler(BaseHTTPRequestHandler):
    received = {}
    delay = 0.0
    status_code = 200

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length).decode("utf-8"))
        type(self).received = {
            "path": self.path,
            "auth": self.headers.get("Authorization"),
            "body": body,
        }
        if self.delay:
            time.sleep(self.delay)
        if self.status_code != 200:
            self.send_response(self.status_code)
            self.end_headers()
            return
        content = json.dumps(valid_ai_decision())
        payload = json.dumps({
            "choices": [{"message": {"content": content}}]
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        try:
            self.wfile.write(payload)
        except ConnectionAbortedError:
            pass

    def log_message(self, *args):
        pass


class ProviderHTTPTests(unittest.TestCase):
    def start_server(self):
        server = ThreadingHTTPServer(("127.0.0.1", 0), _ProviderHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def test_openai_compatible_request(self):
        server = self.start_server()
        port = server.server_address[1]
        cfg = ai.AIConfig(base_url=f"http://127.0.0.1:{port}/v1",
                          model="unit-model", api_key="unit-key",
                          timeout_s=2)
        text = ai.OpenAICompatibleProvider(cfg).complete("system", {"a": 1})
        self.assertIn("low_light", text)
        received = _ProviderHandler.received
        self.assertEqual(received["path"], "/v1/chat/completions")
        self.assertEqual(received["auth"], "Bearer unit-key")
        self.assertEqual(received["body"]["model"], "unit-model")
        self.assertEqual(received["body"]["messages"][0]["role"], "system")

    def test_http_error_maps_to_provider_error(self):
        _ProviderHandler.status_code = 500
        self.addCleanup(setattr, _ProviderHandler, "status_code", 200)
        server = self.start_server()
        port = server.server_address[1]
        cfg = ai.AIConfig(base_url=f"http://127.0.0.1:{port}/v1",
                          model="m", api_key="k", timeout_s=2)
        with self.assertRaises(ai.ProviderError):
            ai.OpenAICompatibleProvider(cfg).complete("s", {})

    def test_timeout_maps_to_provider_error(self):
        _ProviderHandler.delay = 0.5
        self.addCleanup(setattr, _ProviderHandler, "delay", 0.0)
        server = self.start_server()
        port = server.server_address[1]
        cfg = ai.AIConfig(base_url=f"http://127.0.0.1:{port}/v1",
                          model="m", api_key="k", timeout_s=0.1)
        with self.assertRaises(ai.ProviderError) as ctx:
            ai.OpenAICompatibleProvider(cfg).complete("s", {})
        self.assertEqual(str(ctx.exception), "timeout")


class _BlockingEngine:
    def __init__(self):
        self.started = threading.Event()
        self.release = threading.Event()

    def decide(self, prompt_context, now):
        self.started.set()
        self.release.wait(2)
        return ai.validate_ai_decision(valid_ai_decision())


class WorkerTests(unittest.TestCase):
    def test_queue_is_bounded(self):
        engine = _BlockingEngine()
        results = []
        worker = ai.AIDecisionWorker(
            engine,
            lambda device_id, decision, ctx, now: results.append(decision),
            lambda *args: None,
        )
        try:
            self.assertTrue(worker.submit("dev-1", {}, 0.0))
            self.assertTrue(engine.started.wait(1))
            self.assertTrue(worker.submit("dev-1", {}, 0.0))
            self.assertFalse(worker.submit("dev-1", {}, 0.0))
            engine.release.set()
            deadline = time.time() + 3
            while len(results) < 2 and time.time() < deadline:
                time.sleep(0.02)
            self.assertEqual(len(results), 2)
        finally:
            worker.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
