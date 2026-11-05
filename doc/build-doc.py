"""Builds Questlog-on-Linux.pdf.  Usage: python3 build-doc.py <output.pdf>  (needs reportlab and the Liberation fonts)"""
import sys
from xml.sax.saxutils import escape

from reportlab.graphics.shapes import Drawing, Line, Polygon, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (Image, KeepTogether, PageBreak, Paragraph, Preformatted, SimpleDocTemplate, Spacer,
                                Table, TableStyle)
import os
HERE = os.path.dirname(os.path.abspath(__file__))

OUT = sys.argv[1]
VERSION = "1.1.0"

F = "/usr/share/fonts/liberation-sans-fonts/LiberationSans-"
M = "/usr/share/fonts/liberation-mono-fonts/LiberationMono-"
pdfmetrics.registerFont(TTFont("Sans", F + "Regular.ttf"))
pdfmetrics.registerFont(TTFont("Sans-Bold", F + "Bold.ttf"))
pdfmetrics.registerFont(TTFont("Sans-Italic", F + "Italic.ttf"))
pdfmetrics.registerFont(TTFont("Sans-BoldItalic", F + "BoldItalic.ttf"))
pdfmetrics.registerFont(TTFont("Mono", M + "Regular.ttf"))
pdfmetrics.registerFont(TTFont("Mono-Bold", M + "Bold.ttf"))
pdfmetrics.registerFontFamily("Sans", normal="Sans", bold="Sans-Bold", italic="Sans-Italic", boldItalic="Sans-BoldItalic")
pdfmetrics.registerFontFamily("Mono", normal="Mono", bold="Mono-Bold", italic="Mono", boldItalic="Mono-Bold")

INK = colors.HexColor("#1d2330")
MUTED = colors.HexColor("#5b6475")
ACCENT = colors.HexColor("#2f5d8a")
RULE = colors.HexColor("#d4d9e2")
CODEBG = colors.HexColor("#f2f4f7")
HEADBG = colors.HexColor("#e8edf3")

body = ParagraphStyle("body", fontName="Sans", fontSize=9.6, leading=13.6, textColor=INK, spaceAfter=5, alignment=TA_LEFT)
small = ParagraphStyle("small", parent=body, fontSize=8.4, leading=11.4, textColor=MUTED)
cell = ParagraphStyle("cell", parent=body, fontSize=8.6, leading=11.6, spaceAfter=0)
cellb = ParagraphStyle("cellb", parent=cell, fontName="Sans-Bold")
h1 = ParagraphStyle("h1", fontName="Sans-Bold", fontSize=14.5, leading=18, textColor=ACCENT, spaceBefore=12, spaceAfter=6, keepWithNext=1)
h2 = ParagraphStyle("h2", fontName="Sans-Bold", fontSize=11, leading=14.5, textColor=INK, spaceBefore=9, spaceAfter=3, keepWithNext=1)
label = ParagraphStyle("label", parent=body, fontName="Sans-Bold", spaceAfter=1, keepWithNext=1)
bullet = ParagraphStyle("bullet", parent=body, leftIndent=12, bulletIndent=2, spaceAfter=2.5)
code = ParagraphStyle("code", fontName="Mono", fontSize=8.2, leading=10.8, textColor=INK, backColor=CODEBG,
                      borderPadding=(5, 6, 5, 6), leftIndent=6, rightIndent=6, spaceBefore=3, spaceAfter=9)
title = ParagraphStyle("title", fontName="Sans-Bold", fontSize=23, leading=27, textColor=INK, spaceAfter=4)
subtitle = ParagraphStyle("subtitle", fontName="Sans", fontSize=12, leading=16, textColor=MUTED, spaceAfter=14)


def c(text):
    """inline code"""
    return f'<font face="Mono" size="8.6">{escape(text)}</font>'


def P(text, style=body):
    return Paragraph(text, style)


def B(items):
    return [Paragraph(i, bullet, bulletText="•") for i in items]


def code_block(text):
    return Preformatted(text.strip("\n"), code)


def table(rows, widths, header=True):
    data = [[x if not isinstance(x, str) else Paragraph(x, cellb if (header and r == 0) else cell) for x in row]
            for r, row in enumerate(rows)]
    t = Table(data, colWidths=widths, repeatRows=1 if header else 0)
    st = [("VALIGN", (0, 0), (-1, -1), "TOP"),
          ("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE),
          ("TOPPADDING", (0, 0), (-1, -1), 3.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 3.5),
          ("LEFTPADDING", (0, 0), (-1, -1), 4), ("RIGHTPADDING", (0, 0), (-1, -1), 4)]
    if header:
        st += [("BACKGROUND", (0, 0), (-1, 0), HEADBG)]
    t.setStyle(TableStyle(st))
    return t


def problem(name, symptom, cause, fix, extra=None):
    """One 'what went wrong under Wine' entry."""
    rows = [[P("Symptom", cellb), P(symptom, cell)], [P("Cause", cellb), P(cause, cell)], [P("Fix", cellb), P(fix, cell)]]
    if extra:
        rows.append([P(extra[0], cellb), P(extra[1], cell)])
    t = Table(rows, colWidths=[22 * mm, 148 * mm])
    t.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "TOP"), ("LINEBELOW", (0, 0), (-1, -2), 0.3, RULE),
                           ("TOPPADDING", (0, 0), (-1, -1), 2.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 2.5),
                           ("LEFTPADDING", (0, 0), (-1, -1), 3), ("BOX", (0, 0), (-1, -1), 0.5, RULE)]))
    return KeepTogether([P(name, h2), t, Spacer(1, 4)])


def diagram():
    w, h = 170 * mm, 78 * mm
    d = Drawing(w, h)

    def box(x, y, bw, bh, head, lines, fill):
        d.add(Rect(x, y, bw, bh, fillColor=fill, strokeColor=ACCENT, strokeWidth=0.8, rx=3, ry=3))
        d.add(String(x + 6, y + bh - 13, head, fontName="Sans-Bold", fontSize=8.6, fillColor=INK))
        for i, line in enumerate(lines):
            d.add(String(x + 6, y + bh - 25 - i * 10.5, line, fontName="Sans", fontSize=7.6, fillColor=INK))

    def arrow(x1, y1, x2, y2, text=None, tx=None, ty=None):
        d.add(Line(x1, y1, x2, y2, strokeColor=MUTED, strokeWidth=0.9))
        import math
        a = math.atan2(y2 - y1, x2 - x1)
        s = 5
        d.add(Polygon([x2, y2, x2 - s * math.cos(a - 0.4), y2 - s * math.sin(a - 0.4),
                       x2 - s * math.cos(a + 0.4), y2 - s * math.sin(a + 0.4)], fillColor=MUTED, strokeColor=MUTED))
        if text:
            d.add(String(tx, ty, text, fontName="Sans-Italic", fontSize=7, fillColor=MUTED))

    # regions, with a 20 mm lane between them for the arrows
    d.add(Rect(0, 0, 70 * mm, h, fillColor=colors.HexColor("#f7f9fb"), strokeColor=RULE, strokeWidth=0.6))
    d.add(Rect(90 * mm, 0, 80 * mm, h, fillColor=colors.HexColor("#f7f9fb"), strokeColor=RULE, strokeWidth=0.6))
    d.add(String(4, h - 11, "Linux (the user's session)", fontName="Sans-Bold", fontSize=8, fillColor=MUTED))
    d.add(String(90 * mm + 4, h - 11, "AION 2's Proton session (one wineserver)", fontName="Sans-Bold", fontSize=8, fillColor=MUTED))

    box(3 * mm, 49 * mm, 64 * mm, 21 * mm, "questlog-linux",
        ["Steam launch option; starts the game as", "usual, its watcher runs Questlog"], colors.white)
    box(3 * mm, 23 * mm, 64 * mm, 22 * mm, "questlog-capture",
        ["systemd, CAP_NET_RAW, 127.0.0.1:29741", "libpcap; only the game's TCP ports"], colors.white)
    box(3 * mm, 3 * mm, 64 * mm, 16 * mm, "questlog-tray (PySide6)", ["tray icon: Open / Quit"], colors.white)

    box(93 * mm, 55 * mm, 74 * mm, 15 * mm, "AION2.exe", ["the game, started by Steam as usual"], colors.white)
    box(93 * mm, 18 * mm, 74 * mm, 33 * mm, "Questlog.exe (official, unmodified)",
        ["its Node service process loads", "  C:\\windows\\system32\\Npcap\\wpcap.dll", "  = the bridge, with the window-order guard",
         "finds the game by Toolhelp, GetExtendedTcpTable", "and its window, as on Windows"], colors.white)
    box(93 * mm, 3 * mm, 74 * mm, 12 * mm, "questlog-ctl.exe (on demand)", [], colors.white)

    def lane(y, text):
        d.add(String(80 * mm, y, text, fontName="Sans-Italic", fontSize=6.8, fillColor=MUTED, textAnchor="middle"))

    arrow(67 * mm, 58 * mm, 93 * mm, 44 * mm)
    lane(60 * mm, "steam-runtime-"); lane(57 * mm, "launch-client")
    arrow(93 * mm, 30 * mm, 67 * mm, 32 * mm)
    lane(26.5 * mm, "TCP (bridge)")
    arrow(67 * mm, 9 * mm, 93 * mm, 9 * mm)
    lane(11 * mm, "via the launcher")
    return d


story = []
story += [P("Questlog on Linux", title),
          P(f"questlog-linux {VERSION} — running the official Questlog desktop app for AION 2 on Linux, "
            "next to the game, under Steam's Proton", subtitle)]
story += [table([["Prepared by", "Gabriel Lopes (GitHub: ShybaPsY)"],
                 ["Source", c("https://github.com/ShybaPsY/questlog-linux") + " (MIT license)"],
                 ["Date", "November 2026"],
                 ["Tested on", "Fedora Linux 44, KDE Plasma 6.7.5 (Wayland; the game and Questlog run through XWayland), "
                               "Steam with Proton 11 (Proton Experimental) and Steam Linux Runtime 4, "
                               "Questlog 0.1.48 (Electron 44.5.1), AION 2 (Steam app 3393110)"]],
                [30 * mm, 140 * mm], header=False), Spacer(1, 8)]

story += [P("1. Summary", h1)]
story += [P("Questlog Desktop is a Windows-only Electron app. This package makes the <b>official, unmodified</b> Windows "
            "build run on Linux inside AION 2's own Proton session, with the live combat meter, overlays and hotkeys. "
            "Questlog is installed with its official installer from " + c("cdn.questlog.gg") + " and none of its files "
            "are changed (one file is added to its folder: the font fix, 4.12). What this package adds around it:")]
story += B(["a replacement for Npcap's " + c("wpcap.dll") + " that hands Questlog packets captured on the Linux side;",
            "a small Linux capture service that captures only the game's own TCP connections;",
            "a launcher, set as the game's Steam launch option, that starts Questlog inside the game's Wine session, "
            "keeps it installed and up to date, and closes it after the game;",
            "fixes for the window, focus and tray problems that come with sharing a Wine session with a fullscreen game;",
            "a DirectWrite stand-in for Questlog only, so its variable fonts render at their real weights."])
story += [P("<b>Status.</b> In daily play on the tested system: live capture and DPS meter, character detection, recordings, "
            "the overlays and their Alt+E controls, Alt+Tab between the game and Questlog, Questlog's own title bar, "
            "and the interface fonts at their real weights (since 1.1.0). "
            "Section 7 lists what is not tested yet. The Questlog team knows about this work.")]

story += [P("2. Install and use", h1)]
story += [P("Requirements", h2)]
story += B(["Steam installed as a normal package (the Flatpak build is not supported), AION 2 started at least once with Proton.",
            "systemd and sudo; " + c("curl") + ", " + c("openssl") + ", " + c("python3") + ", and the libpcap runtime library.",
            "The ready-made capture service in " + c("bin/") + " needs glibc 2.34 or newer (Fedora 35+, Ubuntu 22.04+, Debian 12+, "
            "Arch, SteamOS 3). On older systems the installer compiles it instead.",
            "Optional: PySide6 for the system Python (tray icon), 7-Zip (update check), xdotool (window to front), notify-send.",
            "KDE Plasma 6.7 or newer for the overlay-controls fix (4.6). Other desktops are untested."])
story += [P("Steps", h2)]
story += [P("1. Unpack the zip and run the installer as your normal user (it asks for sudo once, for the capture service):")]
story += [code_block("./install.sh            # ready-made programs from bin/\n./install.sh --build    # or: compile everything from the sources first")]
story += [P("2. In Steam: AION 2 › Properties › General › Launch Options, paste the line the installer prints. It is the "
            "full path of " + c("~/.local/bin/questlog-linux") + " followed by " + c("%command%") + ", for example:")]
story += [code_block("/home/<user>/.local/bin/questlog-linux %command%")]
story += [P("3. Play. On the first launch Questlog is downloaded (checksum verified) and installed in the game's prefix. "
            "From then on it starts hidden in the tray with every game launch and closes when the game ends. "
            "The menu entry <i>Questlog (AION 2)</i> opens its window while the game runs.")]
story += [P("Commands and settings", h2)]
story += [table([["Command", "What it does"],
                 [c("questlog-linux show"), "open Questlog's window (starts it if needed; the game must be running)"],
                 [c("questlog-linux stop [--now]"), "close Questlog; while the game runs it first hides the window and waits 12 s (4.9)"],
                 [c("questlog-linux status"), "installed and latest Questlog version, bridge, service, game, tray"],
                 [c("questlog-linux log"), "the launcher's log and the bridge's log"],
                 [c("./install.sh --uninstall"), "remove everything the installer added (then clear the launch option)"]],
                [52 * mm, 118 * mm]), Spacer(1, 6)]
story += [P("Settings are environment variables, written before the command in the launch option, e.g. "
            + c("QL_TRAY=0 /home/<user>/.local/bin/questlog-linux %command%") + ":")]
story += [table([["Variable", "Effect"],
                 [c("QL_AUTO_UPDATE=0"), "do not update Questlog at game start"],
                 [c("QL_TRAY=0"), "keep Questlog's own tray icon instead of ours"],
                 [c("QL_WINDOW_GUARD=0"), "turn off the window-order guard in the bridge (4.7)"],
                 [c("QL_FONT_FIX=0"), "take the font fix out again (4.12)"]],
                [52 * mm, 118 * mm]), Spacer(1, 6)]
story += [P("What gets installed", h2)]
story += [table([["Where", "What"],
                 [c("/usr/local/libexec/questlog-capture"), "the capture service"],
                 [c("/etc/systemd/system/questlog-capture.socket") + "<br/>" + c("questlog-capture@.service"),
                  "socket-activated service, runs as the user who installed it"],
                 [c("~/.local/bin/questlog-linux"), "the launcher"],
                 [c("~/.local/share/questlog-linux/"), c("wpcap.dll") + ", " + c("questlog-ctl.exe") + ", " + c("dwrite.dll") + ", "
                  + c("questlog-tray.py") + ", Questlog's icon (taken from the user's installation)"],
                 [c("~/.local/share/applications/questlog-linux.desktop"), "menu entry “Questlog (AION 2)”"],
                 [c("~/.config/kwinrulesrc"), "one window rule, " + c("questlog-hud") + " (KDE only; other rules untouched)"],
                 ["game prefix (" + c("compatdata/3393110/pfx") + ")",
                  c("C:\\Program Files\\Questlog") + " (official installer), " + c("C:\\windows\\system32\\Npcap\\wpcap.dll")
                  + " (the bridge), " + c("C:\\Program Files\\Questlog\\dwrite.dll") + " (the font fix), under "
                  + c("HKCU\\Software\\Wine\\AppDefaults\\Questlog.exe") + ": two " + c("X11 Driver") + " values and "
                  + c("DllOverrides: dwrite=native,builtin")],
                 [c("~/.local/state/questlog-linux/"), "logs: launcher, Questlog's output, bridge"],
                 [c("~/.cache/questlog-linux/"), "the installers of the current and previous Questlog version"]],
                [72 * mm, 98 * mm])]

story += [PageBreak(), P("3. How it works", h1)]
story += [diagram(), Spacer(1, 8)]
story += [table([["Part", "Runs", "Role"],
                 [c("questlog-linux") + " (bash)", "Linux, as the Steam launch option",
                  "starts the game unchanged; runs a watcher that installs, updates, starts and closes Questlog in the game's session"],
                 [c("wpcap.dll") + " (C, MinGW)", "inside Questlog's Node service process",
                  "implements the 11 libpcap calls Questlog makes and forwards them to the service; hosts the window-order guard"],
                 [c("questlog-capture") + " (C, libpcap)", "Linux, systemd socket activation",
                  "lists adapters, opens a capture, accepts only filters for the game's ports, streams packets"],
                 [c("questlog-ctl.exe") + " (C, MinGW)", "inside the game's session, on demand",
                  "hides or shows Questlog's own tray icon; hides its window before it is ended"],
                 [c("dwrite.dll") + " (C, MinGW)", "inside Questlog's processes (per-program DLL override)",
                  "forwards to Wine's DirectWrite; hides its variation interface so Chromium renders variable fonts with its own FreeType"],
                 [c("questlog-tray.py") + " (PySide6)", "Linux, " + c("systemd-run --user"), "tray icon with Open and Quit"],
                 [c("install.sh"), "Linux", "install, update, uninstall"]],
                [38 * mm, 42 * mm, 90 * mm]), Spacer(1, 6)]
story += [P("Sequence", h2)]
story += [P("1. Steam runs " + c("questlog-linux %command%") + ". It exports " + c("STEAM_COMPAT_LAUNCHER_SERVICE=proton")
            + ", which makes Proton expose a launcher service for the game's container, starts its watcher in the background "
            "and then runs the game's command exactly as Steam gave it.")]
story += [P("2. The watcher waits for that service and runs commands inside the container with "
            + c("steam-runtime-launch-client --inside-app=3393110") + ". They get Proton's environment and join the game's wineserver.")]
story += [P("3. Before starting Questlog it puts the bridge DLL in " + c("System32\\Npcap") + ", installs Questlog if it is missing, "
            "updates it if a newer version exists (4.8), applies two per-program Wine settings, then starts "
            + c("Questlog.exe --hidden") + ".")]
story += [P("4. Questlog finds the game as on Windows (process list, " + c("GetExtendedTcpTable") + ", the game window), "
            "picks the adapter whose IPv4 address matches the game connection, compiles its filter and reads packets "
            "with " + c("pcap_next_ex") + ", all through the bridge.")]
story += [P("5. When the game exits, Steam takes the game's session down a few seconds later, which ends Questlog too; "
            "the watcher ends anything left 15 s after the game.")]
story += [P("Bridge protocol", h2)]
story += [P("One TCP connection per " + c("pcap_findalldevs") + " or " + c("pcap_open_live") + " call. Requests are text lines; "
            "answers are frames " + c("[type u8][length u32 LE][payload]") + ".")]
story += [code_block(
    "LIST                                  -> D (one per adapter) ... Z\n"
    "                                         D = name\\0 description\\0 flags u32, count u32, count x (ipv4, mask)\n"
    "OPEN <snaplen> <promisc> <ms> <dev>   -> K (link type u32) | E (error text)\n"
    "FILTER <expression>                   -> K | E\n"
    "(after FILTER)                        -> P frames: sec u32, usec u32, caplen u32, len u32, packet bytes")]
story += [P("Adapters are reported with their Linux names and IPv4 addresses (the loopback one as " + c("\\Device\\NPF_Loopback")
            + "); " + c("any") + " and adapters without IPv4 are left out. Linux cooked captures (SLL, SLL2) are passed on as raw IP. "
            "The service uses libpcap's immediate mode and an 8 MB buffer.", body)]

story += [PageBreak(), P("4. What went wrong under Wine and Proton, and why each fix", h1)]
story += [problem("4.1 Npcap cannot exist under Wine",
                  "Questlog needs " + c("C:\\Windows\\System32\\Npcap\\wpcap.dll") + " to read the game's traffic.",
                  "Npcap is a Windows kernel driver. A Wine process inside Steam's container cannot open raw sockets either.",
                  "A stand-in " + c("wpcap.dll") + " with the 11 functions Questlog calls, forwarding to a capture service on the "
                  "Linux side (3, 5). Questlog's Npcap warnings stay away: it only checks that the file exists; there is no "
                  + c("npcap") + " service or " + c("AdminOnly") + " value in the prefix.")]
story += [problem("4.2 Questlog has to be in the game's Wine session",
                  "Started in its own prefix, Questlog does not see the game.",
                  "It finds the game by process name (Toolhelp), its connections with " + c("GetExtendedTcpTable")
                  + " and positions overlays from the game window. All three only work inside the same wineserver.",
                  "Steam's own launcher service (" + c("STEAM_COMPAT_LAUNCHER_SERVICE=proton") + ") and "
                  + c("steam-runtime-launch-client --inside-app") + ". The game starts exactly as without the wrapper.")]
story += [problem("4.3 The installer quits with exit code 2",
                  "The silent NSIS installer stops without installing.",
                  "Wine ships a PowerShell stub that reports success for every script. The installer's “is Questlog running?” "
                  "check therefore always says yes; it tries to close Questlog and gives up.",
                  c("powershell.exe") + " is disabled (" + c("WINEDLLOVERRIDES=powershell.exe=d") + ") for Questlog's process "
                  "tree only. With PowerShell absent, the installer and electron-updater both take their “PowerShell not "
                  "available” paths: the updater skips the Authenticode check (its SHA-512 check of the download remains).")]
story += [problem("4.4 The top of the window was cut off",
                  "Questlog's own title bar was hidden under a second one.",
                  "The window manager decorated Questlog's frameless window (" + c("titleBarStyle: hidden") + ").",
                  "Wine setting " + c("Decorated=N") + " for " + c("Questlog.exe") + " only.")]
story += [problem("4.5 Alt+Tab from the game to Questlog did nothing",
                  "Questlog was listed in Alt+Tab but selecting it while the game had focus did nothing.",
                  "With " + c("WM_TAKE_FOCUS") + ", KWin under Wayland/XWayland hands focus over with an X timestamp 5–8 s old. "
                  "The game keeps refreshing its own focus, so the X server drops Questlog's " + c("XSetInputFocus") + ".",
                  "Wine setting " + c("UseTakeFocus=N") + " for " + c("Questlog.exe") + ": the window manager sets the focus itself.",
                  ("Note", "The black thumbnail in Alt+Tab while the game is in front is Questlog's own " + c("setCovered")
                   + " behaviour (views hidden when the game covers the window), not a Linux issue."))]
story += [problem("4.6 Alt+E (overlay controls) showed nothing",
                  "The hotkey fired, but the controls never appeared over a fullscreen game.",
                  "The controls are drawn in the screen-sized transparent HUD window (title “questlog.gg - …”). Wine hands "
                  "screen-sized popups to the window manager, and KWin keeps the active fullscreen game above managed "
                  "windows. The small overlay widgets are override-redirect and were not affected.",
                  "A KWin window rule for that one window: layer “popup” (title contains “" + c("questlog.gg - ") + "”"
                  + ", class contains " + c("steam_app_") + "). The HUD stays click-through. A short hitch when the "
                  "controls open is expected (KWin starts compositing it).")]
story += [problem("4.7 Clicks in the game could reach Questlog",
                  "With Questlog's main window open behind the game, an Alt+click in the game can activate Questlog.",
                  "While the game is in front it confines the cursor; winex11 does that with a pointer grab on a screen-sized "
                  "clip window, and wineserver gives such clicks to the window under the cursor in Wine's own z-order. That "
                  "order is not updated when the window manager raises the game, so Questlog's window can still count as above it.",
                  "A window-order guard in the bridge DLL (it runs inside Questlog): every 50 ms, if the foreground window belongs "
                  "to another program, Questlog's ordinary windows above it in Wine's order are moved just below it "
                  "(" + c("SWP_NOACTIVATE") + "). Topmost and non-activatable windows (the overlays) are untouched, nothing is "
                  "activated, and winex11 does not restack managed windows downwards, so nothing visible changes. Questlog's "
                  "“covered” logic uses the foreground window, not the z-order, so it is unaffected.",
                  ("Seen in play", "In the first minutes of a real session the guard corrected the order twice, so that state "
                   "does occur. Off with " + c("QL_WINDOW_GUARD=0") + "; it logs to " + c("bridge.log") + "."))]
story += [problem("4.8 Updates",
                  "Questlog's updater downloads new versions, but they are never installed.",
                  "electron-updater installs on a normal quit (" + c("autoInstallOnAppQuit") + ") or from its prompt. Under this "
                  "wrapper Questlog is ended from outside (or by Steam taking the session down), so the quit step never runs.",
                  "At every game start, before Questlog starts, the launcher reads " + c("latest.yml") + ". For a newer "
                  "version it takes the installer Questlog's updater already left in " + c("@questlogdesktop-app-updater\\pending")
                  + " or downloads it, verifies the SHA-512, unpacks it with 7-Zip and checks that the new app still loads "
                  + c("System32\\Npcap\\wpcap.dll") + " and calls only functions the bridge has. Then it runs the official "
                  "installer silently (" + c("/S --updated") + "). If the new version ends within 60 s, the previous "
                  "installer (kept in the cache) goes back and that version is skipped. Questlog's own updater is left on and unchanged.",
                  ("Status", "Every step tested offline with fake and real installers; not yet with a real new release."))]
story += [problem("4.9 Quitting without losing data",
                  "Ending Questlog from outside skips its own quit step.",
                  "Questlog writes " + c("state.json") + " 0.5 s after a change and flushes it, and stops its workers, in "
                  + c("will-quit") + ", which only " + c("app.quit()") + " reaches. From outside, " + c("WM_CLOSE")
                  + " goes through the <i>closeAction</i> setting (default “ask”: a prompt), and " + c("WM_ENDSESSION")
                  + " makes Electron end the process at once.",
                  "“Quit” in our tray and " + c("questlog-linux stop") + " hide the window, wait 12 s, then end Questlog's "
                  "processes. The 12 s are a margin for pending writes; the workers' own write timing was not measured. "
                  "Suggestion 8.1 would make this clean.")]
story += [problem("4.10 Questlog's tray menu",
                  "Under Wine, the tray menu of Electron apps opens only the first time.",
                  "Electron takes the foreground before showing the menu; wineserver allows that only once per window when "
                  "the click came through the desktop's tray host. This is how it is reported for Electron apps under Wine; the menu was replaced here rather than the problem reproduced in play.",
                  "Our own tray icon (Qt StatusNotifierItem, drawn by the desktop) with Open and Quit. Questlog's icon is "
                  "hidden with " + c("Shell_NotifyIcon(NIM_MODIFY, NIS_HIDDEN)") + " on its own icon window only, and shown "
                  "again if our icon goes away while Questlog runs.")]
story += [problem("4.11 Smaller things",
                  "Host tools failed when started from the launch option; the app log filled up; capture refused right after a reconnect.",
                  "Steam puts its runtime libraries and overlay in " + c("LD_LIBRARY_PATH") + "/" + c("LD_PRELOAD")
                  + " (host curl failed with an undefined symbol). Chromium under Wine logs a " + c("network_change_notifier")
                  + " error every second. Questlog's filter can still name ports of connections the game has just closed.",
                  "Both variables are removed for everything but the game. That one log line is filtered out. The service "
                  "captures the ports that are still the game's and logs the others (it refuses only when none is).")]

story += [problem("4.12 The interface fonts looked wrong",
                  "Text in the window and the overlays looked thin and odd; the Questlog team noticed it on a screenshot.",
                  "Questlog's fonts are variable fonts: Encode Sans (the file is Encode Sans Condensed, axis wght 100-900, "
                  "default instance Thin) and Noto Sans (wght 100-900). Chromium on Windows renders a variable web font through "
                  "DirectWrite when its font faces offer " + c("IDWriteFontFace5") + ", and with its own FreeType otherwise. "
                  "Wine's DirectWrite offers the interface but does not apply axis values when rendering (Proton 11: "
                  + c("GetFontAxisValues") + " is a stub, and its FreeType rasterizer never sets variation coordinates). "
                  "So every weight came out as one instance: all Encode Sans text Thin, all Noto Sans text one weight.",
                  "A " + c("dwrite.dll") + " stand-in, loaded by " + c("Questlog.exe") + " only (per-program DLL override "
                  + c("dwrite=native,builtin") + " and a copy in Questlog's folder). It forwards " + c("DWriteCreateFactory")
                  + " to Wine's DirectWrite and makes font faces answer “no” to " + c("IDWriteFontFace5/6/7")
                  + ". Chromium then renders the variable fonts with FreeType, which applies the weights, as it does on older "
                  "Windows. Everything else is Wine's DirectWrite. The game is not affected. Off with " + c("QL_FONT_FIX=0") + ".",
                  ("Tested", "Same sample rendered by the real Questlog under Proton, without and with the stand-in (below); "
                   + c("tests/fontfix-test.c") + " checks the interfaces."))]
story += [KeepTogether([table([["Before: every weight alike", "After: 100 to 900, as designed"],
                               [Image(os.path.join(HERE, "fonts-before.png"), width=82 * mm, height=82 * mm * 262 / 560),
                                Image(os.path.join(HERE, "fonts-after.png"), width=82 * mm, height=82 * mm * 262 / 560)]],
                              [85 * mm, 85 * mm]),
                        P("Encode Sans at weights 100, 300, 400, 500, 600, 700, 900, Noto Sans at 400, 600, 700, and Encode Sans "
                          "400 and 700 at 20 px, captured from Questlog 0.1.48 through its DevTools protocol.", small)])]

story += [P("5. Security and privacy", h1)]
story += B(["Capture runs on the Linux side, as the user, with one capability (" + c("CAP_NET_RAW") + ") granted by systemd, plus "
            + c("NoNewPrivileges") + ", " + c("ProtectSystem=strict") + ", " + c("ProtectHome") + ", " + c("PrivateTmp")
            + ", " + c("PrivateDevices") + ", restricted address families, a " + c("@system-service") + " syscall filter and a one-day runtime limit.",
            "The socket listens on 127.0.0.1 only, and every connection must come from the same user (checked in "
            + c("/proc/net/tcp") + ").",
            "Only filters of the form " + c("tcp and (port A or port B …)") + " are accepted. Every port must be the local port "
            "of a TCP socket owned by a process whose program is " + c("AION2.exe") + ". The filter is rebuilt from the checked "
            "ports, never passed through, and checked again every 5 s (capture pauses while the ports are not the game's). "
            "Promiscuous mode is never used.",
            "No traffic is written to disk or sent anywhere. Questlog's files are not modified; the prefix gets the bridge DLL, "
            "the font fix in Questlog's folder and three per-program Wine values (all for " + c("Questlog.exe") + " only)."])

story += [P("6. Measurements", h1)]
story += [P("Delay from capture to Questlog processing, taken from Questlog's own encounter index: for fights that end in "
            "a kill, " + c("savedAt - (startedAt + durationSeconds)") + ". 64 kills over about 6 hours of play on one day:")]
story += [table([["Mean", "Median", "Max", "First 32 kills", "Last 32 kills"],
                 ["0.079 s", "0.067 s", "0.49 s", "0.073 s", "0.086 s"]], [26 * mm, 26 * mm, 26 * mm, 46 * mm, 46 * mm]),
          Spacer(1, 4), P("No drift over the session.", small)]

story += [P("7. Known limitations and not tested yet", h1)]
story += B(["<b>Tested on one system</b> (see the first page). The KWin rule exists only for KDE Plasma; on other desktops the "
            "overlay controls may stay under a fullscreen game (borderless window mode may help, untested).",
            "<b>Not tested yet:</b> Discord sign-in inside Questlog; an update to a real new release; other games than AION 2 "
            "(" + c("QL_APPID") + " and " + c("QL_GAME_EXE") + " exist but are untested).",
            "Steam as Flatpak is not supported."])

story += [P("8. Suggestions for the Questlog app", h1)]
story += [P("Small changes on Questlog's side that would make Linux support cleaner. None is needed for this package to work.")]
story += [P("8.1 A " + c("--quit") + " switch", label),
          P("Handled by the running instance (" + c("second-instance") + " → " + c("app.quit()") + "). Wrappers could then end "
            "Questlog with its normal quit step: data flushed in " + c("will-quit") + ", and the updater's install-on-quit working.")]
story += [P("8.2 Flush on " + c("session-end"), label),
          P("Do the " + c("will-quit") + " work (flush the store, stop the workers) on the window's " + c("session-end") +
            " event too. That also protects Windows users at logoff/shutdown.")]
story += [P("8.3 An overridable Npcap folder (optional)", label),
          P("For example an environment variable read before " + c("%SystemRoot%\\System32\\Npcap") + ", so a bridge does not "
            "have to live in System32.")]

story += [P("8.4 Static font files (optional)", label),
          P("If Questlog shipped static (non-variable) font files for the weights it uses, Wine would render them correctly "
            "without the font fix of 4.12.")]

story += [P("9. What is in the package", h1)]
story += [table([["Path", "What"],
                 [c("install.sh"), "the installer (" + c("--build") + ", " + c("--uninstall") + ", " + c("--help") + ")"],
                 [c("questlog-linux"), "the launcher"],
                 [c("bin/"), "ready-made " + c("wpcap.dll") + ", " + c("questlog-ctl.exe") + ", " + c("dwrite.dll") + ", " + c("questlog-capture")
                  + " (built on Fedora 44 from these sources)"],
                 [c("wpcap/wpcap.c"), "the bridge DLL and the window-order guard"],
                 [c("capture/questlog-capture.c"), "the capture service"],
                 [c("ctl/questlog-ctl.c"), "the helper for Questlog's tray icon and window"],
                 [c("fontfix/dwrite.c"), "the DirectWrite stand-in (font fix)"],
                 [c("tray/questlog-tray.py"), "the tray icon"],
                 [c("systemd/"), "the socket and service units"],
                 [c("tests/"), "Windows test programs for the guard, the helper and the font fix"],
                 [c("Makefile"), c("make") + " builds everything (gcc, libpcap headers, MinGW-w64); " + c("make tests") + " the tests"]],
                [52 * mm, 118 * mm])]
story += [P("Tests", h2)]
story += [P("Run them under Wine in a throwaway prefix, never the game's. Each writes PASS or FAIL (to stdout, or to the file "
            "named by " + c("QL_TEST_OUT") + ", since Proton's " + c("run") + " does not pass console output on).")]
story += [code_block("make tests\n"
                     "wine build/tests/zorder-test.exe build/wpcap.dll build/tests/fake-game.exe\n"
                     "    -> PASS\n"
                     "QL_WINDOW_GUARD=0 wine build/tests/zorder-test.exe build/wpcap.dll build/tests/fake-game.exe\n"
                     "    -> FAIL (shows that the test detects the problem)\n"
                     "wine build/tests/tray-test.exe build/questlog-ctl.exe\n"
                     "    -> PASS\n"
                     "cp build/dwrite.dll build/tests/ && WINEDLLOVERRIDES=dwrite=n,b wine build/tests/fontfix-test.exe\n"
                     "    -> PASS (and FAIL with WINEDLLOVERRIDES=dwrite=b)")]
story += [P("The capture service was tested with a client speaking the bridge protocol: the game's ports were captured "
            "(242 packets in 5 s during play), any other port was refused. All tests above passed on the tested system.")]


def on_page(canvas, doc):
    canvas.saveState()
    canvas.setFont("Sans", 7.5)
    canvas.setFillColor(MUTED)
    canvas.drawString(20 * mm, 12 * mm, f"Questlog on Linux · questlog-linux {VERSION}")
    canvas.drawRightString(190 * mm, 12 * mm, str(doc.page))
    canvas.restoreState()


doc = SimpleDocTemplate(OUT, pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=18 * mm, bottomMargin=20 * mm,
                        title="Questlog on Linux", author="Gabriel Lopes", subject=f"questlog-linux {VERSION}")
doc.build(story, onFirstPage=on_page, onLaterPages=on_page)
print("ok", OUT)
