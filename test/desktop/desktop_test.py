#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 googlesky
#
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Types VNI into real applications through a nested KWin and fcitx5 with
the bamboo addon, as fast as a person rolling keys, and checks their text.

Nothing reaches the desktop: KWin renders to a virtual output and runs on a
D-Bus of its own, fcitx5 gets a configuration of its own, Chrome resolves no
host but localhost. See README.md.
"""

import argparse
import ast
import contextlib
import functools
import http.server
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

# Milliseconds between key presses, key hold, jitter of both.
SPEEDS = [(40, 70, 10), (20, 55, 8), (10, 45, 5), (5, 30, 3)]

CODES = {c: k for c, k in zip("1234567890", range(2, 12))}
CODES.update({c: k for c, k in zip("qwertyuiop", range(16, 26))})
CODES.update({c: k for c, k in zip("asdfghjkl", range(30, 39))})
CODES.update({c: k for c, k in zip("zxcvbnm", range(44, 51))})
CODES[" "] = 57
SHIFT, CTRL, L, RETURN = 42, 29, 38, 28

FCITX_PROFILE = """[Groups/0]
Name=Default
Default Layout=us
DefaultIM=bamboo

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=bamboo
Layout=

[GroupOrder]
0=Default
"""
# Shift taps switch to English by default, the first key typed is one.
FCITX_CONFIG = "[Hotkey]\nAltTriggerKeys=\n\n[Behavior]\nActiveByDefault=True\n"
BAMBOO_CONFIG = """InputMethod=VNI
DefaultInputMode="Surrounding Text"
WaylandBackSpace=True
"""

# Logs the input of a terminal application that set the modes Claude Code
# sets, bracketed paste among them.
RAWLOG = r"""
import os, sys, termios, time, tty
fd = sys.stdin.fileno()
old = termios.tcgetattr(fd)
tty.setraw(fd)
os.write(1, b"\x1b[>5u\x1b[>4;2m\x1b[?2004h\x1b[?1004h")
with open(sys.argv[1], "ab", buffering=0) as log:
    try:
        while True:
            data = os.read(fd, 4096)
            if not data or b"\x03" in data:
                break
            log.write(repr(data).encode() + b"\n")
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
"""


def key_events(text, gap, hold, jitter, rng):
    """fakekeys arguments typing text: presses gap ms apart, held hold ms
    so they overlap, a key up before it goes down again as on a keyboard. A
    lone Shift first: KWin activates the input method on the first key."""
    presses, at = [], 0
    for c in text:
        presses.append((at, CODES[c], hold + rng.randint(-jitter, jitter)))
        at += max(1, gap + rng.randint(-jitter, jitter))
    events = []
    for i, (at, code, held) in enumerate(presses):
        up = at + max(5, held)
        again = [p for p, k, _ in presses[i + 1 :] if k == code]
        if again:
            up = min(up, again[0] - 1)
        events += [(at, 0, "d", code), (max(at + 1, up), 1, "u", code)]
    events.sort()
    now, out = 0, [f"d{SHIFT}", "w30", f"u{SHIFT}", "w200"]
    for at, _, kind, code in events:
        if at > now:
            out.append(f"w{at - now}")
            now = at
        out.append(f"{kind}{code}")
    return out


class Session:
    """A private D-Bus, KWin on a virtual output, fcitx5 its input method."""

    def __init__(self, work, addon_dir):
        self.work = work
        self.addon_dir = addon_dir
        self.processes = []
        self.bus_pid = None

    def __enter__(self):
        os.makedirs(self.work)
        config = os.path.join(self.work, "config", "fcitx5")
        os.makedirs(os.path.join(config, "conf"), exist_ok=True)
        for name, text in [
            ("profile", FCITX_PROFILE),
            ("config", FCITX_CONFIG),
            ("conf/bamboo.conf", BAMBOO_CONFIG),
        ]:
            with open(os.path.join(config, name), "w") as f:
                f.write(text)
        self.fakekeys = self.build_fakekeys()
        self.env = dict(os.environ)
        for name in ["DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "XAUTHORITY"]:
            self.env.pop(name, None)
        for name in ["CONFIG", "DATA", "CACHE", "STATE"]:
            self.env[f"XDG_{name}_HOME"] = os.path.join(self.work, name.lower())
        self.env.update(
            KWIN_WAYLAND_NO_PERMISSION_CHECKS="1",
            GTK_IM_MODULE="fcitx",
            QT_IM_MODULE="fcitx",
            XMODIFIERS="@im=fcitx",
        )
        if self.addon_dir:
            self.env["FCITX_ADDON_DIRS"] = f"{os.path.abspath(self.addon_dir)}:/usr/lib/fcitx5"
        bus = subprocess.run(
            ["dbus-daemon", "--session", "--fork", "--print-address=1", "--print-pid=1"],
            capture_output=True, text=True, check=True, env=self.env,
        ).stdout.split("\n")
        self.env["DBUS_SESSION_BUS_ADDRESS"], self.bus_pid = bus[0], int(bus[1])
        wrapper = os.path.join(self.work, "fcitx5.sh")
        with open(wrapper, "w") as f:
            log = os.path.join(self.work, "fcitx5.log")
            f.write(f"#!/bin/sh\nexec fcitx5 --verbose='*=4,bamboo=5' > '{log}' 2>&1\n")
        os.chmod(wrapper, 0o755)
        socket = f"wl-{os.path.basename(os.path.dirname(self.work))}-{os.path.basename(self.work)}"
        kwin = self.spawn(["kwin_wayland", "--virtual", "--no-lockscreen", "--socket", socket,
                           "--width", "1280", "--height", "900", "--inputmethod", wrapper], "kwin")
        socket_path = os.path.join(os.environ["XDG_RUNTIME_DIR"], socket)
        for _ in range(80):
            if os.path.exists(socket_path) or kwin.poll() is not None:
                break
            time.sleep(0.25)
        if kwin.poll() is not None or not os.path.exists(socket_path):
            raise RuntimeError("KWin did not start, see kwin.log")
        self.env["WAYLAND_DISPLAY"] = socket
        time.sleep(3)
        return self

    def __exit__(self, *exc):
        for process in reversed(self.processes):
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGTERM)
        for process in self.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        if self.bus_pid:
            with contextlib.suppress(ProcessLookupError):
                os.kill(self.bus_pid, signal.SIGTERM)
        # The bus starts services, portals, ksecretd, kdeconnectd, which
        # outlive it: whatever runs with this session's environment goes.
        marker = f"XDG_DATA_HOME={self.env['XDG_DATA_HOME']}\0".encode()
        for sig in [signal.SIGTERM, signal.SIGKILL]:
            left = False
            for entry in os.listdir("/proc"):
                if not entry.isdigit() or int(entry) == os.getpid():
                    continue
                with contextlib.suppress(OSError):
                    with open(f"/proc/{entry}/environ", "rb") as f:
                        if marker in f.read():
                            os.kill(int(entry), sig)
                            left = True
            if not left:
                break
            time.sleep(2)

    def build_fakekeys(self):
        out = os.path.join(self.work, "fakekeys")
        xml = os.path.join(HERE, "fake-input.xml")
        header = os.path.join(self.work, "fake-input-client-protocol.h")
        code = os.path.join(self.work, "fake-input-protocol.c")
        subprocess.run(["wayland-scanner", "client-header", xml, header], check=True)
        subprocess.run(["wayland-scanner", "private-code", xml, code], check=True)
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "wayland-client"],
                               capture_output=True, text=True, check=True).stdout.split()
        subprocess.run(["cc", "-O1", "-I", self.work, "-o", out,
                        os.path.join(HERE, "fakekeys.c"), code, *flags], check=True)
        return out

    def spawn(self, argv, name, stdin=subprocess.DEVNULL):
        log = open(os.path.join(self.work, f"{name}.log"), "w")
        process = subprocess.Popen(argv, env=self.env, stdout=log, stderr=subprocess.STDOUT,
                                   stdin=stdin, start_new_session=True)
        self.processes.append(process)
        return process

    def keys(self, args):
        subprocess.run([self.fakekeys, *args], env=self.env, check=True)


class Chrome:
    """Chrome in the session, driven through the DevTools protocol."""

    PORT = 9333

    def __init__(self, session):
        import websocket  # uv run --with websocket-client

        profile = os.path.join(session.work, "chrome")
        session.spawn(["google-chrome-stable", f"--user-data-dir={profile}", "--no-first-run",
                       "--no-default-browser-check", "--disable-sync", "--password-store=basic",
                       "--ozone-platform=wayland", "--enable-wayland-ime",
                       f"--remote-debugging-port={self.PORT}", "--disable-background-networking",
                       "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE localhost, "
                       "EXCLUDE *.localhost", "about:blank"], "chrome")
        for _ in range(60):
            with contextlib.suppress(OSError, StopIteration):
                page = self.page()
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("Chrome did not start, see chrome.log")
        self.ws = websocket.create_connection(page["webSocketDebuggerUrl"], suppress_origin=True)
        self.id = 0

    def page(self):
        targets = json.load(urllib.request.urlopen(f"http://127.0.0.1:{self.PORT}/json"))
        return next(t for t in targets if t["type"] == "page")

    def call(self, method, **params):
        self.id += 1
        self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        while True:
            reply = json.loads(self.ws.recv())
            if reply.get("id") == self.id:
                return reply.get("result", {})

    def js(self, expression):
        return self.call("Runtime.evaluate", expression=expression)["result"].get("value")


def run_cases(name, cases, runs, attempt):
    """attempt(keys, speed, rng) returns the text the application got."""
    rng = random.Random(1)
    failures = 0
    for keys, want in cases:
        for speed in SPEEDS:
            bad = []
            for _ in range(runs):
                got = attempt(keys, speed, rng)
                if got != want:
                    bad.append(got)
            failures += len(bad)
            status = "ok" if not bad else f"FAIL {bad[:3]!r}"
            print(f"{name} {keys!r} gap {speed[0]} ms: {runs - len(bad)}/{runs} {status}",
                  flush=True)
    return failures


def test_chrome(session, runs):
    chrome = Chrome(session)
    chrome.call("Page.navigate", url="data:text/html,<textarea id=t autofocus></textarea>")
    time.sleep(2)

    def attempt(keys, speed, rng):
        chrome.js("t.value = ''; t.focus(); 1")
        time.sleep(0.3)
        session.keys(key_events(keys + " ", *speed, rng) + ["w500"])
        return chrome.js("t.value").strip()

    return run_cases("chrome", [
        ("toi6 d9ang hoc5 bai2 hat1 nguoi72 viet65 nam truong72 d9uoc75",
         "tôi đang học bài hát người việt nam trường được"),
        ("nguoi27 d9i truong72 viet65 khong6", "người đi trường việt không"),
    ], runs, attempt)


def test_omnibox(session, runs):
    # Hosts visited, typed: their names complete inline as their start is
    # typed, a suggestion selected after the cursor.
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(http.server.SimpleHTTPRequestHandler,
                                            directory=session.work))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    chrome = Chrome(session)
    for host in ["baihat.localhost", "vietnamnet.localhost", "facebook.localhost"]:
        for _ in range(3):
            chrome.call("Page.navigate", url=f"http://{host}:{port}/", transitionType="typed")
            time.sleep(1)

    def attempt(keys, speed, rng):
        chrome.call("Page.navigate", url="about:blank")
        time.sleep(0.8)
        focus = [f"d{CTRL}", "w20", f"d{L}", "w20", f"u{L}", "w10", f"u{CTRL}", "w300"]
        session.keys(focus + key_events(keys, *speed, rng)[4:]
                     + ["w300", f"d{RETURN}", "w30", f"u{RETURN}", "w100"])
        time.sleep(1.2)
        url = urllib.parse.urlparse(chrome.page()["url"])
        return urllib.parse.parse_qs(url.query).get("q", [url.hostname])[0]

    failures = run_cases("omnibox", [
        ("bai2 hat1 viet65 nam", "bài hát việt nam"),
        # Return takes the suggestion.
        ("face", "facebook.localhost"),
    ], runs, attempt)
    server.shutdown()
    return failures


def test_gtk(session, runs):
    def attempt(keys, speed, rng):
        zenity = subprocess.Popen(["zenity", "--entry", "--text", "test"], env=session.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        time.sleep(1.5)
        session.keys(key_events(keys, *speed, rng) + ["w300", f"d{RETURN}", "w30", f"u{RETURN}"])
        try:
            return zenity.communicate(timeout=5)[0].rstrip("\n")
        except subprocess.TimeoutExpired:
            zenity.kill()
            return None

    return run_cases("gtk", [
        ("nguoi27 d9i truong72 viet65 khong6", "người đi trường việt không"),
    ], runs, attempt)


def test_terminal(session, runs):
    """Alacritty, tmux and an application setting Claude Code's terminal
    modes: the text arrives typed, never as a bracketed paste."""
    rawlog = os.path.join(session.work, "rawlog.py")
    with open(rawlog, "w") as f:
        f.write(RAWLOG)
    log = os.path.join(session.work, "terminal-input.log")
    tmux = ["tmux", "-L", "bamboo-desktop-test", "-f", "/dev/null"]
    session.spawn(["alacritty", "-e", *tmux, "new-session",
                   f"{sys.executable} {rawlog} {log}"], "alacritty")
    time.sleep(4)

    def attempt(keys, speed, rng):
        open(log, "w").close()
        session.keys(key_events(" " + keys + " ", *speed, rng) + ["w500"])
        data = b"".join(ast.literal_eval(line) for line in open(log))
        if b"\x1b[200~" in data:
            return "bracketed paste"
        text = ""
        for ch in data.decode("utf-8", "replace"):
            text = text[:-1] if ch == "\x7f" else text + ch
        return text.strip()

    failures = run_cases("terminal", [
        ("toi6 biet61 ro4 nguoi72 viet65 nam", "tôi biết rõ người việt nam"),
    ], runs, attempt)
    subprocess.run([*tmux, "kill-server"], stderr=subprocess.DEVNULL)
    return failures


TESTS = {"chrome": test_chrome, "omnibox": test_omnibox, "gtk": test_gtk,
         "terminal": test_terminal}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("tests", nargs="*", help=f"some of {', '.join(TESTS)}; all by default")
    parser.add_argument("--addon-dir", help="directory with the libbamboo.so to test, "
                        "a build's src; the installed one by default")
    parser.add_argument("--runs", type=int, default=3, help="runs per case and speed")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    args = parser.parse_args()
    if unknown := set(args.tests) - set(TESTS):
        parser.error(f"unknown tests: {', '.join(unknown)}")
    work = tempfile.mkdtemp(prefix="bamboo-desktop-test-")
    failures = 0
    try:
        for name in args.tests or TESTS:
            with Session(os.path.join(work, name), args.addon_dir) as session:
                failures += TESTS[name](session, args.runs)
    finally:
        if args.keep:
            print(f"logs in {work}")
        else:
            # fcitx5 may still write its configuration when it quits.
            for _ in range(5):
                shutil.rmtree(work, ignore_errors=True)
                if not os.path.exists(work):
                    break
                time.sleep(1)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
