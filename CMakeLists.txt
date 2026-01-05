cmake_minimum_required(VERSION 3.13)

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board configuration variant (M1, M2)
set(BOARD_VARIANT "M1" CACHE STRING "Board variant: M1 or M2")

# CPU overclock configuration (252, 378, 504 MHz)
set(CPU_SPEED "252" CACHE STRING "CPU clock in MHz: 252, 378, 504")

# PSRAM speed configuration (84, 100, 133, 166 MHz target)
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 166")

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "murmapple - Apple IIe Emulator for RP2350")
message(STATUS "Board: ${BOARD_VARIANT}, CPU: ${CPU_SPEED} MHz, PSRAM: ${PSRAM_SPEED} MHz, Voltage: ${CPU_VOLTAGE}")

include(pico_sdk_import.cmake)

project(murmapple C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Copy driver subdirectories from murmdoom structure
add_subdirectory(drivers/ps2kbd)
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)

# Create a library for drivers
add_library(drivers
    drivers/HDMI.c
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/nespad/nespad.c
)
target_include_directories(drivers PUBLIC drivers drivers/nespad src)
target_compile_definitions(drivers PRIVATE
    BOARD_${BOARD_VARIANT}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
target_link_libraries(drivers pico_stdlib hardware_dma hardware_pio hardware_spi)

# Apple IIe Emulator sources
add_executable(murmapple
    src/main.c
    src/mii_65c02.c
    src/mii_bank.c
    src/mii_video.c
    src/mii_rom.c
    src/mii.c
    src/mii_rom_iiee.c
    src/mii_rom_iiee_video.c
    src/disk_loader.c
    src/disk_ui.c
    src/mii_analog.c
    # Disk drive support
    src/mii_slot.c
    src/mii_dd_stub.c
    src/mii_floppy.c
    src/mii_disk2.c
    src/mii_dsk.c
    src/mii_nib.c
    src/mii_woz.c
    src/mii_vcd_stub.c
    src/mii_rom_disk2_p5.c
)

target_include_directories(murmapple PRIVATE
    src
    drivers
)

target_compile_definitions(murmapple PRIVATE
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    MII_65C02_DIRECT_ACCESS=1
    MII_RP2350=1
)

# Optimization for maximum performance on RP2350
# -O3: Maximum optimization including loop vectorization
# -ffunction-sections -fdata-sections: Allow linker to remove unused code
# -flto: Link-time optimization for cross-file inlining
# -fno-strict-aliasing: Allow type punning (common in emulators)
# -march=armv8-m.main+fp+dsp: Target Cortex-M33 architecture
target_compile_options(murmapple PRIVATE 
    -O3 
    -ffunction-sections 
    -fdata-sections
    -fno-strict-aliasing
)

target_link_libraries(murmapple
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_spi
    drivers
    ps2kbd
    sdcard
    fatfs
)

# Enable USB serial output for debugging
pico_enable_stdio_usb(murmapple 1)
pico_enable_stdio_uart(murmapple 0)

# Create UF2 output
pico_add_extra_outputs(murmapple)
