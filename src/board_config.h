/*
 * board_config.h
 * 
 * Board configuration for murmapple - Apple IIe emulator for RP2350
 * Based on murmdoom board configuration
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "pico.h"
#include "hardware/structs/sysinfo.h"
#include "hardware/vreg.h"

/*
 * Board Configuration Variants:
 * 
 * BOARD_M1 - M1 GPIO layout
 * BOARD_M2 - M2 GPIO layout
 * 
 * PSRAM pin is auto-detected based on chip package:
 *   RP2350B: GPIO47 (for both M1 and M2)
 *   RP2350A: GPIO19 (M1) or GPIO8 (M2)
 * 
 * M1 GPIO Layout:
 *   HDMI: CLKN=6, CLKP=7, D0N=8, D0P=9, D1N=10, D1P=11, D2N=12, D2P=13
 *   SD:   CLK=2, CMD=3, DAT0=4, DAT3=5
 *   PS/2: CLK=0, DATA=1
 * 
 * M2 GPIO Layout:
 *   HDMI: CLKN=12, CLKP=13, D0N=14, D0P=15, D1N=16, D1P=17, D2N=18, D2P=19
 *   SD:   CLK=6, CMD=7, DAT0=4, DAT3=5
 *   PS/2: CLK=2, DATA=3
 */

// Default to M1 if no config specified
#if !defined(BOARD_M1) && !defined(BOARD_M2)
#define BOARD_M1
#endif

//=============================================================================
// CPU/PSRAM Speed Defaults (can be overridden via CMake)
//=============================================================================
#ifndef CPU_CLOCK_MHZ
#define CPU_CLOCK_MHZ 252
#endif

#ifndef CPU_VOLTAGE
#define CPU_VOLTAGE VREG_VOLTAGE_1_50
#endif

//=============================================================================
// PSRAM Configuration
//=============================================================================

// PSRAM pin for RP2350A variants
#ifdef BOARD_M1
#define PSRAM_PIN_RP2350A 19
#else
#define PSRAM_PIN_RP2350A 8
#endif

// PSRAM pin for RP2350B (always GPIO47)
#define PSRAM_PIN_RP2350B 47

// Runtime function to get PSRAM pin based on chip package
static inline uint get_psram_pin(void) {
#if PICO_RP2040
    return 0;
#endif
#if PICO_RP2350
    uint32_t package_sel = *((io_ro_32*)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET));
    if (package_sel & 1) {
        return PSRAM_PIN_RP2350A;
    } else {
        return PSRAM_PIN_RP2350B;
    }
#endif
}

//=============================================================================
// M1 Layout Configuration
//=============================================================================
#ifdef BOARD_M1

// HDMI Pins
#define HDMI_PIN_CLKN 6
#define HDMI_PIN_CLKP 7
#define HDMI_PIN_D0N  8
#define HDMI_PIN_D0P  9
#define HDMI_PIN_D1N  10
#define HDMI_PIN_D1P  11
#define HDMI_PIN_D2N  12
#define HDMI_PIN_D2P  13

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins
#define SDCARD_PIN_CLK    2
#define SDCARD_PIN_CMD    3
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

// PS/2 Keyboard Pins
#define PS2_PIN_CLK  0
#define PS2_PIN_DATA 1

// NES/SNES Gamepad Pins (directly after HDMI pins)
#define NESPAD_GPIO_CLK   14
#define NESPAD_GPIO_DATA  16
#define NESPAD_GPIO_LATCH 15

// I2S Audio Pins
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

#define BEEPER_PIN 28

#define PSRAM
#define PSRAM_SPINLOCK 1
#define PSRAM_ASYNC 1

#define PSRAM_PIN_CS 18
#define PSRAM_PIN_SCK 19
#define PSRAM_PIN_MOSI 20
#define PSRAM_PIN_MISO 21

#endif // BOARD_M1

//=============================================================================
// M2 Layout Configuration
//=============================================================================
#ifdef BOARD_M2

// HDMI Pins
#define HDMI_PIN_CLKN 12
#define HDMI_PIN_CLKP 13
#define HDMI_PIN_D0N  14
#define HDMI_PIN_D0P  15
#define HDMI_PIN_D1N  16
#define HDMI_PIN_D1P  17
#define HDMI_PIN_D2N  18
#define HDMI_PIN_D2P  19

#define HDMI_BASE_PIN HDMI_PIN_CLKN

// SD Card Pins
#define SDCARD_PIN_CLK    6
#define SDCARD_PIN_CMD    7
#define SDCARD_PIN_D0     4
#define SDCARD_PIN_D3     5

// PS/2 Keyboard Pins
#define PS2_PIN_CLK  2
#define PS2_PIN_DATA 3

// NES/SNES Gamepad Pins (using available GPIOs)
#define NESPAD_GPIO_CLK   20
#define NESPAD_GPIO_DATA  22
#define NESPAD_GPIO_LATCH 21

// I2S Audio Pins
#define I2S_DATA_PIN       9
#define I2S_CLOCK_PIN_BASE 10

#define BEEPER_PIN 9

//#define PSRAM
#define PSRAM_SPINLOCK 1
#define PSRAM_ASYNC 1

#define PSRAM_PIN_CS 8
#define PSRAM_PIN_SCK 6
#define PSRAM_PIN_MOSI 7
#define PSRAM_PIN_MISO 4

#endif // BOARD_M2

//=============================================================================
// Apple IIe Display Configuration
//=============================================================================

// Apple II native resolution is 280x192 (HiRes) or 560x192 (DHR)
// We'll scale to fit 640x480 HDMI output
#define APPLE2_HIRES_WIDTH   280
#define APPLE2_HIRES_HEIGHT  192
#define APPLE2_DHR_WIDTH     560
#define APPLE2_DHR_HEIGHT    192

// HDMI output resolution
#define HDMI_WIDTH  640
#define HDMI_HEIGHT 480

// Apple II framebuffer (8-bit indexed color)
#define APPLE2_FB_WIDTH  560
#define APPLE2_FB_HEIGHT 384  // 192 * 2 for scanline doubling

#endif // BOARD_CONFIG_H
