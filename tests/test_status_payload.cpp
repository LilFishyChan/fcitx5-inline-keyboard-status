// status-payload.h 的单测。不依赖 fcitx5，直接编译跑的断言程序。
//
// 这段数据决定键盘里那排输入法开关长什么样：顺序错、状态反、转义漏，
// 用户看到的就是「点了简化变繁体」或者整条 JSON 解析失败。
#include "status-payload.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void expectEq(const std::string &actual, const std::string &want,
              const char *what) {
    checks++;
    if (actual == want) {
        return;
    }
    std::printf("  FAIL %s\n    得到: %s\n    期望: %s\n", what, actual.c_str(),
                want.c_str());
    failures++;
}

inline_keyboard::StatusItem makeItem(const std::string &name,
                                     const std::string &text) {
    inline_keyboard::StatusItem item;
    item.name = name;
    item.text = text;
    return item;
}

void testEmpty() {
    expectEq(inline_keyboard::buildStatusPayload({}), "[]", "空列表");
}

void testSingle() {
    expectEq(inline_keyboard::buildStatusPayload({makeItem("a", "A")}),
             "[{\"n\":\"a\",\"t\":\"A\"}]", "单项");
}

void testCheckedOnlyWhenTrue() {
    auto item = makeItem("simplification", "简体");
    item.checked = true;
    expectEq(inline_keyboard::buildStatusPayload({item}),
             "[{\"n\":\"simplification\",\"t\":\"简体\",\"c\":1}]", "选中项");

    auto off = makeItem("simplification", "繁体");
    expectEq(inline_keyboard::buildStatusPayload({off}),
             "[{\"n\":\"simplification\",\"t\":\"繁体\"}]", "未选中不写 c");
}

void testSeparator() {
    auto item = makeItem("sep", "");
    item.separator = true;
    expectEq(inline_keyboard::buildStatusPayload({item}),
             "[{\"n\":\"sep\",\"t\":\"\",\"s\":1}]", "分隔线");
}

void testIcon() {
    auto item = makeItem("emoji", "表情开");
    item.icon = "/usr/share/icons/emoji.svg";
    expectEq(inline_keyboard::buildStatusPayload({item}),
             "[{\"n\":\"emoji\",\"t\":\"表情开\",\"i\":\"/usr/share/icons/"
             "emoji.svg\"}]",
             "图标");
}

void testChildren() {
    auto parent = makeItem("wanxiang", "万象拼音");
    parent.children.push_back(makeItem("wanxiang_a", "方案 A"));
    parent.children.push_back(makeItem("wanxiang_b", "方案 B"));
    expectEq(inline_keyboard::buildStatusPayload({parent}),
             "[{\"n\":\"wanxiang\",\"t\":\"万象拼音\",\"k\":[{\"n\":\"wanxiang_a\","
             "\"t\":\"方案 A\"},{\"n\":\"wanxiang_b\",\"t\":\"方案 B\"}]}]",
             "子菜单");
}

void testEscaping() {
    expectEq(inline_keyboard::buildStatusPayload({makeItem("q", "a\"b")}),
             "[{\"n\":\"q\",\"t\":\"a\\\"b\"}]", "引号转义");
    expectEq(inline_keyboard::buildStatusPayload({makeItem("q", "a\\b")}),
             "[{\"n\":\"q\",\"t\":\"a\\\\b\"}]", "反斜杠转义");
}

void testOrderPreserved() {
    const std::string json = inline_keyboard::buildStatusPayload(
        {makeItem("a", "1"), makeItem("b", "2"), makeItem("c", "3")});
    // 顺序必须和 StatusArea 给的一致，否则开关的位置会跳
    expectEq(json,
             "[{\"n\":\"a\",\"t\":\"1\"},{\"n\":\"b\",\"t\":\"2\"},{\"n\":"
             "\"c\",\"t\":\"3\"}]",
             "顺序保持");
}

void testEmptyNameStillSerialized() {
    // name 为空也要照常输出，插件侧按它回查不到就跳过，不能整条坏掉
    expectEq(inline_keyboard::buildStatusPayload({makeItem("", "无名字")}),
             "[{\"n\":\"\",\"t\":\"无名字\"}]", "空 name");
}

} // namespace

int main() {
    testEmpty();
    testSingle();
    testCheckedOnlyWhenTrue();
    testSeparator();
    testIcon();
    testChildren();
    testEscaping();
    testOrderPreserved();
    testEmptyNameStillSerialized();

    std::printf("status-payload: %d 项检查，%d 个失败\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
