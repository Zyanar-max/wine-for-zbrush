#!/bin/sh
# Wine for ZBrush: opens a ZBrush file (.ztl, .zpr, .zbp, ...) in ZBrush.
# Installed by install.sh with the @...@ values filled in.
#
# No file:     start ZBrush, unless it is already running (then only a notification).
# Not running: start ZBrush from Bottles with the file.
# Running:     start a second ZBrush.exe inside the SAME Wine session. Like on Windows it hands the
#              file to the running ZBrush and exits. (Bottles Flatpak runs every launch in its own
#              sandbox with its own wineserver; a second ZBrush there can't see the first one, and
#              the two prefixes' servers break each other.)

CLI='@CLI@'
BOTTLE='@BOTTLE@'
PROGRAM='@PROGRAM@'
PREFIX='@PREFIX@'

winpath() { printf 'Z:%s' "$(realpath -- "$1" | sed 's|/|\\|g')"; }

# Bottles Flatpak sometimes leaves a dead session behind: Wine's services (services.exe,
# winedevice.exe, ...) keep running in the old sandbox after its wineserver is gone, and the next
# start on the same bottle can hang without a window. Close such a sandbox, but only if it belongs
# to this bottle, its wineserver is gone, and no program and no Bottles window is left in it.
cleanup_stale() {
    case "$CLI" in flatpak*) ;; *) return 0 ;; esac
    for p in /proc/[0-9]*; do
        info="$p/root/.flatpak-info"
        grep -qxF 'name=com.usebottles.bottles' "$info" 2>/dev/null || continue
        inst=$(sed -n 's/^instance-id=//p' "$info" 2>/dev/null)
        [ -n "$inst" ] || continue
        if tr '\0' '\n' < "$p/environ" 2>/dev/null | grep -qxF "WINEPREFIX=$PREFIX"; then mine=1; else mine=0; fi
        case "$(cat "$p/comm" 2>/dev/null)" in
        services.exe|winedevice.exe|plugplay.exe|svchost.exe|rpcss.exe|lsass.exe|explorer.exe|conhost.exe|tabtip.exe|rundll32.exe)
            kind=service ;;
        bwrap|xdg-dbus-proxy|bottles-cli|start.exe|sh|bash)
            kind=other ;;
        *)  kind=busy ;;         # wineserver, a program, the Bottles window ...
        esac
        echo "$inst $kind $mine"
    done | awk '{ if ($2 == "busy") busy[$1] = 1; if ($2 == "service" && $3 == 1) left[$1] = 1 }
                END { for (i in left) if (!(i in busy)) print i }' |
    while read -r inst; do
        flatpak kill "$inst" 2>/dev/null && sleep 1
    done
    return 0
}

zpid=
for p in $(pgrep -x ZBrush.exe); do
    if tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qxF "WINEPREFIX=$PREFIX"; then
        zpid=$p
        break
    fi
done

# No file: start ZBrush, but never a second one (a second Bottles launch freezes Bottles and ZBrush).
# Also covers the few seconds while Bottles is still starting ZBrush (a launch younger than 90 s;
# older leftovers of finished sessions don't count).
starting() {
    for b in $(pgrep -f -- "bottles-cli run -b $BOTTLE -p $PROGRAM"); do
        [ "$(ps -o etimes= -p "$b" 2>/dev/null | tr -d ' ')" -lt 90 ] 2>/dev/null && return 0
    done
    return 1
}
if [ -z "$1" ]; then
    if [ -n "$zpid" ] || starting; then
        notify-send -u critical -i wine-for-zbrush "ZBrush is already running" "Only one ZBrush can run at a time."
        exit 0
    fi
    cleanup_stale
    exec $CLI run -b "$BOTTLE" -p "$PROGRAM"
fi
file=$(winpath "$1") || exit 1

# Bottles Flatpak only sees the folders it has access to (Flatseal / flatpak override).
# Without this check ZBrush just says "Unable to open" and a second ZBrush hangs.
case "$CLI" in
flatpak*)
    app=${CLI##* }
    if ! flatpak run --command=test "$app" -r "$(realpath -- "$1")" 2>/dev/null; then
        notify-send -u critical -i wine-for-zbrush "ZBrush can't see this file" \
            "$(basename -- "$1") is in a folder Bottles has no access to. Move it to a drive or folder Bottles can see, or give Bottles access to this folder (Flatseal)."
        exit 1
    fi
    ;;
esac

[ -z "$zpid" ] && { cleanup_stale; exec $CLI run -b "$BOTTLE" -p "$PROGRAM" "$file"; }

getenv() { tr '\0' '\n' < "/proc/$zpid/environ" | sed -n "s/^$1=//p" | head -n 1; }
exe=$(tr '\0' '\n' < "/proc/$zpid/cmdline" | head -n 1 | sed 's/ *$//')
runner=$(readlink "/proc/$zpid/exe")
runner=${runner%/lib/wine/*}

set -- env WINEPREFIX="$PREFIX" WINEARCH=win64 WINEDEBUG=-all \
    WINEFSYNC="$(getenv WINEFSYNC)" WINEESYNC="$(getenv WINEESYNC)" \
    DISPLAY="$(getenv DISPLAY)" LD_LIBRARY_PATH="$(getenv LD_LIBRARY_PATH)" \
    WINEDLLOVERRIDES="$(getenv WINEDLLOVERRIDES)" \
    "$runner/bin/wine" "$exe" "$file"

case "$CLI" in
flatpak*)
    # the Flatpak instance is the nearest ancestor of ZBrush that `flatpak ps` lists
    instances=$(flatpak ps --columns=instance,pid 2>/dev/null)
    p=$zpid
    while [ "${p:-1}" -gt 1 ]; do
        inst=$(printf '%s\n' "$instances" | awk -v pid="$p" '$2 == pid { print $1 }')
        [ -n "$inst" ] && exec flatpak enter "$inst" "$@"
        p=$(awk '{ print $4 }' "/proc/$p/stat" 2>/dev/null)
    done
    notify-send -u critical -i wine-for-zbrush "ZBrush is already running" \
        "Load $(basename -- "$1") from inside ZBrush (Load Tool / Load Brush ...)."
    ;;
*)
    exec "$@"
    ;;
esac
