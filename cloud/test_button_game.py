import asyncio
import json
import os
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import button_game
import companion_consumer
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


class FakeClock:
    def __init__(self, now=1000.0):
        self.now = now

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class FixedRandom:
    def __init__(self, value=2):
        self.value = value

    def randrange(self, stop):
        assert stop == button_game.BUTTON_COUNT
        return self.value


def make_store(tmpdir, clock=None, target=2):
    return button_game.ButtonGameStore(
        path=os.path.join(tmpdir, "game.db"), clock=clock,
        random_source=FixedRandom(target))


class ButtonGameStoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.clock = FakeClock(1000.0)
        self.store = make_store(self.tmp.name, self.clock)

    def start(self, **kwargs):
        return self.store.start("dev-1", timeout_seconds=30,
                                device_uptime_ms=10000, **kwargs)

    def test_start_waiting_game(self):
        game = self.start()
        self.assertEqual(game["target_button"], 2)
        self.assertEqual(game["target_label"], "KEY2")
        self.assertEqual(game["state"], button_game.STATE_WAITING)
        self.assertIn("KEY2", game["instruction"])
        self.assertEqual(game["feedback_text"]["correct"],
                         "你真棒，按对啦！")

    def test_new_game_supersedes_old(self):
        first = self.start()
        second = self.start(target_button=0)
        self.assertNotEqual(first["game_id"], second["game_id"])
        self.assertEqual(self.store.latest("dev-1")["state"],
                         button_game.STATE_WAITING)
        old = self.store.latest()
        self.assertEqual(old["game_id"], second["game_id"])
        result = self.store.handle_key("dev-1", "seq:1", 0, 11000)
        self.assertTrue(result["handled"])
        self.assertEqual(result["outcome"], "correct")

    def test_correct_key(self):
        game = self.start()
        result = self.store.handle_key("dev-1", "seq:11", 2, 11000)
        self.assertTrue(result["handled"])
        self.assertEqual(result["outcome"], "correct")
        self.assertEqual(result["feedback"], "correct")
        self.assertEqual(result["state"], button_game.STATE_COMPLETED)
        self.assertEqual(self.store.latest("dev-1")["state"],
                         button_game.STATE_COMPLETED)

    def test_wrong_then_correct(self):
        self.start(target_button=2)
        wrong = self.store.handle_key("dev-1", "seq:12", 0, 11000)
        self.assertTrue(wrong["handled"])
        self.assertEqual(wrong["outcome"], "wrong")
        self.assertEqual(wrong["feedback"], "wrong")
        self.assertEqual(wrong["state"], button_game.STATE_WAITING)
        self.assertEqual(wrong["game"]["attempts"], 1)
        right = self.store.handle_key("dev-1", "seq:13", 2, 12000)
        self.assertEqual(right["outcome"], "correct")

    def test_no_active_game_no_feedback(self):
        result = self.store.handle_key("dev-1", "seq:1", 0, 11000)
        self.assertFalse(result["handled"])
        self.assertEqual(result["reason"], "no_active_game")
        self.assertNotIn("feedback", result)

    def test_duplicate_event_once(self):
        self.start()
        first = self.store.handle_key("dev-1", "seq:20", 0, 11000)
        second = self.store.handle_key("dev-1", "seq:20", 0, 11000)
        self.assertEqual(first["outcome"], "wrong")
        self.assertFalse(second["handled"])
        self.assertEqual(second["reason"], "duplicate_event")
        self.assertNotIn("feedback", second)

    def test_stale_event_ignored(self):
        self.start()
        result = self.store.handle_key("dev-1", "seq:21", 2, 5000)
        self.assertFalse(result["handled"])
        self.assertEqual(result["reason"], "stale_event")
        self.assertNotIn("feedback", result)
        valid = self.store.handle_key("dev-1", "seq:22", 2, 11000)
        self.assertTrue(valid["handled"])

    def test_timeout_expires_once(self):
        self.start()
        self.clock.advance(31)
        expired = self.store.expire_due()
        self.assertEqual(len(expired), 1)
        self.assertEqual(expired[0]["feedback"], "timeout")
        self.assertEqual(expired[0]["state"], button_game.STATE_EXPIRED)
        self.assertEqual(self.store.expire_due(), [])
        after = self.store.handle_key("dev-1", "seq:30", 2, 20000)
        self.assertEqual(after["reason"], "no_active_game")

    def test_key_after_timeout_gets_timeout_once(self):
        self.start()
        self.clock.advance(31)
        result = self.store.handle_key("dev-1", "seq:31", 2, 20000)
        self.assertFalse(result["handled"])
        self.assertEqual(result["reason"], "game_expired")
        self.assertEqual(result["feedback"], "timeout")
        second = self.store.handle_key("dev-1", "seq:32", 2, 20000)
        self.assertEqual(second["reason"], "no_active_game")
        self.assertNotIn("feedback", second)

    def test_cancel_cleans_old_game(self):
        game = self.start()
        cancelled = self.store.cancel("dev-1")
        self.assertTrue(cancelled["cancelled"])
        self.assertEqual(cancelled["game"]["game_id"], game["game_id"])
        result = self.store.handle_key("dev-1", "seq:40", 2, 11000)
        self.assertEqual(result["reason"], "no_active_game")

    def test_invalid_arguments(self):
        with self.assertRaises(ValueError):
            self.store.start("dev-1", target_button=4)
        with self.assertRaises(ValueError):
            self.store.start("dev-1", timeout_seconds=5)
        with self.assertRaises(ValueError):
            self.store.start("dev-1", timeout_seconds=121)
        result = self.store.handle_key("dev-1", "seq:1", 9, 11000)
        self.assertEqual(result["reason"], "invalid_key_index")

    def test_state_visible_to_second_store_instance(self):
        self.start()
        other = button_game.ButtonGameStore(
            path=os.path.join(self.tmp.name, "game.db"), clock=self.clock)
        result = other.handle_key("dev-1", "seq:50", 2, 11000)
        self.assertTrue(result["handled"])
        self.assertEqual(result["outcome"], "correct")


class ConsumerGameIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.clock = FakeClock(1000.0)
        self.store = make_store(self.tmp.name, self.clock)
        self.consumer = companion_consumer.Consumer.__new__(
            companion_consumer.Consumer)
        self.consumer.game_store = self.store
        self.consumer.args = SimpleNamespace(no_command=False)
        self.sent = []

        def capture(*args, **kwargs):
            self.sent.append({"args": args, **kwargs})

        self.consumer.publish_command = capture

    def payload(self, key_index, sequence, timestamp=11000):
        return {
            "message_type": "event",
            "event_type": "key",
            "sequence": sequence,
            "timestamp": timestamp,
            "data": {"key_index": key_index, "state": "待机"},
        }

    def test_correct_key_publishes_fixed_feedback(self):
        self.store.start("dev-1", target_button=2, timeout_seconds=30,
                         device_uptime_ms=10000)
        self.consumer._handle_game_key("dev-1", self.payload(2, 10), 1000.0)
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.sent[0]["action"], "play_feedback")
        self.assertEqual(self.sent[0]["feedback"], "correct")
        self.assertEqual(self.sent[0]["device_id"], "dev-1")
        self.assertEqual(self.sent[0]["intent"], "button_game_correct")

    def test_wrong_then_duplicate_publishes_once(self):
        self.store.start("dev-1", target_button=2, timeout_seconds=30,
                         device_uptime_ms=10000)
        self.consumer._handle_game_key("dev-1", self.payload(0, 11), 1000.0)
        self.consumer._handle_game_key("dev-1", self.payload(0, 11), 1001.0)
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.sent[0]["feedback"], "wrong")

    def test_missing_key_index_no_feedback(self):
        self.store.start("dev-1", target_button=2, timeout_seconds=30,
                         device_uptime_ms=10000)
        payload = self.payload(2, 12)
        payload["data"] = {}
        self.consumer._handle_game_key("dev-1", payload, 1000.0)
        self.assertEqual(self.sent, [])

    def test_tick_publishes_timeout_once(self):
        self.store.start("dev-1", target_button=2, timeout_seconds=15,
                         device_uptime_ms=10000)
        self.clock.advance(16)
        self.consumer.tick()
        self.consumer.tick()
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.sent[0]["feedback"], "timeout")

    def test_no_command_suppresses_feedback(self):
        self.store.start("dev-1", target_button=2, timeout_seconds=30,
                         device_uptime_ms=10000)
        self.consumer.args.no_command = True
        self.consumer._handle_game_key("dev-1", self.payload(2, 13), 1000.0)
        self.assertEqual(self.sent, [])


class PublishCommandFeedbackTests(unittest.TestCase):
    def test_feedback_field_published(self):
        obj = companion_consumer.Consumer.__new__(
            companion_consumer.Consumer)
        obj.device_id = "dev-1"
        obj.client = companion_consumer.mqtt.Client(
            client_id="test", protocol=companion_consumer.mqtt.MQTTv311)

        messages = []

        class FakeInfo:
            rc = 0

        def fake_publish(topic, payload, qos=1):
            messages.append((topic, json.loads(payload), qos))
            return FakeInfo()

        obj.client.publish = fake_publish
        obj.publish_command("play_feedback", 15, device_id="dev-1",
                            intent="button_game_correct",
                            feedback="correct")
        topic, payload, qos = messages[0]
        self.assertEqual(topic, "companion/dev-1/command")
        self.assertEqual(payload["action"], "play_feedback")
        self.assertEqual(payload["feedback"], "correct")
        self.assertEqual(payload["ttl"], 15)
        self.assertEqual(qos, 1)


class MCPGameToolTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.state_path = os.path.join(self.tmp.name, "state.json")
        self.db_path = os.path.join(self.tmp.name, "game.db")
        now = time.time()
        snapshot = {
            "schema_version": 1,
            "generated_at": now - 1,
            "devices": {
                "dev-1": {
                    "status": {
                        "state": "待机",
                        "timestamp": 10000,
                        "sensors": {"als": 100, "ps": 0, "ap_ok": True,
                                    "qma_ok": True, "dark": False},
                    },
                    "status_received_at": now - 2,
                    "events": [],
                }
            },
        }
        with open(self.state_path, "w", encoding="utf-8") as handle:
            json.dump(snapshot, handle)

    def params(self):
        env = dict(os.environ)
        env["SMARTGUARD_STATE_FILE"] = self.state_path
        env["SMARTGUARD_GAME_DB"] = self.db_path
        return StdioServerParameters(
            command=sys.executable,
            args=[os.path.join(os.path.dirname(__file__),
                               "smartguard_mcp.py")],
            env=env,
        )

    async def test_start_cancel_and_shared_state(self):
        async with stdio_client(self.params()) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                tools = await session.list_tools()
                names = {tool.name for tool in tools.tools}
                self.assertIn("start_button_game", names)
                self.assertIn("cancel_button_game", names)
                start = await session.call_tool(
                    "start_button_game",
                    {"target_button": 2, "timeout_seconds": 30})
                started = json.loads(start.content[0].text)
                self.assertTrue(started["started"])
                self.assertEqual(started["target_label"], "KEY2")
                self.assertLessEqual(
                    len(start.content[0].text.encode("utf-8")), 1024)

                store = button_game.ButtonGameStore(path=self.db_path)
                result = store.handle_key(
                    "dev-1", "seq:100", 2, 11000)
                self.assertTrue(result["handled"])
                self.assertEqual(result["outcome"], "correct")

                start2 = await session.call_tool(
                    "start_button_game",
                    {"target_button": 1, "timeout_seconds": 30})
                self.assertTrue(json.loads(start2.content[0].text)["started"])
                cancel = await session.call_tool("cancel_button_game", {})
                cancelled = json.loads(cancel.content[0].text)
                self.assertTrue(cancelled["cancelled"])

    async def test_stale_status_rejected(self):
        with open(self.state_path, encoding="utf-8") as handle:
            data = json.load(handle)
        data["devices"]["dev-1"]["status_received_at"] = time.time() - 120
        with open(self.state_path, "w", encoding="utf-8") as handle:
            json.dump(data, handle)
        async with stdio_client(self.params()) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                result = await session.call_tool("start_button_game", {})
                payload = json.loads(result.content[0].text)
                self.assertFalse(payload["started"])
                self.assertEqual(payload["reason"], "device_status_stale")


if __name__ == "__main__":
    unittest.main(verbosity=2)
