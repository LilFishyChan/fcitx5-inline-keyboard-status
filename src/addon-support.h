// addon 的运行时基础设施：路径、原子写文件、图标查找。
//
// 独立于 fcitx5，纯函数，可以直接单测。
// 之所以单独放：这些是「怎么和文件系统打交道」的细节，
// 和「怎么和 fcitx5 打交道」混在一个文件里会让主文件迅速膨胀。

#ifndef INLINE_KEYBOARD_ADDON_SUPPORT_H
#define INLINE_KEYBOARD_ADDON_SUPPORT_H

#include <string>

namespace inline_keyboard {

// ::access 判存在。
// 不用 std::filesystem：它的异常会打崩 fcitx5 主进程。
bool fileExists(const std::string &path);

// $XDG_RUNTIME_DIR，取不到时回退 /tmp。
std::string runtimeDir();

// 先写 <path>.tmp 再 rename，避免读端拿到半截 JSON。
void atomicWrite(const std::string &path, const std::string &data);

// 图标主题名 -> 绝对路径（QML 的 Image 只认文件路径）。
// 传入的已经是绝对路径则原样返回；找不到返回空串。
std::string resolveIcon(const std::string &name);

} // namespace inline_keyboard

#endif // INLINE_KEYBOARD_ADDON_SUPPORT_H
