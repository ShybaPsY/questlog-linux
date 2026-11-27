#!/bin/bash
# questlog-linux installer: runs the official Questlog desktop app for AION 2 on Linux, inside
# the game's Steam Proton session. Run it as your normal user; sudo is asked for only to install
# the capture service (binary in /usr/local/libexec, units in /etc/systemd/system).
#
#   ./install.sh               install or update, with the ready-made programs in bin/
#   ./install.sh --build       compile everything from the sources first, then install
#   ./install.sh --uninstall   remove everything this installer put on the system
#
# Afterwards set Steam > AION 2 > Properties > General > Launch Options to the line it prints
# (~/.local/bin/questlog-linux %command%, written out as a full path).
set -eu
cd "$(dirname "$(readlink -f "$0")")"

VERSION=1.1.0
USER_NAME=$(id -un)
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/questlog-linux"
BIN="$HOME/.local/bin/questlog-linux"
UNIT_DIR=/etc/systemd/system
HELPER=/usr/local/libexec/questlog-capture
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICON="$DATA_DIR/questlog.png"
MODE=install
case "${1:-}" in
    "") ;;
    --build) MODE=build ;;
    --uninstall) MODE=uninstall ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see ./install.sh --help)"; exit 2 ;;
esac

if [ "$(id -u)" = 0 ]; then
    echo "Run this as your normal user, not root (it asks for sudo when needed)."
    exit 1
fi

# KDE Plasma (KWin 6.7+): Questlog's overlay layer ("hud") is a transparent, click-through,
# screen-sized window. Wine hands screen-sized popups to the window manager, and KWin keeps an
# active fullscreen game above normal windows, so the overlay controls (Alt+E) ended up behind
# the game. This rule puts that one window in the popup layer. Other rules are left untouched.
KWIN_RULE=questlog-hud
kwin_rule() {
    command -v kwriteconfig6 >/dev/null && command -v kreadconfig6 >/dev/null || return 0
    local f=kwinrulesrc rules
    rules=$(kreadconfig6 --file $f --group General --key rules)
    if [ "$1" = add ]; then
        # already there with the same settings: nothing to write, and no KWin reload mid-game
        case ",$rules," in *",$KWIN_RULE,"*)
            [ "$(kreadconfig6 --file $f --group $KWIN_RULE --key layer)" = popup ] \
                && [ "$(kreadconfig6 --file $f --group $KWIN_RULE --key layerrule)" = 2 ] \
                && [ "$(kreadconfig6 --file $f --group $KWIN_RULE --key title)" = "questlog.gg - " ] \
                && [ "$(kreadconfig6 --file $f --group $KWIN_RULE --key wmclass)" = steam_app_ ] && return 0 ;;
        esac
        case ",$rules," in *",$KWIN_RULE,"*) ;; *) rules="${rules:+$rules,}$KWIN_RULE" ;; esac
        kwriteconfig6 --file $f --group $KWIN_RULE --key Description "Questlog overlay layer (above fullscreen games)"
        kwriteconfig6 --file $f --group $KWIN_RULE --key title "questlog.gg - "
        kwriteconfig6 --file $f --group $KWIN_RULE --key titlematch 2
        kwriteconfig6 --file $f --group $KWIN_RULE --key wmclass "steam_app_"
        kwriteconfig6 --file $f --group $KWIN_RULE --key wmclassmatch 2
        kwriteconfig6 --file $f --group $KWIN_RULE --key layer popup
        kwriteconfig6 --file $f --group $KWIN_RULE --key layerrule 2
    else
        case ",$rules," in *",$KWIN_RULE,"*) ;; *) return 0 ;; esac
        rules=$(printf '%s' "$rules" | tr ',' '\n' | grep -vx "$KWIN_RULE" | paste -sd, -)
        for k in Description title titlematch wmclass wmclassmatch layer layerrule; do
            kwriteconfig6 --file $f --group $KWIN_RULE --key $k --delete
        done
    fi
    kwriteconfig6 --file $f --group General --key rules "$rules"
    kwriteconfig6 --file $f --group General --key count "$(printf '%s' "$rules" | tr ',' '\n' | grep -c . || true)"
    dbus-send --session --type=method_call --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure 2>/dev/null || true
}

if [ "$MODE" = uninstall ]; then
    # take the bridge DLL out of the game prefix, if the launcher can find it
    if [ -x "$BIN" ] && pfx=$("$BIN" status 2>/dev/null | sed -n 's/^game prefix:[[:space:]]*//p') && [ -d "$pfx" ]; then
        rm -f "$pfx/drive_c/windows/system32/Npcap/wpcap.dll"
        rmdir "$pfx/drive_c/windows/system32/Npcap" 2>/dev/null || true
        # the font fix in Questlog's folder (its DllOverrides value alone does nothing without it)
        f="$pfx/drive_c/Program Files/Questlog/dwrite.dll"
        [ -f "$f" ] && grep -aqF "variable web fonts use Chromium's FreeType" "$f" && rm -f "$f"
    fi
    systemctl --user stop questlog-linux-tray.service 2>/dev/null || true
    sudo systemctl disable --now questlog-capture.socket 2>/dev/null || true
    sudo rm -f "$UNIT_DIR/questlog-capture.socket" "$UNIT_DIR/questlog-capture@.service" "$HELPER"
    sudo systemctl daemon-reload
    rm -rf "$DATA_DIR"
    rm -f "$BIN" "$APPS_DIR/questlog-linux.desktop"
    kwin_rule remove
    echo "Removed. Also clear AION 2's launch options in Steam."
    echo "Questlog itself stays installed in the game's prefix; its data is in the prefix's AppData."
    exit 0
fi

echo "questlog-linux $VERSION"

# ---- what the system needs --------------------------------------------------------------------
missing=""
need() { command -v "$1" >/dev/null || missing="$missing $2"; }
need curl curl
need openssl openssl
need python3 python3
need systemctl systemd
need sudo sudo
{ ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null; } | grep -q 'libpcap\.so\.1 ' || missing="$missing libpcap(runtime library)"

# Ready-made programs, or compile them (--build, or when bin/ is missing or does not fit this system).
SRC=bin
[ "$MODE" = build ] && SRC=build
for f in wpcap.dll questlog-ctl.exe dwrite.dll questlog-capture noshape.so; do [ -f "bin/$f" ] || SRC=build; done
if [ "$SRC" = bin ] && ldd bin/questlog-capture 2>&1 | grep -q -e 'not found' -e 'version .* not found'; then
    if ldd bin/questlog-capture 2>&1 | grep -q 'GLIBC'; then
        echo "The ready-made capture service needs a newer glibc than this system has; compiling it instead."
        SRC=build
    fi
fi
if [ "$SRC" = build ]; then
    need make make
    need gcc gcc
    need x86_64-w64-mingw32-gcc "mingw-w64-gcc(x86_64-w64-mingw32-gcc)"
    [ -f /usr/include/pcap/pcap.h ] || missing="$missing libpcap-development-headers"
fi
if [ -n "$missing" ]; then
    echo "Missing:$missing"
    echo "  Fedora:        sudo dnf install curl openssl python3 libpcap   (to compile: make gcc mingw64-gcc libpcap-devel)"
    echo "  Debian/Ubuntu: sudo apt install curl openssl python3 libpcap0.8 (libpcap0.8t64 on Ubuntu 24.04+)   (to compile: make gcc gcc-mingw-w64-x86-64 libpcap-dev)"
    echo "  Arch/SteamOS:  sudo pacman -S curl openssl python libpcap   (to compile: make gcc mingw-w64-gcc)"
    exit 1
fi
optional=""
/usr/bin/python3 -c 'import PySide6' 2>/dev/null || optional="$optional
  PySide6 for the system Python 3 (Fedora python3-pyside6, Arch pyside6, Debian/Ubuntu python3-pyside6.qtwidgets where available): a tray icon whose menu works under Wine"
command -v 7z >/dev/null || optional="$optional
  7-Zip (Fedora 7zip, Debian/Ubuntu 7zip or p7zip-full, Arch 7zip): checks a Questlog update before installing it"
command -v xdotool >/dev/null || optional="$optional
  xdotool: brings Questlog's window to the front when it is opened from the menu"
command -v notify-send >/dev/null || optional="$optional
  notify-send (libnotify): desktop notifications about updates and problems"
[ -n "$optional" ] && echo "Optional, not installed:$optional"
ls "$HOME"/.local/share/Steam/steamapps/common/SteamLinuxRuntime_*/pressure-vessel/bin/steam-runtime-launch-client >/dev/null 2>&1 \
    || echo "Note: no Steam Linux Runtime found in ~/.local/share/Steam yet. Steam installs it with Proton; start AION 2 once before using Questlog."
if [ -n "${FLATPAK_ID:-}" ] || [ -d "$HOME/.var/app/com.valvesoftware.Steam" ]; then
    echo "Note: the Flatpak build of Steam is not supported (the game's container is not reachable from outside the sandbox)."
fi

if [ "$SRC" = build ]; then
    echo "Compiling..."
    make
fi

# ---- capture service (system-wide, needs sudo) --------------------------------------------------
# Reinstalled (and restarted) only when it changed, so an update never interrupts a capture.
service_tmp=$(mktemp)
sed "s/@USER@/$USER_NAME/" systemd/questlog-capture@.service > "$service_tmp"
if cmp -s "$SRC/questlog-capture" "$HELPER" && cmp -s "$service_tmp" "$UNIT_DIR/questlog-capture@.service" \
    && cmp -s systemd/questlog-capture.socket "$UNIT_DIR/questlog-capture.socket"; then
    echo "Capture service unchanged."
    sudo systemctl enable --now questlog-capture.socket
else
    echo "Installing the capture service (sudo)..."
    sudo install -D -m 0755 "$SRC/questlog-capture" "$HELPER"
    sudo install -m 0644 "$service_tmp" "$UNIT_DIR/questlog-capture@.service"
    sudo install -m 0644 systemd/questlog-capture.socket "$UNIT_DIR/questlog-capture.socket"
    command -v restorecon >/dev/null && sudo restorecon "$HELPER" "$UNIT_DIR/questlog-capture.socket" "$UNIT_DIR/questlog-capture@.service"
    sudo systemctl daemon-reload
    sudo systemctl enable --now questlog-capture.socket
    sudo systemctl restart questlog-capture.socket
fi
rm -f "$service_tmp"

# ---- per-user parts -------------------------------------------------------------------------------
# Each file is written next to its place and renamed over it: programs that are running from the
# old copy (the launcher is a bash script that is read while it runs) keep the old one.
put() { install -D -m "$1" "$2" "$3.new" && mv -f "$3.new" "$3"; }
put 0644 "$SRC/wpcap.dll" "$DATA_DIR/wpcap.dll"
put 0644 "$SRC/questlog-ctl.exe" "$DATA_DIR/questlog-ctl.exe"
put 0644 "$SRC/dwrite.dll" "$DATA_DIR/dwrite.dll"
put 0644 "$SRC/noshape.so" "$DATA_DIR/noshape.so"
put 0755 tray/questlog-tray.py "$DATA_DIR/questlog-tray.py"
put 0755 questlog-linux "$BIN"
"$BIN" _icon || true   # Questlog's own icon, taken from its installation (once it is installed)
mkdir -p "$APPS_DIR"
cat > "$APPS_DIR/questlog-linux.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Questlog (AION 2)
Comment=Open the Questlog window (AION 2 must be running)
Exec=$BIN show
Icon=$( [ -f "$ICON" ] && echo "$ICON" || echo applications-games )
Terminal=false
Categories=Game;
EOF
kwin_rule add

echo
echo "Done. In Steam, set AION 2 > Properties > General > Launch Options to:"
echo "    $BIN %command%"
echo "Questlog is installed in the game's prefix on the first launch and starts in the tray."
echo "Check with: $BIN status"
