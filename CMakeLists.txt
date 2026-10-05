cmake_minimum_required(VERSION 3.13)

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

# The GEEK brings UART1 out on the 3-pin connector (GP4/GP5). stdio must not
# go over USB: the USB device is ours, and must stay silent until the boot
# window has closed.
target_compile_definitions(rfp-usb PRIVATE
    PICO_DEFAULT_UART=1
    PICO_DEFAULT_UART_TX_PIN=4
    PICO_DEFAULT_UART_RX_PIN=5
)

pico_enable_stdio_usb(rfp-usb 0)
pico_enable_stdio_uart(rfp-usb 1)

pico_add_extra_outputs(rfp-usb)
