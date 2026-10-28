# fcitx5-inline-keyboard-status

**English** · [简体中文](README.zh-CN.md)

The fcitx5 companion addon for the Inline Keyboard touch keyboard.

It lets the touch keyboard receive fcitx5's candidates, preedit string and current input-method
information, and delivers the keys pressed on the keyboard into fcitx5.

**Installing this alone gives you no keyboard UI** — the keyboard itself is
[inline-keyboard](https://github.com/LilFishyChan/dms-inline-keyboard) (a DMS plugin), and the two are meant to be
used together.

## Dependencies

- fcitx5
- fcitx5 development headers for building (`fcitx5-devel` or the equivalent package on the distro)
- CMake ≥ 3.16, a compiler with C++20 support

## Build and install

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure

# user-level install (no root needed): ships both the library and the conf
# The Library= path in the conf is generated from the prefix at install time —
# fcitx5 does not search ~/.local/lib, so the user-level copy must be absolute;
# let cmake work it out instead of hand-copying a path into the repo
cmake --install build --prefix ~/.local
systemctl --user restart 'app-org.fcitx.Fcitx5@autostart.service'
```

System-wide install (for packaging): `sudo cmake --install build --prefix /usr`
(this also installs both files, with `Library=/usr/lib/fcitx5/inline-keyboard-status`; when packaging,
`DESTDIR` does not pollute that path).

## Uninstall

```bash
rm -f ~/.local/lib/fcitx5/inline-keyboard-status.so
rm -f ~/.local/share/fcitx5/addon/inline-keyboard-status.conf
pkill -x fcitx5; systemctl --user start app-org.fcitx.Fcitx5@autostart.service
```

## Talking to the keyboard

`$XDG_RUNTIME_DIR/inline-keyboard.sock`, one message per line.

The keyboard sends to this addon:

| Message | Meaning |
|---|---|
| `key:<keysym>[:<mods>]` | A key press. `mods` is a bitmask: 1=Shift 2=Ctrl 4=Alt 8=Super |
| `text:<utf8>` | Commit a string straight into the input field (used by the emoji / symbol pages). One emoji spans several code points, so it cannot go through the key channel — only `InputContext::commitString()` can carry it |
| `ui:0` / `ui:1` | Debug only: temporarily declare/withdraw availability |
| `mode:physical` / `mode:osk` | Debug only: temporarily switch `InputMethodMode` |

This addon sends to the keyboard:

| Message | Meaning |
|---|---|
| `state:{json}` | Candidates, preedit string, input method, whether more candidates exist |
| `imlist:{json}` | Input-method list |
| `allcand:{json}` | Full candidate set (sent when the keyboard asks for it) |
| `error:<kind>:<detail>` | Error report |

## Runtime state files

For debugging; nothing to care about in normal use:

| File | Contents |
|---|---|
| `$XDG_RUNTIME_DIR/inline-keyboard-status.json` | Snapshot of the current panel state |
| `$XDG_RUNTIME_DIR/inline-keyboard-addon.log` | Event log (for tracking down problems) |

## Testing

```bash
ctest --test-dir build --output-on-failure
```

## License

MIT
