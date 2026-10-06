#!/usr/bin/env bash
#
# Builds the firmware, for both GEEKs by default. The same script runs on a
# developer's machine and in the release workflow, so what CI does is what you
# can reproduce by hand.
#
#   PICO_SDK_PATH=~/toolchains/pico-sdk tools/build-firmware.sh
#   PICO_SDK_PATH=~/toolchains/pico-sdk tools/build-firmware.sh 2350
#
#   docker build -t rfp-usb-build .
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" rfp-usb-build \
#       tools/build-firmware.sh
#
# Each variant gets its own directory under build/, because the two have
# different compilers and a CMake cache cannot be reconfigured from one to the
# other. The .uf2 files are copied up to build/ so there is one predictable
# path per variant:
#
#   build/rfp-usb-2040.uf2
#   build/rfp-usb-2350.uf2
#
# BUILD_DIR overrides where those directories live, which is how to keep a
# container build from colliding with a host one: a CMake cache remembers the
# absolute paths it was configured with, and the two disagree about where the
# SDK lives.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${BUILD_DIR:-$root/build}"

board_for() {
    case "$1" in
        2040) echo waveshare_rp2040_geek ;;
        2350) echo waveshare_rp2350_geek ;;
        *)    return 1 ;;
    esac
}

variants=("$@")
if [ ${#variants[@]} -eq 0 ]; then
    variants=(2040 2350)
fi

for variant in "${variants[@]}"; do
    if ! board_for "$variant" >/dev/null; then
        echo "unknown variant '$variant' -- expected 2040 or 2350" >&2
        exit 2
    fi
done

if [ -z "${PICO_SDK_PATH:-}" ]; then
    echo "PICO_SDK_PATH is not set -- set it, or run this inside the" >&2
    echo "toolchain image, which sets it for you." >&2
    exit 1
fi

# The SD driver is vendored under lib/, but a tree where it is missing gives a
# CMake error that is not obviously about that.
if [ ! -f "$root/lib/sd_fatfs/src/CMakeLists.txt" ]; then
    echo "lib/sd_fatfs is empty. Run:" >&2
    echo "    git submodule update --init --recursive" >&2
    exit 1
fi

for variant in "${variants[@]}"; do
    board="$(board_for "$variant")"
    dir="$build/$variant"

    echo
    echo "== $board =="

    # A cache configured against a different source tree, SDK or board cannot
    # be reused, and the failure is confusing. Start again instead.
    if [ -f "$dir/CMakeCache.txt" ] &&
       ! { grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$root" "$dir/CMakeCache.txt" &&
           grep -qx "PICO_BOARD:STRING=$board" "$dir/CMakeCache.txt"; }; then
        echo "note: $dir was configured differently; reconfiguring"
        rm -rf "$dir"
    fi

    cmake -S "$root" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPICO_BOARD="$board"
    cmake --build "$dir" -j "$(nproc)"

    cp "$dir/rfp-usb-$variant.uf2" "$build/rfp-usb-$variant.uf2"
done

echo
for variant in "${variants[@]}"; do
    ls -l "$build/rfp-usb-$variant.uf2"
done
