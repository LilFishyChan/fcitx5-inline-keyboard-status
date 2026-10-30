// 心跳调度：决定「这一拍该发 ping 还是该判死」，不碰 timer 和 socket。
//
// 抽出来的理由：这段逻辑原本埋在 addon 的 timer 回调里，两个真实回归都是
// 它出的 ——
//   1. sd-event 的 time source 触发后被 disarm，心跳只响第一拍；
//   2. 「timer 已存在就不重排」让第二个连接完全没有心跳。
// 这两种都表现为「键盘用着用着就不响应了」，靠读代码看不出来，得有单测钉住。
#pragma once

#include <cstdint>

namespace inline_keyboard {

class HeartbeatState {
public:
    static constexpr int64_t kIntervalUs = 2000000; // 2s 一拍
    static constexpr int64_t kTimeoutUs = 5000000;  // 5s 没消息判死

    // 每拍该做什么
    enum class Tick {
        None,   // 还没到点，或者根本没有连接
        Ping,   // 发一条 ping
        Timeout // 对端在超时窗口里一句话都没说，该断开
    };

    // 新连接：重新开始计时，序号归零
    void onAccept(int64_t nowUs);
    // 收到对端任何字节（pong / 按键 / 命令都算）
    void onData(int64_t nowUs);
    // 连接结束
    void onClosed();

    // 心跳节拍。addon 每 kIntervalUs 调一次。
    Tick onTick(int64_t nowUs);

    bool active() const { return active_; }
    uint64_t pingSeq() const { return pingSeq_; }
    int64_t lastSeenUs() const { return lastSeenUs_; }
    int64_t nextTickUs() const { return nextTickUs_; }

private:
    bool active_ = false;
    int64_t lastSeenUs_ = 0;
    int64_t nextTickUs_ = 0;
    uint64_t pingSeq_ = 0;
};

} // namespace inline_keyboard
