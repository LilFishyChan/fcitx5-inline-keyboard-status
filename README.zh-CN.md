# fcitx5-inline-keyboard-status

**简体中文** · [English](README.md)

Inline Keyboard 触摸键盘的 fcitx5 配套扩展。

它让触摸键盘能拿到 fcitx5 的候选词、拼音串和当前输入法信息，
并把键盘上按下的键送进 fcitx5。

**单独装它不会得到键盘界面**——键盘本体是 [inline-keyboard](https://github.com/LilFishyChan/dms-inline-keyboard)
（DMS 插件），两者需要一起使用。

## 依赖

- fcitx5
- 构建时需要 fcitx5 开发头文件（`fcitx5-devel` 或对应发行版的等价包）
- CMake ≥ 3.16、支持 C++20 的编译器

## 构建与安装

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure

# 用户级安装（无需 root）：库和 conf 一次装齐
# conf 里的 Library= 路径在安装时按 prefix 生成 —— fcitx5 不搜 ~/.local/lib，
# 所以用户级那份必须是绝对路径，交给 cmake 算，别再手抄一份进仓库
cmake --install build --prefix ~/.local
systemctl --user restart 'app-org.fcitx.Fcitx5@autostart.service'
```

系统级安装（打包用）：`sudo cmake --install build --prefix /usr`
（同样装齐两个文件，`Library=/usr/lib/fcitx5/inline-keyboard-status`。打包时配合
`DESTDIR` 不会污染这个路径。）

## 卸载

```bash
rm -f ~/.local/lib/fcitx5/inline-keyboard-status.so
rm -f ~/.local/share/fcitx5/addon/inline-keyboard-status.conf
pkill -x fcitx5; systemctl --user start app-org.fcitx.Fcitx5@autostart.service
```

## 与键盘的通信

`$XDG_RUNTIME_DIR/inline-keyboard.sock`，一行一条消息。

键盘发给本扩展：

| 消息 | 说明 |
|---|---|
| `key:<keysym>[:<mods>]` | 按下的键。mods 位掩码：1=Shift 2=Ctrl 4=Alt 8=Super |
| `text:<utf8>` | 把一段文本直接提交进输入框（表情 / 符号页用）。一个表情由多个码位组成，按键通道送不进去，只有 `InputContext::commitString()` 可以 |
| `ui:0` / `ui:1` | 调试用：临时声明/撤销自己可用 |
| `mode:physical` / `mode:osk` | 调试用：临时切换 `InputMethodMode` |

本扩展发给键盘：

| 消息 | 说明 |
|---|---|
| `state:{json}` | 候选词、拼音串、输入法、是否还有更多候选 |
| `imlist:{json}` | 输入法清单 |
| `allcand:{json}` | 全量候选（键盘请求时发） |
| `error:<kind>:<detail>` | 错误上报 |

## 运行状态文件

调试用，正常使用不需要关心：

| 文件 | 内容 |
|---|---|
| `$XDG_RUNTIME_DIR/inline-keyboard-status.json` | 当前面板状态快照 |
| `$XDG_RUNTIME_DIR/inline-keyboard-addon.log` | 事件日志（定位问题用） |

## 测试

```bash
ctest --test-dir build --output-on-failure
```

## 许可

MIT
