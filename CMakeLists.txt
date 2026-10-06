cmake_minimum_required(VERSION 3.13)

# Which GEEK to build for. The two boards are pin-for-pin identical -- see the
# board section of src/config.h -- so naming the board header is almost all it
# takes: the SDK reads the chip, the flash size and the UART pinout out of it.
#
#   cmake -B build/2040 -DPICO_BOARD=waveshare_rp2040_geek
#   cmake -B build/2350 -DPICO_BOARD=waveshare_rp2350_geek
#
# tools/build-firmware.sh does both. Note that PICO_BOARD has to be settled
# before the SDK is imported, because importing it is what picks the platform
# and therefore the compiler flags.
if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD waveshare_rp2040_geek CACHE STRING "Target board")
endif()

# A short suffix for the artefacts. The two .uf2 files are not interchangeable
# and nothing but the filename tells them apart once they are downloaded, so
# the chip goes in the name rather than in the directory it was built in.
if (PICO_BOARD STREQUAL "waveshare_rp2040_geek")
    set(RFP_VARIANT 2040)
elseif (PICO_BOARD STREQUAL "waveshare_rp2350_geek")
    set(RFP_VARIANT 2350)
else()
    message(FATAL_ERROR
        "PICO_BOARD=${PICO_BOARD} is not a board this firmware knows. Use "
        "waveshare_rp2040_geek or waveshare_rp2350_geek: the pin assignments "
        "in src/config.h are specific to them.")
endif()

# What the build calls itself, for the mode menu and for `picotool info`.
#
# A release passes the tag it is cutting, so the string on the screen is the
# same string as the one in the artefact's filename rather than a second guess
# at it. Anything else asks git, and falls back to "dev" where there is no git
# to ask -- a bare tarball, say.
#
# Settled at configure time, so a build that is rebuilt without reconfiguring
# keeps the string it was configured with.
#
# The nearest tag, not everything `git describe` would say: the commit count
# and hash it appends are longer than the screen can hold, and what is worth
# knowing on a 12-character readout is which release this build is near, not
# how far past it. Anything that is not exactly that tag with a clean tree gets
# a trailing +, so a development build is never read as the release itself.
if (NOT DEFINED RFP_VERSION)
    execute_process(
        COMMAND git describe --tags --abbrev=0
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE RFP_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    execute_process(
        COMMAND git describe --tags --exact-match --dirty=+
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
        OUTPUT_VARIABLE RFP_VERSION_EXACT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if (NOT RFP_VERSION_EXACT STREQUAL RFP_VERSION)
        set(RFP_VERSION "${RFP_VERSION}+")
    endif()
endif()

# No tags to describe, or no git at all -- a bare tarball, say.
if (NOT RFP_VERSION)
    set(RFP_VERSION "dev")
endif()
message(STATUS "Firmware version: ${RFP_VERSION}")

include(pico_sdk_import.cmake)

project(rfp_usb C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# SD card block driver plus FatFs. FatFs is used only at startup, to create
# the marker file on the card; everything the host sees goes through
# src/blockdev.c instead.
add_subdirectory(lib/sd_fatfs/src sd_fatfs)

add_executable(rfp-usb
    src/main.c
    src/blockdev.c
    src/fat32lite.c
    src/modes.c
    src/msc_disk.c
    src/overlay.c
    src/ramdisk.c
    src/nvstate.c
    src/sdcard.c
    src/st7789.c
    src/ui.c
    src/usb_descriptors.c
)

target_include_directories(rfp-usb PRIVATE src)

# src/config.h carries a "dev" fallback for the host harnesses, which compile
# the same sources without a build system; this is what a real build uses.
target_compile_definitions(rfp-usb PRIVATE RFP_VERSION="${RFP_VERSION}")

# Puts the same string in the binary's info block, so `picotool info` on a
# board reports which build is actually on it -- the screen is the operator's
# answer to that question, this is the one you can ask over USB.
pico_set_program_version(rfp-usb "${RFP_VERSION}")

target_link_libraries(rfp-usb
    pico_stdlib
    pico_unique_id
    hardware_spi
    hardware_flash
    hardware_sync
    hardware_watchdog
    pico_flash
    tinyusb_device
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# stdio must not go over USB: the USB device is ours, and must stay silent
# until the boot window has closed. Both GEEKs bring UART1 out on the 3-pin
# connector (GP4/GP5), and both board headers already make that the default
# UART, so there is nothing to override here.
pico_enable_stdio_usb(rfp-usb 0)
pico_enable_stdio_uart(rfp-usb 1)

# rfp-usb-2040.uf2 / rfp-usb-2350.uf2. pico_add_uf2_output honours OUTPUT_NAME,
# so this has to be set before the outputs are added.
set_target_properties(rfp-usb PROPERTIES OUTPUT_NAME rfp-usb-${RFP_VARIANT})

pico_add_extra_outputs(rfp-usb)
