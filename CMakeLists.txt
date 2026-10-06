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
