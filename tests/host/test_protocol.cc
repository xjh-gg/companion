/* 主机侧单元测试：直接编译真实的 protocol.cc，
 * 仅替换日志与 cJSON 类型声明，验证超时与错误标志的原子访问语义。 */
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "protocol.h"

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

class TestProtocol : public Protocol {
public:
    bool Start() override { return true; }
    bool OpenAudioChannel() override { return true; }
    void CloseAudioChannel() override {}
    bool IsAudioChannelOpened() const override { return true; }
    bool SendAudio(std::unique_ptr<AudioStreamPacket>) override { return true; }

    using Protocol::IsTimeout;
    using Protocol::SetError;
    using Protocol::UpdateLastIncomingTime;

    bool ErrorFlag() const { return error_occurred_.load(); }
    void SetLastMs(uint32_t ms) { last_incoming_ms_.store(ms); }

protected:
    bool SendText(const std::string&) override { return true; }
};

static uint32_t NowMs()
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(ms);
}

int main(void)
{
    TestProtocol protocol;

    CHECK("初始无错误标志", !protocol.ErrorFlag());
    protocol.SetError("test");
    CHECK("SetError 置位错误标志", protocol.ErrorFlag());

    uint32_t now = NowMs();
    protocol.SetLastMs(now - 60000);
    CHECK("最近 60s 有下行消息不超时", !protocol.IsTimeout());
    protocol.SetLastMs(now - 121000);
    CHECK("121s 无下行消息判定超时", protocol.IsTimeout());

    protocol.UpdateLastIncomingTime();
    CHECK("更新时间后立即恢复不超时", !protocol.IsTimeout());

    printf("\nRESULT: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
