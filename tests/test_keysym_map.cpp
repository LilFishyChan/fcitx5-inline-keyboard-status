// keysym-map.h 的单测。不依赖 fcitx5，直接编译跑的断言程序。
//
// 这张表错一个条目，对应的键在虚拟键盘上就彻底没反应（fcitx5 那边
// keySymFromString 返回 None，事件被丢弃），而且没有任何日志能定位。
#include "keysym-map.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expectEq(const std::string &actual, const std::string &want,
              const char *what) {
    if (actual == want) {
        return;
    }
    std::printf("  FAIL %s: 得到 \"%s\"，期望 \"%s\"\n", what,
                actual.c_str(), want.c_str());
    failures++;
}

} // namespace

int main() {
    using inline_keyboard::toKeysymName;

    // 名字对不上、必须转换的
    expectEq(toKeysymName("Backspace"), "BackSpace", "Backspace");
    expectEq(toKeysymName("Enter"), "Return", "Enter");
    expectEq(toKeysymName("Caps Lock"), "Caps_Lock", "Caps Lock");
    expectEq(toKeysymName("Del"), "Delete", "Del");
    expectEq(toKeysymName("Esc"), "Escape", "Esc");
    expectEq(toKeysymName("Ctrl"), "Control_L", "Ctrl");
    expectEq(toKeysymName("Win"), "Super_L", "Win");
    expectEq(toKeysymName("Alt"), "Alt_L", "Alt");
    expectEq(toKeysymName("Shift"), "Shift_L", "Shift");
    // 空格键在布局里没有显示名
    expectEq(toKeysymName(""), "space", "空字符串");

    // 两边名字一致的，必须原样透传
    expectEq(toKeysymName("a"), "a", "小写字母");
    expectEq(toKeysymName("A"), "A", "大写字母");
    expectEq(toKeysymName("1"), "1", "数字");
    expectEq(toKeysymName("Tab"), "Tab", "Tab");
    expectEq(toKeysymName("F1"), "F1", "功能键");
    expectEq(toKeysymName("space"), "space", "本来就是 space");
    expectEq(toKeysymName("Escape"), "Escape", "本来就是 Escape");

    // 不认识的也不能崩，原样返回让上游自己判断（fcitx5 会对无效名返回 None）
    expectEq(toKeysymName("不存在的键"), "不存在的键", "未知名字");

    if (failures == 0) {
        std::printf("keysym-map: 全部通过\n");
        return 0;
    }
    std::printf("keysym-map: %d 项失败\n", failures);
    return 1;
}
