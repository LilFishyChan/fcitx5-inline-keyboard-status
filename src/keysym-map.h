// 键盘布局里的键名 → X keysym 名。
//
// 布局（layouts/*.js）里存的是给人看的显示名（"Backspace" / "Enter" / "Caps Lock"），
// 而 fcitx5 的 Key::keySymFromString() 只认标准 X keysym（"BackSpace" / "Return" /
// "Caps_Lock"），大小写和分隔符都不一样。这张表就是那层转换。
//
// 单独成文件是为了能独立编译测试：错一个别名，那个键在虚拟键盘上就完全没反应，
// 而这种问题没有任何运行时日志能告诉你在哪——只能靠单测钉住。
#pragma once

#include <string>
#include <unordered_map>

namespace inline_keyboard {

// 只列「布局名 ≠ keysym 名」的那些；查不到就原样返回（"a" / "Tab" / "F1" 这类
// 两边一致的不用列）。
inline std::string toKeysymName(const std::string &name) {
    static const std::unordered_map<std::string, std::string> alias = {
        {"Backspace", "BackSpace"},
        {"Enter", "Return"},
        {"Caps Lock", "Caps_Lock"},
        {"Del", "Delete"},
        {"Esc", "Escape"},
        {"Ctrl", "Control_L"},
        {"Win", "Super_L"},
        {"Alt", "Alt_L"},
        {"Shift", "Shift_L"},
        {"", "space"},   // 空格键在布局里没有显示名
    };
    auto it = alias.find(name);
    return it != alias.end() ? it->second : name;
}

} // namespace inline_keyboard
