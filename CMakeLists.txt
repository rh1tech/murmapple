# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# Set board to Pico 2 (RP2350)
set(PICO_BOARD pico2 CACHE STRING "Pico board type")

# Board configuration variant (M1, M2)
set(BOARD_VARIANT "M1" CACHE STRING "Board variant: M1 or M2")

# CPU overclock configuration (252, 378, 504 MHz)
set(CPU_SPEED "252" CACHE STRING "CPU clock in MHz: 252, 378, 504")

# PSRAM speed configuration (84, 100, 133, 166 MHz target)
#set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 166")

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

# I2S Audio pin configuration based on board variant
if(BOARD_VARIANT STREQUAL "M1")
    set(I2S_DATA_PIN "26")
    set(I2S_CLOCK_PIN_BASE "27")
else()  # M2
    set(I2S_DATA_PIN "9")
    set(I2S_CLOCK_PIN_BASE "10")
endif()

# USB HID Host support (keyboard and gamepad via USB)
# When enabled, USB CDC stdio is disabled!
option(USB_HID_ENABLED "Enable USB HID Host for keyboard/gamepad" ON)

# PS/2 keyboard support toggle
option(PS2_KEYBOARD_ENABLED "Enable PS/2 keyboard input" ON)

# Verbose debug logging toggle
option(DEBUG_LOGS_ENABLED "Enable verbose debug logging" OFF)

message(STATUS "murmapple - Apple IIe Emulator for RP2350")
if (PSRAM_SPEED)
    message(STATUS "Board: ${BOARD_VARIANT}, CPU: ${CPU_SPEED} MHz, PSRAM: ${PSRAM_SPEED} MHz, Voltage: ${CPU_VOLTAGE}")
else ()
    message(STATUS "Board: ${BOARD_VARIANT}, CPU: ${CPU_SPEED} MHz, Voltage: ${CPU_VOLTAGE}")
endif ()
message(STATUS "I2S Audio: DATA=${I2S_DATA_PIN}, CLK_BASE=${I2S_CLOCK_PIN_BASE}")

include(pico_sdk_import.cmake)

# Import pico-extras for audio_i2s library
set(PICO_EXTRAS_PATH ${CMAKE_CURRENT_LIST_DIR}/pico-extras CACHE PATH "Path to pico-extras")
if(EXISTS ${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)
    include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)
endif()

project(murmapple C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Initialize pico-extras if available
if(COMMAND pico_extras_init)
    pico_extras_init()
endif()

# Copy driver subdirectories from murmdoom structure
add_subdirectory(drivers/ps2kbd)
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/usbhid)

# Create a library for drivers
if (PSRAM_SPEED)
    add_library(drivers
        drivers/psram_init.c
        drivers/psram_allocator.c
    )
    target_compile_definitions(drivers PRIVATE
        PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    )
endif()

add_library(drivers
    drivers/HDMI.c
    drivers/nespad/nespad.c
)
target_include_directories(drivers PUBLIC drivers drivers/nespad src)
target_compile_definitions(drivers PRIVATE
    BOARD_${BOARD_VARIANT}
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
    src/mii_startscreen.c
    src/mii_analog.c
    # Disk drive support
    src/mii_slot.c
    src/mii_dd_stub.c
    src/mii_floppy.c
    src/mii_disk2.c
    src/mii_disk2_lss_asm.S
    src/mii_dsk.c
    src/mii_nib.c
    src/mii_woz.c
    src/mii_vcd_stub.c
    src/mii_rom_disk2_p5.c
    # Audio support
    src/mii_audio_i2s.c
    # Note: mii_speaker.c is excluded - stub provided in main.c for RP2350
    src/mockingboard.c
)

target_include_directories(murmapple PRIVATE
    src
    drivers
)

target_compile_definitions(murmapple PRIVATE
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    MII_65C02_DIRECT_ACCESS=1
    MII_RP2350=1
    # I2S Audio configuration
    # NOTE: HDMI uses PIO1, PS/2 uses PIO0 SM0 (claimed dynamically)
    # So I2S uses PIO0 SM2 (claimed statically before PS/2 init)
    PICO_AUDIO_I2S_PIO=0
    PICO_AUDIO_I2S_STATE_MACHINE=2
    PICO_AUDIO_I2S_DMA_IRQ=1
    PICO_AUDIO_I2S_DMA_CHANNEL=6
    PICO_AUDIO_I2S_DATA_PIN=${I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${I2S_CLOCK_PIN_BASE}
    FEATURE_AUDIO=1
)

if(PS2_KEYBOARD_ENABLED)
    target_compile_definitions(murmapple PRIVATE ENABLE_PS2_KEYBOARD=1)
else()
    target_compile_definitions(murmapple PRIVATE ENABLE_PS2_KEYBOARD=0)
endif()

if(DEBUG_LOGS_ENABLED)
    target_compile_definitions(murmapple PRIVATE ENABLE_DEBUG_LOGS=1)
else()
    target_compile_definitions(murmapple PRIVATE ENABLE_DEBUG_LOGS=0)
endif()

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
    pico_audio_i2s
    hardware_dma
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_spi
    drivers
    ps2kbd
    sdcard
    fatfs
    usbhid
)

# USB CDC stdio: enabled for debugging, but disabled when USB HID is enabled
# (USB port can only be Host OR Device, not both)
if(USB_HID_ENABLED)
    pico_enable_stdio_usb(murmapple 0)
    message(STATUS "USB CDC stdio disabled (USB HID Host is enabled)")
else()
    pico_enable_stdio_usb(murmapple 1)
endif()
pico_enable_stdio_uart(murmapple 1)

# Create UF2 output
pico_add_extra_outputs(murmapple)

target_link_options(murmapple PRIVATE -Xlinker --print-memory-usage --data-sections)
