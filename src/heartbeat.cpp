#include "heartbeat.h"

#include "client-channel.h"

namespace inline_keyboard {

void HeartbeatState::onAccept(int64_t nowUs) {
    active_ = true;
    lastSeenUs_ = nowUs;
    nextTickUs_ = nowUs + kIntervalUs;
    pingSeq_ = 0;
}

void HeartbeatState::onData(int64_t nowUs) {
    if (!active_) {
        return;
    }
    lastSeenUs_ = nowUs;
}

void HeartbeatState::onClosed() {
    active_ = false;
    lastSeenUs_ = 0;
    nextTickUs_ = 0;
}

HeartbeatState::Tick HeartbeatState::onTick(int64_t nowUs) {
    if (!active_) {
        return Tick::None;
    }
    if (nowUs < nextTickUs_) {
        return Tick::None;
    }
    // 排下一拍。这里用「当前时间 + 间隔」而不是「上一拍 + 间隔」：
    // 主循环被卡住一段时间后不该连发一串补 ping。
    nextTickUs_ = nowUs + kIntervalUs;

    // 超时优先于 ping：对端已经僵死了，再往它那儿写没有意义
    if (!clientIsAlive(true, false, nowUs, lastSeenUs_, kTimeoutUs)) {
        active_ = false;
        return Tick::Timeout;
    }
    ++pingSeq_;
    return Tick::Ping;
}

} // namespace inline_keyboard
