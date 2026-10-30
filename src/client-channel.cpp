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
