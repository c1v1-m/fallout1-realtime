#!/usr/bin/env bash
# Fallout 1 Realtime -- installer for the STEAM version on a Steam Deck (or any Linux with Steam).
#
# Does what Fallout1-Realtime-Setup.exe does on Windows. Steam's Play button runs
# FalloutLauncher.exe (through Proton here), so that file is backed up as
# FalloutLauncher.original.exe and the game takes its place: Play then starts Fallout 1 Realtime,
# in Game Mode too -- no non-Steam shortcut, no Proton settings.
#
#   bash install-steamdeck.sh                        install (or update)
#   bash install-steamdeck.sh uninstall              put Steam's launcher back, remove our files
#   bash install-steamdeck.sh install --target DIR   a Fallout folder that was not found by itself
#
# fallout1-realtime.exe and aimmask.dat must sit next to this script. fallout.cfg and saved games
# are never touched. Steam's "Verify integrity of game files" also restores the original launcher.

set -u

VERSION="0.1.0" # kept equal to REALTIME_VERSION (src/game/version.h)
MARKER="Fallout 1 Realtime" # every build of the game carries it; Steam's launcher does not
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_EXE="$HERE/fallout1-realtime.exe"
AIMMASK="$HERE/aimmask.dat"

pause() {
    # Started by a double-click the terminal would vanish with the result; keep it readable.
    if [ -t 0 ]; then
        echo
        read -r -p "Press Enter to close..." _
    fi
}

fail() {
    echo "FAILED: $*"
    pause
    exit 1
}

# Linux file names are case-sensitive and the game's files are not (Proton resolves either way):
# find a name in one folder whatever its case. -type is optional (f or d).
ci_find() {
    local dir="$1" name="$2" type="${3:-}"
    if [ -n "$type" ]; then
        find "$dir" -maxdepth 1 -type "$type" -iname "$name" -print -quit 2>/dev/null
    else
        find "$dir" -maxdepth 1 -iname "$name" -print -quit 2>/dev/null
    fi
}

vdf_values() { # every "key" "value" pair's value for one key, from a Steam .vdf/.acf
    grep -o "\"$2\"[[:space:]]*\"[^\"]*\"" "$1" 2>/dev/null | sed "s/^\"$2\"[[:space:]]*\"\(.*\)\"$/\1/" | sed 's/\\\\/\\/g'
}

find_fallout() {
    local roots=(
        "$HOME/.local/share/Steam"
        "$HOME/.steam/steam"
        "$HOME/.steam/root"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
    )
    local libraries=() root lib seen known dir acf installdir
    for root in "${roots[@]}"; do
        [ -d "$root/steamapps" ] || continue
        libraries+=("$root")
        while IFS= read -r lib; do
            [ -n "$lib" ] && libraries+=("$lib")
        done < <(vdf_values "$root/steamapps/libraryfolders.vdf" path)
    done

    seen=()
    for lib in "${libraries[@]}"; do
        lib="$(readlink -f "$lib" 2>/dev/null || echo "$lib")"
        for known in "${seen[@]+"${seen[@]}"}"; do
            [ "$known" = "$lib" ] && continue 2
        done
        seen+=("$lib")

        acf="$lib/steamapps/appmanifest_38400.acf"
        [ -f "$acf" ] || continue
        installdir="$(vdf_values "$acf" installdir | head -n 1)"
        dir="$lib/steamapps/common/${installdir:-Fallout}"
        if [ -n "$(ci_find "$dir" master.dat f)" ]; then
            echo "$dir"
            return 0
        fi
    done
    return 1
}

ACTION="install"
TARGET=""
while [ $# -gt 0 ]; do
    case "$1" in
        install|uninstall) ACTION="$1" ;;
        --target) TARGET="${2:-}"; shift ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) fail "unknown argument: $1 (try --help)" ;;
    esac
    shift
done

echo "Fallout 1 Realtime $VERSION -- Steam installer for Steam Deck / Linux"
echo

if [ -z "$TARGET" ]; then
    TARGET="$(find_fallout)" || fail "the Steam version of Fallout was not found. Is it installed? If it is, run: bash install-steamdeck.sh $ACTION --target \"/path/to/Fallout\""
    echo "Found the Steam version of Fallout in $TARGET"
fi
TARGET="${TARGET%/}"
[ -d "$TARGET" ] || fail "$TARGET is not a folder."
if [ -z "$(ci_find "$TARGET" master.dat f)" ] || [ -z "$(ci_find "$TARGET" critter.dat f)" ]; then
    fail "$TARGET is not a Fallout 1 folder (MASTER.DAT and CRITTER.DAT are missing)."
fi

LAUNCHER="$(ci_find "$TARGET" falloutlauncher.exe f)"
BACKUP="$(ci_find "$TARGET" falloutlauncher.original.exe f)"
DATA="$(ci_find "$TARGET" data d)"
MANIFEST="$TARGET/Fallout1-Realtime.txt"
OURS=0
if [ -n "$LAUNCHER" ] && grep -qaF "$MARKER" "$LAUNCHER"; then
    OURS=1
fi

if [ "$ACTION" = "install" ]; then
    [ -f "$GAME_EXE" ] && [ -f "$AIMMASK" ] || fail "fallout1-realtime.exe and aimmask.dat must be in the same folder as this script ($HERE)."
    echo "Installing into $TARGET"

    if [ -n "$LAUNCHER" ] || [ -n "$BACKUP" ]; then
        if [ -z "$LAUNCHER" ]; then
            LAUNCHER="$TARGET/FalloutLauncher.exe"
        fi
        if [ -f "$LAUNCHER" ] && [ "$OURS" = 0 ]; then
            # Steam's launcher -- maybe a fresh one after "Verify integrity", so it replaces any older backup.
            cp -f "$LAUNCHER" "$TARGET/FalloutLauncher.original.exe" || fail "could not back up FalloutLauncher.exe."
            echo "  Backed up Steam's FalloutLauncher.exe as FalloutLauncher.original.exe."
        elif [ -z "$BACKUP" ]; then
            echo "  Note: there is no backup of Steam's original launcher. To go back to the original game,"
            echo "  use Steam: Fallout > Properties > Installed Files > Verify integrity of game files."
        fi
        # Beside it first, then over it: a failure never leaves half an exe where Play points.
        cp -f "$GAME_EXE" "$LAUNCHER.new" && mv -f "$LAUNCHER.new" "$LAUNCHER" || { rm -f "$LAUNCHER.new"; fail "could not write FalloutLauncher.exe. Is the game running? Close it and try again."; }
        if [ "$OURS" = 1 ]; then echo "  Updated the game (FalloutLauncher.exe)."; else echo "  Installed the game as FalloutLauncher.exe."; fi
        MODE="steam"
        FILES="FalloutLauncher.exe (the game; Steam's original is FalloutLauncher.original.exe)"
    else
        cp -f "$GAME_EXE" "$TARGET/fallout1-realtime.exe" || fail "could not write fallout1-realtime.exe."
        echo "  Installed fallout1-realtime.exe."
        MODE="plain"
        FILES="fallout1-realtime.exe"
    fi

    if [ -z "$DATA" ]; then
        DATA="$TARGET/data"
        mkdir -p "$DATA" || fail "could not create the data folder."
    fi
    cp -f "$AIMMASK" "$DATA/aimmask.dat" || fail "could not write aimmask.dat."
    echo "  Installed $(basename "$DATA")/aimmask.dat."

    {
        printf 'Fallout 1 Realtime\r\nversion=%s\r\nmode=%s\r\ninstalled=%s\r\n\r\n' "$VERSION" "$MODE" "$(date '+%Y-%m-%d %H:%M')"
        printf 'Files installed by install-steamdeck.sh:\r\n%s\r\n%s/aimmask.dat\r\nFallout1-Realtime.txt (this file)\r\n\r\n' "$FILES" "$(basename "$DATA")"
        printf 'To uninstall: bash install-steamdeck.sh uninstall\r\nfallout.cfg and saved games are never touched.\r\n'
    } > "$MANIFEST" || fail "could not write Fallout1-Realtime.txt."

    echo
    if [ "$MODE" = "steam" ]; then
        echo "Done. One more step before you play, in Game Mode:"
        echo "  Fallout's page > controller settings > Templates > \"Gamepad with Mouse Trackpad\"."
        echo "Steam's default layout for Fallout turns the buttons into keyboard keys, and the game"
        echo "then never sees a controller. After that, press Play on Fallout to start Fallout 1 Realtime."
    else
        echo "Done. This is not the Steam version: start fallout1-realtime.exe from that folder with Proton or Wine."
    fi
    pause
    exit 0
fi

# --- uninstall ---
if [ "$OURS" = 0 ] && [ -z "$BACKUP" ] && [ ! -f "$TARGET/fallout1-realtime.exe" ] && [ ! -f "$MANIFEST" ]; then
    echo "Fallout 1 Realtime is not installed in $TARGET. Nothing to do."
    pause
    exit 0
fi

echo "Uninstalling from $TARGET"
if [ "$OURS" = 1 ]; then
    if [ -n "$BACKUP" ]; then
        mv -f "$BACKUP" "$LAUNCHER" || fail "could not restore Steam's FalloutLauncher.exe. Is the game running? Close it and try again."
        echo "  Restored Steam's original FalloutLauncher.exe."
    else
        echo "  The backup of Steam's launcher is missing, so Play still starts Fallout 1 Realtime."
        echo "  To get the original back: Fallout > Properties > Installed Files > Verify integrity of game files."
    fi
elif [ -n "$BACKUP" ]; then
    rm -f "$BACKUP" && echo "  Removed the old launcher backup (Steam had already restored its launcher)."
fi
[ -f "$TARGET/fallout1-realtime.exe" ] && rm -f "$TARGET/fallout1-realtime.exe" && echo "  Removed fallout1-realtime.exe."
if [ -n "$DATA" ] && [ -f "$DATA/aimmask.dat" ]; then
    rm -f "$DATA/aimmask.dat" && echo "  Removed $(basename "$DATA")/aimmask.dat."
fi
[ -f "$MANIFEST" ] && rm -f "$MANIFEST" && echo "  Removed Fallout1-Realtime.txt."
echo
echo "Done. fallout.cfg and saved games were left as they are."
pause
exit 0
