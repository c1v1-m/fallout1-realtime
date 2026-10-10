#!/usr/bin/env bash
# Build fallout-ce on Linux, including Steam Deck / SteamOS.
#
# SteamOS keeps / read-only and wipes pacman's work on every OS update, so this script
# refuses to install anything there and points at a distrobox container instead -- that
# is the only way a toolchain survives a SteamOS update. Everywhere else it just checks
# that the dependencies are present and builds.
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build-linux}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

in_container() { [ -f /run/.containerenv ] || [ -f /.dockerenv ] || [ -n "${CONTAINER_ID:-}" ]; }
is_steamos()   { grep -qi '^ID=steamos' /etc/os-release 2>/dev/null; }

if is_steamos && ! in_container; then
    cat <<'MSG'
This is SteamOS with a read-only root. Build inside a container instead -- one command,
and it survives SteamOS updates:

    distrobox create --name fo --image archlinux:latest
    distrobox enter fo
    sudo pacman -Syu --needed base-devel cmake sdl2 zlib git
    cd ~/fallout1-ce && ./build-linux.sh

The container shares your home, the GPU and /dev, so the built binary runs from inside it
with graphics and the Deck's own controls working. If you would rather not use a container,
`sudo steamos-readonly disable` works too -- but every SteamOS update undoes it.
MSG
    exit 2
fi

missing=()
command -v cmake >/dev/null || missing+=("cmake")
command -v c++   >/dev/null || missing+=("a C++ compiler (base-devel / build-essential)")
if command -v pkg-config >/dev/null; then
    pkg-config --exists sdl2 || missing+=("SDL2 development files (sdl2 / libsdl2-dev)")
    pkg-config --exists zlib || missing+=("zlib development files (zlib / zlib1g-dev)")
fi

if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing, install these first:" >&2
    printf '  - %s\n' "${missing[@]}" >&2
    echo >&2
    echo "  Arch/SteamOS container:  sudo pacman -S --needed base-devel cmake sdl2 zlib" >&2
    echo "  Debian/Ubuntu:           sudo apt install build-essential cmake libsdl2-dev zlib1g-dev" >&2
    exit 1
fi

echo "==> Configuring in $BUILD_DIR (first run downloads adecode + fpattern, needs network)"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "==> Building with $JOBS jobs"
cmake --build "$BUILD_DIR" -j"$JOBS"

BIN="$BUILD_DIR/fallout-ce"
echo
echo "==> Built: $BIN"
if command -v ldd >/dev/null; then
    if ldd "$BIN" | grep -q 'not found'; then
        echo "!!! Unresolved libraries -- this binary will not start here:" >&2
        ldd "$BIN" | grep 'not found' >&2
    else
        echo "    All shared libraries resolve."
    fi
fi
cat <<'MSG'

To play, put the binary next to your Fallout data (the folder holding MASTER.DAT,
CRITTER.DAT and the DATA directory) and run it from there:

    cp build-linux/fallout-ce ~/Fallout/ && cd ~/Fallout && ./fallout-ce

Case matters on Linux: the data files are usually uppercase, and fallout.cfg lives beside
the binary. Copy your Windows fallout.cfg over to keep the combat tuning identical.
MSG
