#!/usr/bin/env bash
#
# Builds the firmware. The same script runs on a developer's machine and in the
# release workflow, so what CI does is what you can reproduce by hand.
#
#   PICO_SDK_PATH=~/toolchains/pico-sdk tools/build-firmware.sh
#
#   docker build -t rfp-usb-build .
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" rfp-usb-build \
#       tools/build-firmware.sh
#
# BUILD_DIR overrides where it builds, which is how to keep a container build
# from colliding with a host one: a CMake cache remembers the absolute paths it
# was configured with, and the two disagree about where the SDK lives.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${BUILD_DIR:-$root/build}"

if [ -z "${PICO_SDK_PATH:-}" ]; then
    echo "PICO_SDK_PATH is not set -- set it, or run this inside the" >&2
    echo "toolchain image, which sets it for you." >&2
    exit 1
fi

# The submodule is easy to forget on a fresh clone, and the error CMake gives
# for a missing add_subdirectory is not obviously about that.
if [ ! -f "$root/lib/sd_fatfs/src/CMakeLists.txt" ]; then
    echo "lib/sd_fatfs is empty. Run:" >&2
    echo "    git submodule update --init --recursive" >&2
    exit 1
fi

# A cache configured against a different source tree or SDK cannot be reused,
# and the failure is confusing. Start again instead.
if [ -f "$build/CMakeCache.txt" ] &&
   ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$root" "$build/CMakeCache.txt"; then
    echo "note: $build was configured for another tree; reconfiguring"
    rm -rf "$build"
fi

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j "$(nproc)"

echo
ls -l "$build/rfp-usb.uf2"
