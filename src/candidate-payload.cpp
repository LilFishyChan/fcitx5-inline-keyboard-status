#include "candidate-payload.h"

#include <cstdio>

namespace inline_keyboard {

namespace {

// 期望输入里几乎不会出现的控制字符，按 JSON 规范必须转义
void appendControlEscape(std::string &out, unsigned char byte) {
    switch (byte) {
    case '\b':
        out += "\\b";
        return;
    case '\f':
        out += "\\f";
        return;
    case '\n':
        out += "\\n";
        return;
    case '\r':
        out += "\\r";
        return;
    case '\t':
        out += "\\t";
        return;
    default:
        break;
    }
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
    out += buffer;
}

} // namespace

std::string escapeJsonString(const std::string &input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char raw : input) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            if (byte < 0x20) {
                appendControlEscape(out, byte);
            } else {
                // UTF-8 其余字节（>= 0x80）原样写入
                out += raw;
            }
        }
    }
    return out;
}

std::string buildCandidatePayload(const std::vector<std::string> &words) {
    std::string json = "[";
    json.reserve(words.size() * 16 + 2);
    for (std::size_t index = 0; index < words.size(); index++) {
        if (index) {
            json += ",";
        }
        json += "{\"i\":" + std::to_string(index) + ",\"t\":\"" +
                escapeJsonString(words[index]) + "\"}";
    }
    json += "]";
    return json;
}

} // namespace inline_keyboard
