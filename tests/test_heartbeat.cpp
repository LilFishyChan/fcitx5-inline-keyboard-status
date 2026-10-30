// 心跳调度的单元测试。
//
// 重点是把两个真实回归钉死：
//   · 心跳只响一次（sd-event 的 time source 触发后被 disarm）
//   · 第二个连接完全没有心跳（timer 已存在就不重排）
// 这两个在真机上的表现都是「用着用着就不响应」，很难从现象反推。
#include "heartbeat.h"

#include <cstdio>

namespace {

using inline_keyboard::HeartbeatState;

int failures = 0;

void check(bool ok, const char *what, int line) {
    if (!ok) {
        std::printf("FAIL (line %d): %s\n", line, what);
        ++failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

using Tick = HeartbeatState::Tick;

// 时间轴用一个整数微秒游标推进，读起来比绝对时间清楚。
struct Clock {
    int64_t us = 10000000;
    void advance(int64_t delta) { us += delta; }
};

// ---------------- 基本的 2s 节拍 ----------------

void testTickOnlyWhenDue() {
    HeartbeatState hb;
    Clock c;
    hb.onAccept(c.us);

    // 刚接上，还没到 2s：什么都不做
    CHECK(hb.onTick(c.us) == Tick::None);
    c.advance(1999999);
    CHECK(hb.onTick(c.us) == Tick::None);

    // 到 2s：发 ping
    c.advance(1);
    CHECK(hb.onTick(c.us) == Tick::Ping);
    CHECK(hb.pingSeq() == 1);
}

// 回归 1：心跳必须一直响，不能只响第一拍
void testHeartbeatRepeats() {
    HeartbeatState hb;
    Clock c;
    hb.onAccept(c.us);

    int pings = 0;
    // 推进 10 秒，每 0.5s 试一次节拍（模拟 timer 的粗粒度唤醒）
    for (int i = 0; i < 20; ++i) {
        c.advance(500000);
        if (hb.onTick(c.us) == Tick::Ping) {
            ++pings;
            hb.onData(c.us); // 对端正常回应
        }
    }
    // 10s / 2s = 5 拍
    CHECK(pings == 5);
    CHECK(hb.pingSeq() == 5);
}

// 回归 2：连接重建之后心跳要重新跑起来
void testHeartbeatRestartsOnReconnect() {
    HeartbeatState hb;
    Clock c;

    // 第一个连接：响两拍后断开
    hb.onAccept(c.us);
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping);
    hb.onData(c.us);
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping);
    hb.onClosed();
    CHECK(!hb.active());
    CHECK(hb.onTick(c.us) == Tick::None);

    // 第二个连接：必须重新开始计时、重新发 ping
    c.advance(1000000);
    hb.onAccept(c.us);
    CHECK(hb.active());
    CHECK(hb.pingSeq() == 0); // 序号归零
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping);
    CHECK(hb.pingSeq() == 1);
}

// ---------------- 超时 ----------------

void testTimeoutWithoutPong() {
    HeartbeatState hb;
    Clock c;
    hb.onAccept(c.us);

    // 对端一次都不回应：2s 发 ping，4s 再发，6s 时超过 5s 窗口 → 判死
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping);
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping);
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Timeout);
    // 判死之后不再 ping，也不再报超时
    CHECK(!hb.active());
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::None);
}

void testNoTimeoutWhilePongArrives() {
    HeartbeatState hb;
    Clock c;
    hb.onAccept(c.us);

    // 每拍都回 pong：一直活着，永远不判死
    for (int i = 0; i < 10; ++i) {
        c.advance(HeartbeatState::kIntervalUs);
        CHECK(hb.onTick(c.us) == Tick::Ping);
        hb.onData(c.us);
    }
    CHECK(hb.active());
    CHECK(hb.pingSeq() == 10);
}

// 丢一个 ping 不该判死（2s 间隔、5s 超时，允许丢一拍）
void testSingleMissedPongTolerated() {
    HeartbeatState hb;
    Clock c;
    hb.onAccept(c.us);

    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping);
    hb.onData(c.us);

    // 这一拍不回
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping); // 4s 时，最后消息在 2s，还活着

    // 下一拍补回来
    hb.onData(c.us);
    c.advance(HeartbeatState::kIntervalUs);
    CHECK(hb.onTick(c.us) == Tick::Ping); // 6s 时，最后消息在 4s，还活着
    CHECK(hb.active());
}

// ---------------- 边界 ----------------

void testClosedState() {
    HeartbeatState hb;
    Clock c;
    // 从来没接上过
    CHECK(!hb.active());
    CHECK(hb.onTick(c.us) == Tick::None);
    hb.onData(c.us); // 不该崩，也不该激活
    CHECK(!hb.active());
}

void testSeqRestartsEachConnection() {
    HeartbeatState hb;
    Clock c;
    for (int round = 1; round <= 3; ++round) {
        hb.onAccept(c.us);
        c.advance(HeartbeatState::kIntervalUs);
        CHECK(hb.onTick(c.us) == Tick::Ping);
        CHECK(hb.pingSeq() == 1); // 每轮都从 1 开始
        hb.onClosed();
    }
}

} // namespace

int main() {
    testTickOnlyWhenDue();
    testHeartbeatRepeats();
    testHeartbeatRestartsOnReconnect();
    testTimeoutWithoutPong();
    testNoTimeoutWhilePongArrives();
    testSingleMissedPongTolerated();
    testClosedState();
    testSeqRestartsEachConnection();

    if (failures == 0) {
        std::printf("all heartbeat tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
