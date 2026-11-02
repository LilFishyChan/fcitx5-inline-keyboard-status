#include "status-payload.h"

#include "candidate-payload.h"

namespace inline_keyboard {

std::string buildStatusPayload(const std::vector<StatusItem> &items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) {
            out += ",";
        }
        const StatusItem &item = items[i];
        out += "{\"n\":\"" + escapeJsonString(item.name) + "\"";
        out += ",\"t\":\"" + escapeJsonString(item.text) + "\"";
        if (!item.icon.empty()) {
            out += ",\"i\":\"" + escapeJsonString(item.icon) + "\"";
        }
        if (item.checked) {
            out += ",\"c\":1";
        }
        if (item.separator) {
            out += ",\"s\":1";
        }
        if (!item.children.empty()) {
            out += ",\"k\":" + buildStatusPayload(item.children);
        }
        out += "}";
    }
    out += "]";
    return out;
}

} // namespace inline_keyboard
