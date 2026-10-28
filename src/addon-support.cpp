#include "addon-support.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace inline_keyboard {

bool fileExists(const std::string &path) {
    return ::access(path.c_str(), F_OK) == 0;
}

std::string runtimeDir() {
    const char *env = std::getenv("XDG_RUNTIME_DIR");
    return env ? std::string(env) : std::string("/tmp");
}

void atomicWrite(const std::string &path, const std::string &data) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return;
        }
        out << data;
    }
    std::rename(tmp.c_str(), path.c_str());
}

std::string resolveIcon(const std::string &name) {
    if (name.empty()) {
        return {};
    }
    if (name.front() == '/') {
        return name;
    }
    static const char *dirs[] = {
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/32x32/apps",
        "/usr/share/icons/hicolor/scalable/apps",
        "/usr/share/icons/AdwaitaLegacy/48x48/legacy",
        "/usr/share/icons/AdwaitaLegacy/32x32/legacy",
        "/usr/share/icons/breeze/apps/48",
        "/usr/share/icons/breeze-dark/apps/48",
    };
    for (const char *dir : dirs) {
        for (const char *ext : {".png", ".svg"}) {
            const std::string path = std::string(dir) + "/" + name + ext;
            if (fileExists(path)) {
                return path;
            }
        }
    }
    return {};
}

} // namespace inline_keyboard
