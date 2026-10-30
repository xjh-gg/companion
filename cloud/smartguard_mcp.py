"""SmartGuard 只读 MCP 服务（stdio）。

由官方 mcp_pipe.py 启动并通过 MCP_ENDPOINT 桥接到小智智能体。
本服务只读取消费者写入的本地快照，不连接 MQTT、不发布命令、不操作硬件。
"""

import json
import logging
import sys
from typing import Annotated, Optional

from mcp.server.fastmcp import FastMCP
from pydantic import Field

from button_game import (BUTTON_COUNT, BUTTON_LABELS, DEFAULT_TIMEOUT_S,
                         MAX_TIMEOUT_S, MIN_TIMEOUT_S, ButtonGameStore)
from smartguard_state import MAX_EVENT_LIMIT, query_events, query_status

logging.basicConfig(level=logging.INFO, stream=sys.stderr,
                    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s")
logger = logging.getLogger("SMARTGUARD_MCP")

# 记录云端模型的真实工具调用，供端到端验证使用（不记录 token）。
try:
    import os
    _call_log = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "state", "mcp_tool_calls.log")
    os.makedirs(os.path.dirname(_call_log), exist_ok=True)
    _handler = logging.FileHandler(_call_log, encoding="utf-8")
    _handler.setFormatter(logging.Formatter(
        "%(asctime)s %(levelname)s %(message)s"))
    logger.addHandler(_handler)
except Exception:
    pass

mcp = FastMCP(
    "SmartGuard",
    instructions=("查询 SmartGuard 陪伴设备的缓存状态与最近真实物理事件。"
                  "设备被摇晃、按键、靠近或环境光变化后会产生事件。"
                  "每次开始新一轮对话、被唤醒或用户提到刚发生的动作时，"
                  "必须先调用 get_recent_smartguard_events 获取真实事件，"
                  "再结合事件自然回应，不得臆测未发生的行为。"
                  "数据来自本地 MQTT 消费者，不是设备实时直连；"
                  "查询工具只读，不控制设备。"),
)


def _json_text(payload):
    """返回紧凑 JSON 字符串，避免 MCP 框架格式化后超过 1024 字节。"""
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def _game_store():
    return ButtonGameStore()


@mcp.tool()
def get_smartguard_status(
    device_id: Annotated[
        Optional[str],
        Field(description="设备标识；省略时返回最近有数据的设备"),
    ] = None,
) -> str:
    """查询 SmartGuard 设备最新缓存状态。

    返回设备标识、状态、最近状态时间、新鲜度与传感器摘要。
    没有缓存时返回 available=false 和原因，不会伪造状态。
    数据来自本地 MQTT 消费者缓存，不是设备实时直连。
    返回值为紧凑 JSON 字符串。
    """
    logger.info("get_smartguard_status device_id=%s", bool(device_id))
    return _json_text(query_status(device_id=device_id))

@mcp.tool()
def get_recent_smartguard_events(
    limit: Annotated[
        int,
        Field(ge=1, le=MAX_EVENT_LIMIT,
              description=f"返回事件数量，1-{MAX_EVENT_LIMIT}，默认 5"),
    ] = 5,
    device_id: Annotated[
        Optional[str],
        Field(description="设备标识；省略时使用最近有数据的设备"),
    ] = None,
) -> str:
    """查询 SmartGuard 最近的真实物理事件（摇晃/按键/靠近/明暗等）。

    每轮新会话、被唤醒或用户提到刚发生的动作时，必须先调用本工具。
    返回事件类型、发生时间、新鲜度和已有事件详情，最新在前。
    没有事件时返回空列表和说明；没有缓存时返回 available=false。
    只读工具，不触发任何设备动作。返回值为紧凑 JSON 字符串。
    """
    logger.info("get_recent_smartguard_events limit=%s device=%s",
                limit, bool(device_id))
    return _json_text(query_events(device_id=device_id, limit=limit))


@mcp.tool()
def start_button_game(
    target_button: Annotated[
        Optional[int],
        Field(ge=0, le=BUTTON_COUNT - 1,
              description="目标按键 0-3（对应 KEY0-KEY3）；省略时随机选择"),
    ] = None,
    timeout_seconds: Annotated[
        int,
        Field(ge=MIN_TIMEOUT_S, le=MAX_TIMEOUT_S,
              description=f"游戏超时秒数，{MIN_TIMEOUT_S}-{MAX_TIMEOUT_S}"),
    ] = DEFAULT_TIMEOUT_S,
    device_id: Annotated[
        Optional[str],
        Field(description="设备标识；省略时使用最新有状态的设备"),
    ] = None,
) -> str:
    """开始一轮真实按键互动游戏。

    要求设备状态新鲜可用，否则拒绝开局。返回目标按键和需要告诉用户的话。
    用户真实按键后，设备会按云端判定播放固定语音反馈，不需要再次调用工具。
    只允许固定反馈，不接受任意文本或硬件控制。
    """
    status = query_status(device_id=device_id)
    if not status.get("available"):
        return _json_text({
            "started": False,
            "reason": "device_status_unavailable",
            "detail": status.get("reason"),
            "limitations": status.get("limitations"),
        })
    if not status.get("fresh"):
        return _json_text({
            "started": False,
            "reason": "device_status_stale",
            "device_id": status.get("device_id"),
            "age_seconds": status.get("age_seconds"),
        })
    uptime = status.get("device_uptime_ms")
    if not isinstance(uptime, int) or uptime <= 0:
        return _json_text({
            "started": False,
            "reason": "device_uptime_unavailable",
            "device_id": status.get("device_id"),
        })
    try:
        game = _game_store().start(
            status["device_id"], target_button=target_button,
            timeout_seconds=timeout_seconds, device_uptime_ms=uptime)
    except ValueError as exc:
        return _json_text({"started": False, "reason": str(exc)})
    except Exception as exc:
        logger.error("start_button_game failed: %s", type(exc).__name__)
        return _json_text({"started": False,
                           "reason": "game_store_unavailable"})

    result = {
        "started": True,
        "game_id": game["game_id"],
        "device_id": game["device_id"],
        "target_button": game["target_button"],
        "target_label": game["target_label"],
        "state": game["state"],
        "timeout_seconds": game["timeout_seconds"],
        "expires_at": game["expires_at"],
        "instruction": game["instruction"],
        "reply_instruction": (
            f"请对用户说：我们来玩个小游戏吧，请按一下按键 "
            f"{game['target_button']}（{game['target_label']}）。"
        ),
        "feedback_text": game["feedback_text"],
        "note": "用户真实按键后设备会自动播放固定语音；不要虚构按键结果。",
    }
    return _json_text(result)


@mcp.tool()
def cancel_button_game(
    device_id: Annotated[
        Optional[str],
        Field(description="设备标识；省略时取消最近一轮等待中的游戏"),
    ] = None,
    game_id: Annotated[
        Optional[str],
        Field(description="指定要取消的游戏 ID；省略时按设备取最近一轮"),
    ] = None,
) -> str:
    """取消当前等待中的按键游戏。

    取消后旧按键事件不会再触发反馈；没有活动游戏时明确返回 cancelled=false。
    """
    try:
        result = _game_store().cancel(device_id=device_id, game_id=game_id)
    except Exception as exc:
        logger.error("cancel_button_game failed: %s", type(exc).__name__)
        return _json_text({"cancelled": False,
                           "reason": "game_store_unavailable"})
    return _json_text(result)


if __name__ == "__main__":
    mcp.run(transport="stdio")
