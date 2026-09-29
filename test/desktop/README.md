# Typing into real applications

`desktop_test.py` types VNI into Chrome, Chrome's address bar, a GTK entry and
a terminal application, through a nested KWin with fcitx5 as its input method,
and checks the text they get. Keys come fast and overlap, like a person
rolling them: 40 down to 5 ms apart.

It is what found the bugs the unit tests model:

- Chrome reports its text late, typing fast, and deletes around the text it
  reported last: "nguòi7" for "người", "baiài" for "bài".
- Chrome's address bar drops deletions around its inline suggestion.
- Alacritty sends a commit longer than a character as a bracketed paste, which
  Claude Code drops and opencode reorders when more keys follow.

Nothing reaches the desktop: KWin renders to a virtual output and runs on a
D-Bus of its own, fcitx5 gets a configuration of its own, Chrome a profile of
its own and no host but localhost.

## Running

Needs `kwin_wayland`, `fcitx5`, `wayland-scanner`, a C compiler, `uv`, and the
applications tested: `google-chrome-stable`, `zenity`, `alacritty` and `tmux`.

```sh
# All tests, with the libbamboo.so of a build:
uv run --with websocket-client test/desktop/desktop_test.py --addon-dir build/src
# Some of them, with the installed addon, keeping the logs:
uv run --with websocket-client test/desktop/desktop_test.py --keep chrome omnibox
```

`fakekeys.c` sends the keys through KWin's `org_kde_kwin_fake_input`, which
the nested KWin allows with `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`.
`fake-input.xml` comes from plasma-wayland-protocols.
