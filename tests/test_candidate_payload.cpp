// candidate-payload.h 的单测。不依赖 fcitx5，直接编译跑的断言程序。
//
// 这是 addon 和插件之间唯一的数据契约。下标错一位，插件侧就「选 A 提交 B」；
// 转义漏一个字符，整条 JSON 解析失败、候选列表直接空白。
#include "candidate-payload.h"

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

// 统计子串出现次数，用来断言「下标是 0..N-1 连续且各出现一次」
int countOccurrences(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) {
        return 0;
    }
    int count = 0;
    std::size_t at = 0;
    while ((at = haystack.find(needle, at)) != std::string::npos) {
        count++;
        at += needle.size();
    }
    return count;
}

void testEmptyList() {
    expectEq(inline_keyboard::buildCandidatePayload({}), "[]", "空列表");
}

void testSingleWord() {
    expectEq(inline_keyboard::buildCandidatePayload({"你"}),
             "[{\"i\":0,\"t\":\"你\"}]", "单个中文词");
}

void testIndexesAreSequential() {
    const std::vector<std::string> words = {"你", "泥", "尼"};
    expectEq(inline_keyboard::buildCandidatePayload(words),
             "[{\"i\":0,\"t\":\"你\"},{\"i\":1,\"t\":\"泥\"},"
             "{\"i\":2,\"t\":\"尼\"}]",
             "下标必须从 0 起严格连续");
}

void testDuplicateWordsKeepTheirOwnIndex() {
    // addon 不去重：同一个词出现两次就推两条，各自带自己的下标。
    // 去重是显示层的事，这里去掉会让下标的含义和 candidateFromAll 脱节。
    const std::vector<std::string> words = {"速", "素", "速"};
    expectEq(inline_keyboard::buildCandidatePayload(words),
             "[{\"i\":0,\"t\":\"速\"},{\"i\":1,\"t\":\"素\"},"
             "{\"i\":2,\"t\":\"速\"}]",
             "重复词保留各自下标");
}

void testEscapeDoubleQuote() {
    expectEq(inline_keyboard::escapeJsonString("a\"b"), "a\\\"b", "双引号");
}

void testEscapeBackslash() {
    expectEq(inline_keyboard::escapeJsonString("a\\b"), "a\\\\b", "反斜杠");
}

void testEscapeWhitespaceControls() {
    expectEq(inline_keyboard::escapeJsonString("a\nb"), "a\\nb", "换行");
    expectEq(inline_keyboard::escapeJsonString("a\tb"), "a\\tb", "制表符");
    expectEq(inline_keyboard::escapeJsonString("a\rb"), "a\\rb", "回车");
    expectEq(inline_keyboard::escapeJsonString("a\bb"), "a\\bb", "退格");
    expectEq(inline_keyboard::escapeJsonString("a\fb"), "a\\fb", "换页");
}

void testEscapeOtherControlBytes() {
    // 0x01 不在 JSON 的短转义里，必须写成 \u0001
    expectEq(inline_keyboard::escapeJsonString(std::string("a\x01") + "b"),
             "a\\u0001b", "其它控制字符");
    expectEq(inline_keyboard::escapeJsonString(std::string("a\x1f") + "b"),
             "a\\u001fb", "0x1f");
}

void testUtf8PassesThrough() {
    // 中文、emoji 原样保留：JSON 允许直接放 UTF-8
    expectEq(inline_keyboard::escapeJsonString("测试输入"), "测试输入", "中文原样");
    expectEq(inline_keyboard::escapeJsonString("😀"), "😀", "emoji 原样");
}

void testLongWordNotTruncated() {
    const std::string longWord(300, 'x');
    const std::string payload = inline_keyboard::buildCandidatePayload({longWord});
    expectEq(std::to_string(countOccurrences(payload, "x")), "300",
             "长词不截断");
}

void testManyWordsAllPresent() {
    std::vector<std::string> words;
    for (int index = 0; index < 1000; index++) {
        words.push_back("w" + std::to_string(index));
    }
    const std::string payload = inline_keyboard::buildCandidatePayload(words);
    expectEq(std::to_string(countOccurrences(payload, "\"i\":")), "1000",
             "1000 项全部序列化");
    // 最后一项的下标必须是 999
    expectEq(std::to_string(payload.find("\"i\":999") != std::string::npos), "1",
             "最后一项下标 999");
}

void testQuotesInWordDoNotBreakJson() {
    const std::string payload =
        inline_keyboard::buildCandidatePayload({"he said \"hi\"", "a\\b"});
    expectEq(payload,
             "[{\"i\":0,\"t\":\"he said \\\"hi\\\"\"},"
             "{\"i\":1,\"t\":\"a\\\\b\"}]",
             "含引号/反斜杠的词");
}

} // namespace

int main() {
    testEmptyList();
    testSingleWord();
    testIndexesAreSequential();
    testDuplicateWordsKeepTheirOwnIndex();
    testEscapeDoubleQuote();
    testEscapeBackslash();
    testEscapeWhitespaceControls();
    testEscapeOtherControlBytes();
    testUtf8PassesThrough();
    testLongWordNotTruncated();
    testManyWordsAllPresent();
    testQuotesInWordDoNotBreakJson();

    std::printf("candidate-payload: %d 项检查，%d 个失败\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
