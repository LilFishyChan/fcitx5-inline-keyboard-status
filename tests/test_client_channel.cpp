// client-channel 的单元测试。
//
// 覆盖的是「写这一行到底算不算连接废了」这类分支 —— 之前 addon 在这里
// 出过事故（对已断开的 fd 写，sd-event 报 EBADF，fcitx5 直接 abort），
// 所以这条路径值得逐个情况钉住。
#include "client-channel.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what, int line) {
    if (!ok) {
        std::printf("FAIL (line %d): %s\n", line, what);
        ++failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// 造一对已连通的 unix socket，sv[0] 是 addon 侧，sv[1] 是客户端侧。
bool makePair(int sv[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
}

void closePair(int sv[2]) {
    ::close(sv[0]);
    ::close(sv[1]);
}

std::string readAll(int fd, size_t expect) {
    std::string out;
    out.resize(expect);
    const ssize_t n = ::read(fd, out.data(), expect);
    if (n <= 0) {
        return {};
    }
    out.resize(static_cast<size_t>(n));
    return out;
}

// ---------------- focus 消息 ----------------

void testFocusMessageFormat() {
    CHECK(inline_keyboard::focusMessage(true) == "focus:1");
    CHECK(inline_keyboard::focusMessage(false) == "focus:0");
    // 前缀必须能被插件的解析器识别（和 state:/imlist: 同一个协议族）
    CHECK(inline_keyboard::focusMessage(true).rfind("focus:", 0) == 0);
}

// ---------------- 写结果的分类 ----------------

void testWriteIsFatal() {
    // 写成功了 / 部分写，都不算致命
    CHECK(!inline_keyboard::writeIsFatal(0, 0));
    CHECK(!inline_keyboard::writeIsFatal(12, 0));

    // 这几种只是「现在写不进去」，连接还活着，不能关
    CHECK(!inline_keyboard::writeIsFatal(-1, EAGAIN));
    CHECK(!inline_keyboard::writeIsFatal(-1, EWOULDBLOCK));
    CHECK(!inline_keyboard::writeIsFatal(-1, EINTR));

    // 这几种说明连接废了
    CHECK(inline_keyboard::writeIsFatal(-1, EPIPE));
    CHECK(inline_keyboard::writeIsFatal(-1, EBADF));
    CHECK(inline_keyboard::writeIsFatal(-1, ECONNRESET));
    CHECK(inline_keyboard::writeIsFatal(-1, ENOTCONN));
}

// ---------------- 正常写 ----------------

void testWriteDeliversLine() {
    int sv[2];
    CHECK(makePair(sv));
    CHECK(inline_keyboard::writeLineToClient(sv[0], "focus:1"));
    const std::string got = readAll(sv[1], 64);
    // 自动补换行：插件是按行切消息的
    CHECK(got == "focus:1\n");
    closePair(sv);
}

void testWriteKeepsOrder() {
    int sv[2];
    CHECK(makePair(sv));
    CHECK(inline_keyboard::writeLineToClient(sv[0], "focus:1"));
    CHECK(inline_keyboard::writeLineToClient(sv[0], "focus:0"));
    CHECK(inline_keyboard::writeLineToClient(sv[0], "state:{}"));
    const std::string got = readAll(sv[1], 128);
    CHECK(got == "focus:1\nfocus:0\nstate:{}\n");
    closePair(sv);
}

// ---------------- 异常 fd ----------------

void testInvalidFd() {
    CHECK(!inline_keyboard::writeLineToClient(-1, "focus:1"));
}

void testClosedLocalFd() {
    int sv[2];
    CHECK(makePair(sv));
    ::close(sv[0]);
    // 本地 fd 已关：EBADF，必须报废而不是继续写
    CHECK(!inline_keyboard::writeLineToClient(sv[0], "focus:1"));
    ::close(sv[1]);
}

// 对端关掉。这里是 SIGPIPE 的现场：
// 用 write 的话进程会被信号打死，MSG_NOSIGNAL 把它变成 EPIPE。
void testPeerClosed() {
    std::signal(SIGPIPE, SIG_IGN);
    int sv[2];
    CHECK(makePair(sv));
    ::close(sv[1]); // 对端消失

    bool sawFatal = false;
    // 第一次可能还能写进内核缓冲区，多写几次必然 EPIPE
    for (int i = 0; i < 64; ++i) {
        if (!inline_keyboard::writeLineToClient(sv[0], "focus:0")) {
            sawFatal = true;
            break;
        }
    }
    CHECK(sawFatal);
    ::close(sv[0]);
}

// ---------------- 连续 focus 抖动 ----------------

// 模拟插件在键盘开关时反复连接 / 断开：每一轮独立一对 socket，
// 写焦点消息，确认没有残留状态串到下一轮。
void testRepeatedSessions() {
    for (int round = 0; round < 5; ++round) {
        int sv[2];
        CHECK(makePair(sv));
        CHECK(inline_keyboard::writeLineToClient(sv[0], inline_keyboard::focusMessage(true)));
        const std::string got = readAll(sv[1], 64);
        CHECK(got == "focus:1\n");
        closePair(sv);
    }
}

// ---------------- 连接活性（UI 归属的唯一判据） ----------------

void testClientAlive() {
    constexpr int64_t kTimeout = 5000000; // 5s
    constexpr int64_t kNow = 100000000;

    // fd 不在 / 已排队关闭：无论时间都算死
    CHECK(!inline_keyboard::clientIsAlive(false, false, kNow, kNow, kTimeout));
    CHECK(!inline_keyboard::clientIsAlive(true, true, kNow, kNow, kTimeout));

    // 刚收敛、还没收到任何消息：给一个完整窗口，不算超时
    CHECK(inline_keyboard::clientIsAlive(true, false, kNow, 0, kTimeout));

    // 刚收到消息
    CHECK(inline_keyboard::clientIsAlive(true, false, kNow, kNow, kTimeout));

    // 4.9s 前收到：还活着
    CHECK(inline_keyboard::clientIsAlive(true, false, kNow, kNow - 4900000, kTimeout));

    // 整 5s 前收到：已超时（边界取「小于」）
    CHECK(!inline_keyboard::clientIsAlive(true, false, kNow, kNow - 5000000, kTimeout));

    // 30s 没消息：早就死了
    CHECK(!inline_keyboard::clientIsAlive(true, false, kNow, kNow - 30000000, kTimeout));
}

// 心跳周期 2s、超时 5s 的组合：连续两次心跳没回应才判死，
// 单次抖动（丢一个 ping）不该让键盘的 UI 归属翻转。
void testHeartbeatTolerance() {
    constexpr int64_t kHb = 2000000;
    constexpr int64_t kTimeout = 5000000;
    constexpr int64_t t0 = 50000000;

    // 最后一次收到消息在 t0，检查 t0 + 1 个心跳：活着
    CHECK(inline_keyboard::clientIsAlive(true, false, t0 + kHb, t0, kTimeout));
    // t0 + 2 个心跳：活着（= 4s < 5s）
    CHECK(inline_keyboard::clientIsAlive(true, false, t0 + 2 * kHb, t0, kTimeout));
    // t0 + 3 个心跳：判死（= 6s > 5s）
    CHECK(!inline_keyboard::clientIsAlive(true, false, t0 + 3 * kHb, t0, kTimeout));
}

} // namespace

int main() {
    testFocusMessageFormat();
    testWriteIsFatal();
    testWriteDeliversLine();
    testWriteKeepsOrder();
    testInvalidFd();
    testClosedLocalFd();
    testPeerClosed();
    testRepeatedSessions();
    testClientAlive();
    testHeartbeatTolerance();

    if (failures == 0) {
        std::printf("all client-channel tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
