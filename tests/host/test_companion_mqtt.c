#include "companion_mqtt_codec.h"

#include <stdio.h>
#include <string.h>

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(name, cond) do { \
    if (cond) { s_pass++; printf("PASS %s\n", name); } \
    else { s_fail++; printf("FAIL %s\n", name); } \
} while (0)

static bool parse(const char *json, companion_mqtt_cmd_t *cmd, char *reason, size_t cap)
{
    return companion_mqtt_parse_command(json, "dev-1", 5000, cmd, reason, cap);
}

int main(void)
{
    companion_mqtt_cmd_t cmd;
    char reason[48];

    const char *valid =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"cmd-1\","
        "\"ttl\":10,\"action\":\"request_status\",\"intent\":\"greet\"}";
    memset(&cmd, 0, sizeof(cmd));
    CHECK("合法命令解析",
          parse(valid, &cmd, reason, sizeof(reason)) &&
          strcmp(cmd.command_id, "cmd-1") == 0 &&
          strcmp(cmd.action, "request_status") == 0 &&
          cmd.ttl_ms == 10000 && cmd.received_ms == 5000);

    const char *valid_chime =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"cmd-2\","
        "\"ttl\":5,\"action\":\"play_chime\"}";
    CHECK("play_chime 允许",
          parse(valid_chime, &cmd, reason, sizeof(reason)) &&
          strcmp(cmd.action, "play_chime") == 0);

    const char *valid_feedback =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"cmd-3\","
        "\"ttl\":10,\"action\":\"play_feedback\",\"feedback\":\"correct\"}";
    CHECK("play_feedback correct 允许",
          parse(valid_feedback, &cmd, reason, sizeof(reason)) &&
          strcmp(cmd.action, "play_feedback") == 0 &&
          strcmp(cmd.feedback, "correct") == 0);

    const char *missing_feedback =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"c\","
        "\"ttl\":5,\"action\":\"play_feedback\"}";
    CHECK("play_feedback 缺少 feedback 拒绝",
          !parse(missing_feedback, &cmd, reason, sizeof(reason)));

    const char *bad_feedback =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"c\","
        "\"ttl\":5,\"action\":\"play_feedback\",\"feedback\":\"laugh\"}";
    CHECK("play_feedback 未知 feedback 拒绝",
          !parse(bad_feedback, &cmd, reason, sizeof(reason)));

    const char *feedback_text =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"c\","
        "\"ttl\":5,\"action\":\"play_feedback\",\"feedback\":\"wrong\","
        "\"speech\":\"arbitrary text must be ignored\"}";
    CHECK("play_feedback 不接受文本而只认枚举",
          parse(feedback_text, &cmd, reason, sizeof(reason)) &&
          strcmp(cmd.feedback, "wrong") == 0);

    CHECK("无效 JSON 拒绝",
          !parse("not-a-json", &cmd, reason, sizeof(reason)));

    const char *bad_schema =
        "{\"schema_version\":2,\"device_id\":\"dev-1\",\"command_id\":\"c\","
        "\"ttl\":5,\"action\":\"noop\"}";
    CHECK("schema_version 非法拒绝",
          !parse(bad_schema, &cmd, reason, sizeof(reason)));

    const char *missing_dev =
        "{\"schema_version\":1,\"command_id\":\"c\",\"ttl\":5,\"action\":\"noop\"}";
    CHECK("缺少 device_id 拒绝",
          !parse(missing_dev, &cmd, reason, sizeof(reason)));

    const char *wrong_dev =
        "{\"schema_version\":1,\"device_id\":\"dev-2\",\"command_id\":\"c\","
        "\"ttl\":5,\"action\":\"noop\"}";
    CHECK("device_id 不匹配拒绝",
          !parse(wrong_dev, &cmd, reason, sizeof(reason)));

    const char *empty_id =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"\","
        "\"ttl\":5,\"action\":\"noop\"}";
    CHECK("空 command_id 拒绝",
          !parse(empty_id, &cmd, reason, sizeof(reason)));

    const char *ttl_zero =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"c\","
        "\"ttl\":0,\"action\":\"noop\"}";
    CHECK("ttl=0 拒绝",
          !parse(ttl_zero, &cmd, reason, sizeof(reason)));

    const char *ttl_big =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"c\","
        "\"ttl\":301,\"action\":\"noop\"}";
    CHECK("ttl 超上限拒绝",
          !parse(ttl_big, &cmd, reason, sizeof(reason)));

    const char *unknown_action =
        "{\"schema_version\":1,\"device_id\":\"dev-1\",\"command_id\":\"c\","
        "\"ttl\":5,\"action\":\"play_prompt\",\"speech\":\"hello\"}";
    CHECK("未知 action 拒绝",
          !parse(unknown_action, &cmd, reason, sizeof(reason)));

    char oversized[1200];
    memset(oversized, 'a', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    CHECK("超长 payload 拒绝",
          !parse(oversized, &cmd, reason, sizeof(reason)));

    printf("RESULT: PASS=%d FAIL=%d\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
