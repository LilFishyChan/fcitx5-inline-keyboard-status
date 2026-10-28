// 候选数据的序列化：把「按 candidateFromAll 顺序拿到的词」变成发给插件的 JSON。
//
// 独立于 fcitx5，纯函数，可以直接单测（tests/test_candidate_payload.cpp）。
// 之所以要单独抽出来：这是插件和 addon 之间的唯一数据契约，
// 下标错一位、转义漏一个字符，插件侧就会「选 A 提交 B」，
// 而且没有任何日志能定位。

#ifndef INLINE_KEYBOARD_CANDIDATE_PAYLOAD_H
#define INLINE_KEYBOARD_CANDIDATE_PAYLOAD_H

#include <string>
#include <vector>

namespace inline_keyboard {

// 把字符串转义成可以放进 JSON 双引号里的内容。
//
// 处理双引号、反斜杠、常见控制字符（\b \f \n \r \t），
// 其余 < 0x20 的字符输出 \u00XX。UTF-8 多字节原样保留 ——
// JSON 允许直接放，转成 \u 反而会让插件侧多一层 decode。
std::string escapeJsonString(const std::string &input);

// 序列化成 [{"i":0,"t":"你"},{"i":1,"t":"泥"},...]
//
// i 是这一项在**原始候选表**里的下标（candidateFromAll 的参数），
// 必须和传入顺序严格对应：显示层会做去重和过滤，
// 只有这个下标才能选回真正想要的那一项。
std::string buildCandidatePayload(const std::vector<std::string> &words);

} // namespace inline_keyboard

#endif // INLINE_KEYBOARD_CANDIDATE_PAYLOAD_H
