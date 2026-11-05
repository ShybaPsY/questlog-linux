# questlog-linux

Runs the official [Questlog](https://questlog.gg) desktop app for AION 2 on Linux, inside the game's own Steam Proton session: live combat meter, overlays and hotkeys. Questlog itself is not modified; it is installed with its official installer and runs as it does on Windows.

Made with the Questlog team's knowledge. Not an official Questlog product, and it contains no Questlog code.

## Install

1. Download `questlog-linux-<version>.zip` from [Releases](https://github.com/ShybaPsY/questlog-linux/releases), unpack it and run, as your normal user:

   ```
   ./install.sh
   ```

   It asks for sudo once, to install the capture service. From a clone of this repository, use `./install.sh --build` (needs `make`, `gcc`, libpcap headers and MinGW-w64).

2. In Steam, open AION 2 › Properties › General › Launch Options and paste the line the installer prints, for example:

   ```
   /home/<user>/.local/bin/questlog-linux %command%
   ```

3. Play. On the first launch Questlog is downloaded and installed in the game's prefix. From then on it starts in the tray with the game and closes after it.

**Needs:** Steam as a normal package (not Flatpak), systemd, sudo, curl, openssl, python3 and libpcap. The ready-made programs need glibc 2.34 or newer. Optional: PySide6 (tray icon), 7-Zip (update check), xdotool, notify-send.

**Tested on:** Fedora 44, KDE Plasma 6.7 (Wayland), Proton 11, Questlog 0.1.48. Other distributions and desktops are untested.

## Use

| Command | What it does |
|---|---|
| `questlog-linux show` | open Questlog's window (the game must be running) |
| `questlog-linux stop` | close Questlog (waits 12 s first, so it can save) |
| `questlog-linux status` | what is installed and running |
| `questlog-linux log` | the logs |
| `./install.sh --uninstall` | remove everything the installer added |

## How it works

- Npcap cannot run under Wine. A stand-in `wpcap.dll` implements the libpcap calls Questlog makes and forwards them to `questlog-capture`, a small Linux service that captures only the game's own TCP connections.
- The launcher runs Questlog inside the game's Wine session through Steam's launcher service, so Questlog sees the game process, its connections and its window as on Windows.
- It also works around what comes with sharing a Wine session with a fullscreen game: window focus, the overlay layer under KDE, the tray menu, clicks reaching Questlog, updates and quitting.

[Questlog-on-Linux.pdf](Questlog-on-Linux.pdf) explains each part, every problem found under Wine/Proton and why each fix was chosen, plus security, measurements and known limits.

## License

MIT. See [LICENSE](LICENSE).
