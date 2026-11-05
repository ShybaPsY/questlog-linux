#!/usr/bin/python3
"""Tray icon for Questlog under questlog-linux (KDE Plasma and other StatusNotifierItem trays).

Under Wine, Questlog's own tray menu opens only the first time: Electron has to take the
foreground before it shows the menu, and wineserver allows that only once per window when the
click came through the desktop's tray. This icon is drawn by the Linux desktop instead, so its
menu always works. Questlog's own icon is hidden while this one runs and shown again if this
one goes away while Questlog keeps running.

    questlog-tray.py <questlog-linux launcher> <game prefix (…/compatdata/<appid>/pfx)>

The menu calls the launcher: "show" opens Questlog's window, "stop" ends Questlog. The icon goes
away by itself once Questlog has ended. Only one copy runs at a time.
"""
import fcntl
import json
import os
import signal
import struct
import subprocess
import sys

from PySide6.QtCore import QLocale, QTimer
from PySide6.QtGui import QAction, QIcon, QPixmap
from PySide6.QtWidgets import QApplication, QMenu, QSystemTrayIcon

POLL_MS = 3000
GONE_POLLS = 3  # Questlog counts as ended after this many polls without its process

TEXT = {
    "pt": {"open": "Abrir o Questlog", "quit": "Fechar o Questlog", "tip": "Questlog (AION 2)"},
    "en": {"open": "Open Questlog", "quit": "Quit Questlog", "tip": "Questlog (AION 2)"},
}


def questlog_running(prefix):
    """True if a Questlog.exe process of this prefix runs (Chromium renames its processes, so the
    Windows path in argv[0] is what identifies them)."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                argv0 = f.read().split(b"\0", 1)[0].decode("utf-8", "replace")
            if argv0.replace("\\", "/").rsplit("/", 1)[-1].lower() != "questlog.exe":
                continue
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = f.read().split(b"\0")
        except OSError:
            continue
        want = ("WINEPREFIX=" + prefix.rstrip("/")).encode()
        if want in env or want + b"/" in env:
            return True
    return False


def asar_file(asar, name):
    """Reads one file out of an Electron .asar archive (read only, nothing in it is run)."""
    with open(asar, "rb") as f:
        head = f.read(16)
        header_size = struct.unpack_from("<I", head, 4)[0]
        json_size = struct.unpack_from("<I", head, 12)[0]
        node = json.loads(f.read(json_size))
        for part in name.split("/"):
            node = node["files"][part]
        if "offset" not in node or node.get("unpacked"):
            raise KeyError(name)
        f.seek(8 + header_size + int(node["offset"]))
        return f.read(int(node["size"]))


def questlog_icon(prefix):
    asar = os.path.join(prefix, "drive_c/Program Files/Questlog/resources/app.asar")
    try:
        pixmap = QPixmap()
        if pixmap.loadFromData(asar_file(asar, "resources/icon.png")):
            return QIcon(pixmap)
    except (OSError, KeyError, ValueError, struct.error):
        pass
    return QIcon.fromTheme("applications-games")


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    launcher, prefix = sys.argv[1], sys.argv[2]

    state_dir = os.path.join(os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state"), "questlog-linux")
    os.makedirs(state_dir, exist_ok=True)
    lock = open(os.path.join(state_dir, "tray.lock"), "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return 0  # already running

    app = QApplication(sys.argv[:1])
    app.setApplicationName("questlog-linux")
    app.setDesktopFileName("questlog-linux")
    app.setQuitOnLastWindowClosed(False)
    if not QSystemTrayIcon.isSystemTrayAvailable():
        print("questlog-tray: no system tray on this desktop; Questlog keeps its own icon", file=sys.stderr)
        return 1

    text = TEXT["pt" if QLocale.system().name().lower().startswith("pt") else "en"]
    state = {"hid_original": False, "quitting": False, "gone": 0, "stopper": None}

    tray = QSystemTrayIcon(questlog_icon(prefix))
    tray.setToolTip(text["tip"])
    menu = QMenu()
    open_action = QAction(text["open"], menu)
    quit_action = QAction(text["quit"], menu)
    menu.addAction(open_action)
    menu.addSeparator()
    menu.addAction(quit_action)
    tray.setContextMenu(menu)

    def run(*args):
        return subprocess.Popen([launcher, *args], stdin=subprocess.DEVNULL)

    def open_questlog():
        if not state["quitting"]:
            run("show")

    def quit_questlog():
        if state["quitting"]:
            return
        state["quitting"] = True
        tray.hide()  # Qt then leaves its event loop; main() waits for "stop" below
        state["stopper"] = run("stop")

    open_action.triggered.connect(open_questlog)
    quit_action.triggered.connect(quit_questlog)
    tray.activated.connect(lambda reason: open_questlog() if reason == QSystemTrayIcon.Trigger else None)
    tray.show()

    # Hide Questlog's own icon once ours is up; done in the background (it waits for Questlog).
    hider = run("_ctl", "hide-tray", "90")

    def poll():
        if hider.poll() == 0:
            state["hid_original"] = True
        if questlog_running(prefix):
            state["gone"] = 0
        else:
            state["gone"] += 1
            if state["gone"] >= GONE_POLLS:
                app.quit()

    timer = QTimer()
    timer.timeout.connect(poll)
    timer.start(POLL_MS)
    # Lets Python handle SIGTERM/SIGINT while Qt's loop runs.
    signal.signal(signal.SIGTERM, lambda *_: app.quit())
    signal.signal(signal.SIGINT, lambda *_: app.quit())
    wake = QTimer()
    wake.timeout.connect(lambda: None)
    wake.start(500)

    app.exec()
    tray.hide()
    # "stop" waits for Questlog to save before ending it. Under systemd-run, leaving now would
    # take it down with this unit, so wait for it.
    if state["stopper"] is not None:
        try:
            state["stopper"].wait(timeout=120)
        except subprocess.TimeoutExpired:
            pass
    if hider.poll() is None:
        hider.terminate()
    elif hider.returncode == 0:
        state["hid_original"] = True
    # Leaving while Questlog still runs: give it its own icon back, so it can always be reached.
    if state["hid_original"] and not state["quitting"] and questlog_running(prefix):
        try:
            subprocess.run([launcher, "_ctl", "show-tray"], stdin=subprocess.DEVNULL, timeout=60)
        except subprocess.TimeoutExpired:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
