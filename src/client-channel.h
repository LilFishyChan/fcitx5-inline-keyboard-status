// 和客户端 socket 打交道的纯逻辑：不依赖 fcitx5，可以单独测。
//
// 抽出来的理由：写失败怎么算「连接废了」、focus 消息长什么样，
// 这两件事以前散在 addon 的 pushLine / 各个事件回调里，加上 socket
// 的生命周期（延迟关闭）之后，光靠读代码很难确认所有分支都对。
#pragma once

#include <string>
#include <sys/types.h>

namespace inline_keyboard {

// 焦点变化消息。格式跟 state:/imlist: 一样是单行文本协议：
// focus:1 = 输入框拿到焦点，focus:0 = 失焦。
std::string focusMessage(bool focused);

// 写结果是否说明连接已经不可用。
// EAGAIN/EWOULDBLOCK/EINTR 只是「现在写不进去」，连接还活着。
bool writeIsFatal(ssize_t written, int err);

// 连接是否还活着：fd 在、没有排队关闭、并且在超时窗口内收到过对端的消息。
//
// 这是「该不该用键盘内 UI」的唯一判据。抽成纯函数是因为这里一旦判错，
// 表现是候选窗和 classicui 互相抢，或者键盘开着 UI 却被交还 —— 
// 都是很难从现象反推的故障，值得单独测。
bool clientIsAlive(bool fdOpen, bool closing, int64_t nowUs,
                   int64_t lastSeenUs, int64_t timeoutUs);

// 往客户端 fd 写一行（自动补 \n）。
// 返回 false 表示连接已废，调用方应排上关闭。
//
// 用 send + MSG_NOSIGNAL 而不是 write：对端已经关闭时 write 会触发
// SIGPIPE 杀掉整个 fcitx5 进程，而 MSG_NOSIGNAL 把它变成普通的 EPIPE。
bool writeLineToClient(int fd, const std::string &line);

} // namespace inline_keyboard
