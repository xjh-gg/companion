import json
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import smartguard_state as state

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def status_payload(state_name="待机"):
    return {
        "schema_version": 1,
        "device_id": "dev-1",
        "message_type": "status",
        "state": state_name,
        "state_code": 0,
        "last_event": "none",
        "sleepy_remaining_sec": 120,
        "wifi_connected": True,
        "sensors": {
            "als": 120, "ps": 3, "gx": 0.01, "gy": -0.02, "gz": 0.99,
            "ap_ok": True, "qma_ok": True, "dark": False,
        },
    }


def event_payload(etype, seq, state_name="待机", als=40, ps=0, dark=True):
    return {
        "event_type": etype,
        "sequence": seq,
        "timestamp": 1000 + seq,
        "received_at": 1000.0 + seq,
        "data": {"state": state_name, "als": als, "ps": ps, "dark": dark},
    }


def make_snapshot(status_at=1000.0, generated_at=1000.0, devices=None):
    return {
        "schema_version": 1,
        "generated_at": generated_at,
        "devices": devices if devices is not None else {
            "dev-1": {
                "status": status_payload(),
                "status_received_at": status_at,
                "events": [event_payload("key", 3),
                           event_payload("pickup", 2),
                           event_payload("dark", 1)],
            }
        },
    }


class TempStateFile:
    def __init__(self, payload=None, raw=None):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "snapshot.json")
        if raw is not None:
            with open(self.path, "w", encoding="utf-8") as handle:
                handle.write(raw)
        elif payload is not None:
            with open(self.path, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, ensure_ascii=False)

    def close(self):
        self.dir.cleanup()


class StateQueryTests(unittest.TestCase):
    def setUp(self):
        self.files = []

    def tearDown(self):
        for item in self.files:
            item.close()

    def temp(self, payload=None, raw=None):
        item = TempStateFile(payload, raw)
        self.files.append(item)
        return item.path

    def test_missing_cache(self):
        path = os.path.join(tempfile.gettempdir(), "no-such-smartguard.json")
        result = state.query_status(path=path)
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "no_cache_file")
        events = state.query_events(path=path)
        self.assertEqual(events["events"], [])
        self.assertEqual(events["count"], 0)

    def test_empty_devices(self):
        path = self.temp(make_snapshot(devices={}))
        result = state.query_status(path=path)
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "no_cached_devices")

    def test_fresh_status(self):
        path = self.temp(make_snapshot(status_at=1000.0))
        result = state.query_status(now=1005.0, path=path)
        self.assertTrue(result["available"])
        self.assertTrue(result["fresh"])
        self.assertEqual(result["state"], "待机")
        self.assertAlmostEqual(result["age_seconds"], 5.0)
        self.assertEqual(result["sensors"]["als"], 120)
        self.assertNotIn("reason", result)

    def test_stale_status(self):
        path = self.temp(make_snapshot(status_at=1000.0))
        result = state.query_status(now=1040.0, path=path)
        self.assertTrue(result["available"])
        self.assertFalse(result["fresh"])
        self.assertEqual(result["reason"], "status_stale")
        self.assertAlmostEqual(result["age_seconds"], 40.0)

    def test_no_status_but_events(self):
        path = self.temp(make_snapshot(devices={
            "dev-1": {"events": [event_payload("key", 1)]}}))
        result = state.query_status(path=path)
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "no_status_cached")
        events = state.query_events(path=path)
        self.assertTrue(events["available"])
        self.assertEqual(events["count"], 1)

    def test_device_selection(self):
        devices = {
            "old": {"status": status_payload(), "status_received_at": 100.0,
                    "events": []},
            "new": {"status": status_payload("被关注"),
                    "status_received_at": 200.0, "events": []},
        }
        path = self.temp(make_snapshot(devices=devices))
        result = state.query_status(now=205.0, path=path)
        self.assertEqual(result["device_id"], "new")
        old = state.query_status(device_id="old", now=205.0, path=path)
        self.assertEqual(old["device_id"], "old")
        missing = state.query_status(device_id="ghost", path=path)
        self.assertFalse(missing["available"])
        self.assertEqual(missing["reason"], "device_not_found")

    def test_events_limit_and_order(self):
        path = self.temp(make_snapshot())
        result = state.query_events(limit=2, now=1004.0, path=path)
        self.assertTrue(result["available"])
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["total_cached"], 3)
        self.assertEqual([item["sequence"] for item in result["events"]],
                         [3, 2])
        self.assertEqual(result["events"][0]["event_type"], "key")
        self.assertIn("observed_at", result["events"][0])

    def test_no_events(self):
        path = self.temp(make_snapshot(devices={
            "dev-1": {"status": status_payload(),
                      "status_received_at": 1000.0, "events": []}}))
        result = state.query_events(path=path)
        self.assertTrue(result["available"])
        self.assertEqual(result["events"], [])
        self.assertEqual(result["count"], 0)
        self.assertIn("暂无缓存事件", result["note"])

    def test_invalid_limit(self):
        path = self.temp(make_snapshot())
        for bad in (0, 11, "3", True):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    state.query_events(limit=bad, path=path)

    def test_invalid_cache(self):
        path = self.temp(raw="{not json")
        result = state.query_status(path=path)
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "cache_invalid")

    def test_status_result_capped(self):
        long_event = "x" * 2000
        payload = status_payload()
        payload["last_event"] = long_event
        path = self.temp(make_snapshot(devices={
            "dev-1": {"status": payload,
                      "status_received_at": time.time() - 2, "events": []}}))
        result = state.query_status(path=path)
        size = len(json.dumps(result, ensure_ascii=False,
                              separators=(",", ":")).encode("utf-8"))
        self.assertLessEqual(size, state.MAX_RESULT_BYTES)
        self.assertEqual(result["state"], "待机")

    def test_events_result_capped(self):
        events = [event_payload("key", 100 + index, state_name="待机",
                                als=index) for index in range(10)]
        for item in events:
            item["data"]["detail"] = "x" * 300
        path = self.temp(make_snapshot(devices={
            "dev-1": {"status": status_payload(),
                      "status_received_at": time.time() - 2,
                      "events": events}}))
        result = state.query_events(limit=10, path=path)
        size = len(json.dumps(result, ensure_ascii=False,
                              separators=(",", ":")).encode("utf-8"))
        self.assertLessEqual(size, state.MAX_RESULT_BYTES)
        self.assertTrue(result.get("truncated"))
        self.assertLess(result["count"], 10)


class MCPProtocolTests(unittest.IsolatedAsyncioTestCase):
    def write_state(self, payload):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = os.path.join(directory.name, "snapshot.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False)
        return path

    def params(self, path):
        env = dict(os.environ)
        env["SMARTGUARD_STATE_FILE"] = path
        return StdioServerParameters(
            command=sys.executable,
            args=[os.path.join(os.path.dirname(__file__), "smartguard_mcp.py")],
            env=env,
        )

    async def test_tool_discovery_and_calls(self):
        now = time.time()
        path = self.write_state(make_snapshot(devices={
            "dev-1": {
                "status": status_payload(),
                "status_received_at": now - 5,
                "events": [event_payload("key", 3),
                           event_payload("pickup", 2)],
            }
        }))
        async with stdio_client(self.params(path)) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                tools = await session.list_tools()
                names = {tool.name for tool in tools.tools}
                self.assertIn("get_smartguard_status", names)
                self.assertIn("get_recent_smartguard_events", names)

                status_result = await session.call_tool(
                    "get_smartguard_status", {})
                self.assertFalse(getattr(status_result, "isError", False))
                status_payload_result = json.loads(
                    status_result.content[0].text)
                self.assertTrue(status_payload_result["available"])
                self.assertEqual(status_payload_result["state"], "待机")
                self.assertTrue(status_payload_result["fresh"])

                events_result = await session.call_tool(
                    "get_recent_smartguard_events", {"limit": 2})
                self.assertFalse(getattr(events_result, "isError", False))
                events_payload = json.loads(events_result.content[0].text)
                self.assertEqual(events_payload["count"], 2)
                self.assertEqual(events_payload["events"][0]["event_type"],
                                 "key")

    async def test_invalid_limit_rejected(self):
        path = self.write_state(make_snapshot())
        async with stdio_client(self.params(path)) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                try:
                    result = await session.call_tool(
                        "get_recent_smartguard_events", {"limit": 0})
                    self.assertTrue(getattr(result, "isError", False))
                except Exception:
                    pass

    async def test_missing_cache_over_mcp(self):
        missing = os.path.join(tempfile.gettempdir(),
                               "smartguard-no-cache-test.json")
        async with stdio_client(self.params(missing)) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                result = await session.call_tool(
                    "get_smartguard_status", {})
                payload = json.loads(result.content[0].text)
                self.assertFalse(payload["available"])
                self.assertEqual(payload["reason"], "no_cache_file")


if __name__ == "__main__":
    unittest.main(verbosity=2)
