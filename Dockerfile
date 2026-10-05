# Toolchain for building the rfp-usb firmware.
#
# The image carries the toolchain and nothing else; the source is mounted at
# run time. So the image is rebuilt when the toolchain changes rather than on
# every commit, and the same image builds any branch.
#
#   docker build -t rfp-usb-build .
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" rfp-usb-build \
#       tools/build-firmware.sh
#
# The .uf2 lands in build/ in the working tree, exactly as a local build does.
FROM debian:bookworm-slim

# Pinned, so a release can be rebuilt later and come out the same. Both track
# the same upstream version.
ARG PICO_SDK_REF=2.1.1
ARG PICOTOOL_REF=2.1.1

ENV DEBIAN_FRONTEND=noninteractive

# libusb and pkg-config are for building picotool, not for the firmware.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        gcc-arm-none-eabi \
        git \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        libusb-1.0-0-dev \
        pkg-config \
        python3 \
    && rm -rf /var/lib/apt/lists/*

ENV PICO_SDK_PATH=/opt/pico-sdk

# Only the TinyUSB submodule is needed; the rest of the SDK's are not, and
# cloning them all costs several hundred megabytes.
RUN git clone --depth 1 --branch "${PICO_SDK_REF}" \
        https://github.com/raspberrypi/pico-sdk.git "${PICO_SDK_PATH}" \
 && git -C "${PICO_SDK_PATH}" submodule update --init --depth 1 lib/tinyusb

# picotool is what turns the ELF into the .uf2. Built once here: if the SDK
# cannot find one it fetches and compiles its own copy on every firmware
# build, which works but costs minutes each time.
RUN git clone --depth 1 --branch "${PICOTOOL_REF}" \
        https://github.com/raspberrypi/picotool.git /tmp/picotool \
 && cmake -S /tmp/picotool -B /tmp/picotool/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DPICOTOOL_FLAT_INSTALL=1 \
        -DCMAKE_INSTALL_PREFIX=/opt \
 && cmake --build /tmp/picotool/build -j "$(nproc)" \
 && cmake --install /tmp/picotool/build \
 && rm -rf /tmp/picotool
ENV picotool_DIR=/opt/picotool

# The toolchain is cloned as root, but the build runs as whoever invoked it so
# that artefacts in the mounted tree are not left root-owned. Without this git
# refuses to look at a tree it does not own, and the SDK's CMake uses git.
RUN git config --system --add safe.directory '*'

WORKDIR /src
