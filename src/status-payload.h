// 输入法状态区（StatusArea 的 InputMethod 分组）的序列化。
//
// 图片里那个托盘菜单的中段就是它：rime 的「简体 / 半角→全角 / 表情关→表情开…」。
// 独立于 fcitx5，纯函数，可以直接单测。
//
// 字段名压到一个字符：这段数据会跟着状态推送反复下发，而键盘那边的候选栏本来
// 就窄，能省一点是一点。

#ifndef INLINE_KEYBOARD_STATUS_PAYLOAD_H
#define INLINE_KEYBOARD_STATUS_PAYLOAD_H

#include <string>
#include <vector>

namespace inline_keyboard {

struct StatusItem {
    // 稳定标识，点击时按它回查 Action（不能按显示文本匹配，rime 的文本里带状态）
    std::string name;
    // 显示文字。rime 会自己把状态写进文本，比如「半角→全角」
    std::string text;
    // 图标路径，可能为空
    std::string icon;
    bool checked = false;
    // 画一条分隔线
    bool separator = false;
    // 子菜单（Menu::actions()）
    std::vector<StatusItem> children;
};

// 序列化成 [{"n":"simplification","t":"简体","c":1},...]
//
// 假值不写：c / s / k 只在真的时候出现，插件侧按「缺省即 false」处理。
std::string buildStatusPayload(const std::vector<StatusItem> &items);

} // namespace inline_keyboard

#endif // INLINE_KEYBOARD_STATUS_PAYLOAD_H
