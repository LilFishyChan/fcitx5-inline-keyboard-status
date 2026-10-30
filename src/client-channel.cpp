#include "client-channel.h"

#include <cerrno>
#include <sys/socket.h>

namespace inline_keyboard {

std::string focusMessage(bool focused) {
    return focused ? "focus:1" : "focus:0";
}

bool writeIsFatal(ssize_t written, int err) {
    if (written >= 0) {
        return false;
    }
    return err != EAGAIN && err != EWOULDBLOCK && err != EINTR;
}

bool clientIsAlive(bool fdOpen, bool closing, int64_t nowUs,
                   int64_t lastSeenUs, int64_t timeoutUs) {
    if (!fdOpen || closing) {
        return false;
    }
    // lastSeenUs 为 0 表示刚 accept、还没收到任何字节：
    // 给它一个完整的超时窗口，不算超时。
    if (lastSeenUs <= 0) {
        return true;
    }
    return (nowUs - lastSeenUs) < timeoutUs;
}

bool writeLineToClient(int fd, const std::string &line) {
    if (fd < 0) {
        return false;
    }
    const std::string data = line + "\n";
    const ssize_t written =
        ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    return !writeIsFatal(written, errno);
}

} // namespace inline_keyboard
