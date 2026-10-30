/* 主机侧单元测试：直接编译真实的 state_machine.c 与 event_queue.c，
 * 仅用桩件替换 FreeRTOS tick/队列、日志和硬件副作用接口。
 * 不依赖 ESP32、传感器、音频或网络。 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "companion_audio.h"
#include "companion_event_log.h"
#include "companion_led.h"
#include "event_queue.h"
#include "motion_detect.h"
#include "state_machine.h"

/* ---- 虚拟时钟与副作用记录 ---- */
static uint32_t g_tick = 0;
static int g_reminder_requests = 0;
static bool g_reminder_accept = true;
static int g_led_requests = 0;
static companion_led_mode_t g_last_led = COMPANION_LED_OFF;

TickType_t xTaskGetTickCount(void)
{
    return g_tick;
}

bool companion_audio_request_reminder(void)
{
    g_reminder_requests++;
    return g_reminder_accept;
}

bool companion_audio_is_busy(void)
{
    return false;
}

bool companion_audio_reminder_pending(void)
{
    return false;
}

void companion_audio_init(void)
{
}

void companion_led_init(void)
{
}

void companion_led_request(companion_led_mode_t mode)
{
    g_led_requests++;
    g_last_led = mode;
}

/* ---- 断言 ---- */
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(name, cond)                                    \
    do {                                                     \
        if (cond) {                                          \
            g_pass++;                                        \
            printf("PASS %s\n", name);                       \
        } else {                                             \
            g_fail++;                                        \
            printf("FAIL %s\n", name);                       \
        }                                                    \
    } while (0)

static void advance_ms(uint32_t ms)
{
    g_tick += ms;
}

static void advance_seconds(uint32_t sec)
{
    for (uint32_t i = 0; i < sec; i++) {
        advance_ms(1000);
        state_machine_tick();
    }
}

static void reset_state(uint32_t start_tick)
{
    g_tick = start_tick;
    state_machine_init();
}

static void test_state_machine(void)
{
    reset_state(1000);

    CHECK("上电进入待机", state_machine_get_state() == STATE_IDLE);
    CHECK("待机犯困剩余时间=60分钟", state_machine_get_sleepy_remaining_sec() == 3600);
    CHECK("初始状态持续时间=0", state_machine_get_state_duration_ms() == 0);

    /* 1. 待机 -> 交互 */
    advance_ms(500);
    state_machine_on_event(EVT_INTERACT_START);
    CHECK("待机收到交互开始进入互动中", state_machine_get_state() == STATE_INTERACT);
    CHECK("交互事件记录为最近事件", state_machine_get_last_event() == EVT_INTERACT_START);
    CHECK("进入互动后状态持续时间=0", state_machine_get_state_duration_ms() == 0);
    advance_ms(5000);
    CHECK("互动状态持续时间按tick增长", state_machine_get_state_duration_ms() == 5000);

    /* 2. 重复交互开始不产生副作用 */
    int led_before = g_led_requests;
    int rem_before = g_reminder_requests;
    state_machine_on_event(EVT_INTERACT_START);
    CHECK("重复交互开始保持互动中", state_machine_get_state() == STATE_INTERACT);
    CHECK("重复交互开始不重复 LED 请求", g_led_requests == led_before);
    CHECK("重复交互开始不重复提醒", g_reminder_requests == rem_before);

    /* 3. 交互期间不累计犯困计时 */
    advance_seconds(120);
    CHECK("交互两分钟仍未进入犯困", state_machine_get_state() == STATE_INTERACT);
    CHECK("交互期间无提醒请求", g_reminder_requests == 0);
    CHECK("交互期间犯困已计时长=0", state_machine_get_sleepy_elapsed_sec() == 0);

    /* 4. 交互结束 -> 被关注 -> 30s 后回待机 */
    state_machine_on_event(EVT_INTERACT_END);
    CHECK("交互结束进入被关注", state_machine_get_state() == STATE_HAPPY);
    advance_ms(29999);
    state_machine_tick();
    CHECK("被关注29999ms仍保持", state_machine_get_state() == STATE_HAPPY);
    advance_ms(1);
    state_machine_tick();
    CHECK("被关注满30s回到待机", state_machine_get_state() == STATE_IDLE);
    CHECK("回到待机后犯困计时清零", state_machine_get_sleepy_elapsed_sec() == 0);

    /* 5. 60 分钟边界：3599s 不到，3600s 触发一次 */
    advance_seconds(3599);
    CHECK("待机3599s未进入犯困", state_machine_get_state() == STATE_IDLE);
    CHECK("待机3599s剩余1s", state_machine_get_sleepy_remaining_sec() == 1);
    advance_seconds(1);
    CHECK("待机3600s进入犯困", state_machine_get_state() == STATE_SLEEPY);
    CHECK("进入犯困请求提醒一次", g_reminder_requests == 1);

    /* 6. 犯困态不重复触发提醒 */
    advance_seconds(600);
    CHECK("犯困态持续10分钟不重复提醒", g_reminder_requests == 1);

    /* 7. 犯困态收到语音交互 -> 互动中 */
    state_machine_on_event(EVT_INTERACT_START);
    CHECK("犯困收到交互开始退出犯困", state_machine_get_state() == STATE_INTERACT);
    state_machine_on_event(EVT_INTERACT_END);
    advance_ms(30000);
    state_machine_tick();
    CHECK("犯困退出后30s正常回待机", state_machine_get_state() == STATE_IDLE);

    /* 8. 犯困态收到活动事件 -> 被关注 */
    advance_seconds(3600);
    CHECK("再次达到犯困阈值", state_machine_get_state() == STATE_SLEEPY);
    CHECK("第二次进入犯困再次请求提醒", g_reminder_requests == 2);
    state_machine_on_event(EVT_HAND_NEAR);
    CHECK("犯困收到手靠近进入被关注", state_machine_get_state() == STATE_HAPPY);

    /* 9. 非法事件不破坏状态 */
    state_machine_on_event((event_t)777);
    CHECK("非法事件保持被关注", state_machine_get_state() == STATE_HAPPY);

    /* 10. tick 回绕：0xFFFFFF00 + 30000ms 跨 32 位边界仍正确计时 */
    g_tick = 0xFFFFFF00u;
    state_machine_on_event(EVT_SHAKE); /* 刷新被关注计时 */
    advance_ms(30000);                 /* 回绕到 0x6FB0 */
    state_machine_tick();
    CHECK("tick回绕后被关注仍按30s超时", state_machine_get_state() == STATE_IDLE);

    /* 11. 提醒被拒绝后状态机不卡死 */
    g_reminder_accept = false;
    reset_state(1000);
    advance_seconds(3600);
    CHECK("提醒被拒仍进入犯困", state_machine_get_state() == STATE_SLEEPY);
    state_machine_on_event(EVT_INTERACT_START);
    CHECK("提醒被拒后交互仍可退出犯困", state_machine_get_state() == STATE_INTERACT);
    g_reminder_accept = true;

    /* 12. 最近活动时间随活动事件刷新 */
    reset_state(5000);
    advance_ms(10000);
    state_machine_on_event(EVT_PICKED_UP);
    CHECK("活动事件刷新最近活动时间", state_machine_get_last_activity_age_ms() == 0);
    advance_ms(2500);
    CHECK("活动时间按单调 tick 增长", state_machine_get_last_activity_age_ms() == 2500);

    /* 13. 犯困提醒有限重试：首次拒绝，30s/60s 重试，受理后停止 */
    g_reminder_accept = false;
    reset_state(1000);
    int rem_base = g_reminder_requests;
    advance_seconds(3600);
    CHECK("首次犯困提醒被拒仍进入犯困",
          state_machine_get_state() == STATE_SLEEPY && g_reminder_requests == rem_base + 1);
    advance_seconds(30);
    CHECK("30s后重试第二次", g_reminder_requests == rem_base + 2);
    g_reminder_accept = true;
    advance_seconds(30);
    CHECK("60s后重试受理", g_reminder_requests == rem_base + 3);
    advance_seconds(60);
    CHECK("受理后不再重试", g_reminder_requests == rem_base + 3);
    g_reminder_accept = true;

    /* 14. 状态机一致性快照：一次读取的字段应来自同一状态 */
    reset_state(1000);
    advance_ms(10000);
    state_machine_snapshot_t snap;
    state_machine_get_snapshot(&snap);
    CHECK("快照状态为待机", snap.state == STATE_IDLE);
    CHECK("快照超时 60 分钟", snap.sleepy_timeout_min == 60);
    CHECK("快照状态时长 10s", snap.state_duration_ms == 10000);
    CHECK("快照犯困已过 10s", snap.sleepy_elapsed_sec == 10);
    CHECK("快照犯困剩余 3590s", snap.sleepy_remaining_sec == 3590);
}

static void test_event_queue(void)
{
    CHECK("队列初始化成功", event_queue_init() == ESP_OK);
    CHECK("重复初始化幂等", event_queue_init() == ESP_OK);

    CHECK("EVT_NONE 被忽略", event_queue_send(EVT_NONE, 0) == ESP_OK);
    CHECK("EVT_NONE 不入队", event_queue_get_pending_count() == 0);

    int ok = 0;
    for (int i = 0; i < 8; i++) {
        if (event_queue_send(EVT_PICKED_UP, 0) == ESP_OK) {
            ok++;
        }
    }
    CHECK("队列容量为8且可填满", ok == 8 && event_queue_get_pending_count() == 8);
    CHECK("队列满返回超时", event_queue_send(EVT_SHAKE, 0) == ESP_ERR_TIMEOUT);
    CHECK("丢弃计数为1", event_queue_get_dropped_count() == 1);
    CHECK("队列满时深度不变", event_queue_get_pending_count() == 8);

    int recv_ok = 0;
    event_t evt = EVT_NONE;
    while (event_queue_recv(&evt, 0)) {
        if (evt == EVT_PICKED_UP) {
            recv_ok++;
        }
    }
    CHECK("按序取出8个事件", recv_ok == 8);
    CHECK("取出后队列为空", event_queue_get_pending_count() == 0);

    CHECK("未知事件可安全入队", event_queue_send((event_t)777, 0) == ESP_OK);
    evt = EVT_NONE;
    CHECK("未知事件可安全取出", event_queue_recv(&evt, 0) && evt == (event_t)777);

    CHECK("按键事件携带键号入队",
          event_queue_send_key(EVT_KEY_PRESSED, 2, 0) == ESP_OK);
    event_t key_evt = EVT_NONE;
    uint8_t key_index = EVENT_KEY_INDEX_NONE;
    CHECK("按键事件取出 KEY2",
          event_queue_recv_ex(&key_evt, &key_index, 0) &&
          key_evt == EVT_KEY_PRESSED && key_index == 2);

    CHECK("非按键事件键号为 NONE",
          event_queue_send(EVT_SHAKE, 0) == ESP_OK);
    key_index = 0;
    CHECK("普通事件 key_index 为 NONE",
          event_queue_recv_ex(&key_evt, &key_index, 0) &&
          key_evt == EVT_SHAKE && key_index == EVENT_KEY_INDEX_NONE);

    bool null_guard = !event_queue_recv(NULL, 0);
    CHECK("空指针接收被拒绝", null_guard);
}

static void test_event_log(void)
{
    companion_event_log_init();
    CHECK("事件日志初始为空", companion_event_log_count() == 0);
    CHECK("事件日志初始累计为0", companion_event_log_total() == 0);

    for (int i = 0; i < 30; i++) {
        companion_event_log_record(EVT_PICKED_UP, (uint32_t)(i * 10),
                                    (uint16_t)(100 + i), (uint16_t)(i % 20), true);
    }
    CHECK("事件日志累计30条", companion_event_log_total() == 30);
    CHECK("事件日志环形容量24", companion_event_log_count() == COMPANION_EVENT_LOG_CAP);

    companion_event_record_t rec[COMPANION_EVENT_LOG_CAP];
    uint32_t n = companion_event_log_copy(rec, 8);
    CHECK("按限制返回8条", n == 8);
    CHECK("最新事件在前", rec[0].seq == 30 && rec[0].uptime_ms == 290);
    CHECK("最新事件携带ALS值", rec[0].als == 129);
    CHECK("第8条为第23个事件", rec[7].seq == 23);

    n = companion_event_log_copy(rec, COMPANION_EVENT_LOG_CAP);
    CHECK("最多复制24条", n == COMPANION_EVENT_LOG_CAP);
    CHECK("最旧保留事件为第7个", rec[23].seq == 7);

    uint32_t total_before = companion_event_log_total();
    companion_event_log_record(EVT_NONE, 999, 0, 0, false);
    CHECK("EVT_NONE不入历史", companion_event_log_total() == total_before);

    companion_event_log_record(EVT_DARK, 1234, 42, 7, false);
    n = companion_event_log_copy(rec, 1);
    CHECK("无效数据标志被保留", n == 1 && rec[0].valid == false);
    CHECK("事件类型正确", rec[0].type == EVT_DARK && rec[0].als == 42);

    CHECK("复制0条返回0", companion_event_log_copy(rec, 0) == 0);
    CHECK("空指针复制返回0", companion_event_log_copy(NULL, 5) == 0);
    n = companion_event_log_copy(rec, COMPANION_EVENT_LOG_CAP + 1);
    CHECK("复制请求超过容量按24条截断", n == COMPANION_EVENT_LOG_CAP);
    bool strictly_descending = true;
    for (uint32_t i = 1; i < n; i++) {
        if (rec[i].seq >= rec[i - 1].seq) {
            strictly_descending = false;
            break;
        }
    }
    CHECK("事件按序号严格倒序返回", strictly_descending);
}

static motion_event_t feed_motion(motion_detect_t *md, float x, float y, float z)
{
    advance_ms(50);
    return motion_detect_update(md, x, y, z);
}

static void test_motion_detect(void)
{
    /* 拿起与锁定解除 */
    g_tick = 1000;
    motion_detect_t *md = motion_detect_create();
    CHECK("运动检测器创建成功", md != NULL);
    motion_event_t ev = MOTION_NONE;
    for (int i = 0; i < 5; i++) {
        ev = feed_motion(md, 0.0f, 0.0f, 1.0f);
    }
    CHECK("静止校准期间不误报", ev == MOTION_NONE);
    ev = feed_motion(md, 0.0f, 0.0f, 0.1f);
    CHECK("z分量骤减触发拿起", ev == MOTION_PICK_UP);
    ev = feed_motion(md, 0.0f, 0.0f, 0.1f);
    CHECK("拿起锁定期间不重复触发", ev == MOTION_NONE);

    advance_ms(350); /* 累计满 400ms 锁定 */
    ev = motion_detect_update(md, 0.0f, 0.0f, 1.0f);
    CHECK("锁定期满后不再重复上报", ev == MOTION_NONE);
    for (int i = 0; i < 5; i++) {
        feed_motion(md, 0.0f, 0.0f, 1.0f);
    }
    ev = feed_motion(md, 0.0f, 0.0f, 0.1f);
    CHECK("重新校准后可再次拿起", ev == MOTION_PICK_UP);
    motion_detect_destroy(md);

    /* 摇晃：1 秒窗口内方向翻转达到阈值 */
    g_tick = 1000;
    md = motion_detect_create();
    bool shake = false;
    for (int i = 0; i < 30; i++) {
        ev = feed_motion(md, (i % 2) ? 2.0f : -2.0f, 0.0f, 0.0f);
        if (ev == MOTION_SHAKE) {
            shake = true;
            break;
        }
    }
    CHECK("方向反复翻转触发摇晃", shake);
    motion_detect_destroy(md);

    /* tick 回绕：锁定截止点未回绕、观察点已回绕时仍能解锁 */
    g_tick = 0xFFFFFD00u;
    md = motion_detect_create();
    for (int i = 0; i < 5; i++) {
        feed_motion(md, 0.0f, 0.0f, 1.0f);
    }
    ev = feed_motion(md, 0.0f, 0.0f, 0.1f);
    CHECK("回绕前拿起正常触发", ev == MOTION_PICK_UP);
    advance_ms(500); /* 跨过 32 位 tick 边界，期间不更新 */
    ev = motion_detect_update(md, 0.0f, 0.0f, 0.1f);
    CHECK("回绕后锁定期正确结束", ev == MOTION_NONE);
    for (int i = 0; i < 5; i++) {
        feed_motion(md, 0.0f, 0.0f, 1.0f);
    }
    ev = feed_motion(md, 0.0f, 0.0f, 0.1f);
    CHECK("回绕后可重新校准并触发拿起", ev == MOTION_PICK_UP);
    motion_detect_destroy(md);
}

int main(void)
{
    printf("=== state machine tests ===\n");
    test_state_machine();
    printf("=== event queue tests ===\n");
    test_event_queue();
    printf("=== event log tests ===\n");
    test_event_log();
    printf("=== motion detect tests ===\n");
    test_motion_detect();

    printf("\nRESULT: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
