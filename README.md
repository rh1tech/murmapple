# MurmApple

Apple IIe Emulator for RP2040/RP2350 (Raspberry Pi Pico, Pico 2 or similar) with HDMI output, SD card, PS/2 and USB keyboard, and audio.

## Supported Boards

This firmware is designed for the following RP2350-based boards with integrated HDMI, SD card and PS/2 or USB HID:

- **[Murmulator](https://murmulator.ru)** — A compact retro-computing platform based on RP Pico / Pico 2, designed for emulators and classic games.
- **[FRANK](https://rh1.tech/projects/frank?area=about)** — A versatile development board based on RP Pico 2, HDMI output, and extensive I/O options.

Both boards provide all necessary peripherals out of the box—no additional wiring required.

## Features

- Full Apple IIe emulation with 65C02 CPU
- Native 320×240 video output via PIO (HDMI or VGA)
- All video modes: Text, Lo-Res, Hi-Res, Double Hi-Res
- SD card support for DSK, NIB, WOZ, and BDSK disk images
- Disk write-back support (saves changes to .bdsk files)
- PS/2 keyboard input
- USB keyboard input (via native USB Host)
- NES/USB gamepad support (via USB HID)
- USB hub support for multiple devices
- Audio output via I2S (external DAC) or PWM

## Hardware Requirements

- **Raspberry Pi Pico** (RP2040), or **Raspberry Pi Pico 2** (RP2350), or compatible board
- **Video output** (choose one):
  - **HDMI connector** (directly connected via resistors, no HDMI encoder needed)
  - **VGA connector** (accent resistor DAC)
- **SD card module** (SPI mode)
- **PS/2 keyboard** (directly connected) — OR —
- **USB keyboard** (via native USB port, hub supported)
- **Audio output** (choose one):
  - **I2S DAC module** (e.g., TDA1387 or PCM5102) — recommended for best quality
  - **PWM audio** — no additional hardware needed, directly to speaker/amplifier

> **Note:** When USB HID is enabled, the native USB port is used for keyboard input. USB serial console (CDC) is disabled in this mode; use UART for debug output.

## Board Configurations

Two GPIO layouts are supported: **M1** and **M2**.

### HDMI (via 270Ω resistors)
| Signal | M1 GPIO | M2 GPIO |
|--------|---------|---------|
| CLK-   | 6       | 12      |
| CLK+   | 7       | 13      |
| D0-    | 8       | 14      |
| D0+    | 9       | 15      |
| D1-    | 10      | 16      |
| D1+    | 11      | 17      |
| D2-    | 12      | 18      |
| D2+    | 13      | 19      |

### SD Card (SPI mode)
| Signal  | M1 GPIO | M2 GPIO |
|---------|---------|---------|
| CLK     | 2       | 6       |
| CMD     | 3       | 7       |
| DAT0    | 4       | 4       |
| DAT3/CS | 5       | 5       |

### PS/2 Keyboard
| Signal | M1 GPIO | M2 GPIO |
|--------|---------|---------|
| CLK    | 0       | 2       |
| DATA   | 1       | 3       |

### I2S Audio
| Signal | M1 GPIO | M2 GPIO |
|--------|---------|---------|
| DATA   | 26      | 9       |
| BCLK   | 27      | 10      |
| LRCLK  | 28      | 11      |

### VGA (accent resistor DAC)
| Signal | M1 GPIO | M2 GPIO |
|--------|---------|---------|
| HSYNC  | 6       | 12      |
| VSYNC  | 7       | 13      |
| R0     | 8       | 14      |
| R1     | 9       | 15      |
| G0     | 10      | 16      |
| G1     | 11      | 17      |
| B0     | 12      | 18      |
| B1     | 13      | 19      |

> **Note:** VGA uses the same GPIO pins as HDMI. Choose the appropriate firmware based on your video output hardware.

## Firmware Versions

MurmApple is available in multiple configurations based on your hardware setup:

### Video Output Options

| Option | Description |
|--------|-------------|
| **HDMI** | Digital video via PIO-driven HDMI (no encoder chip needed) |
| **VGA** | Analog video via resistor DAC VGA output |

### Audio Output Options

| Option | Description |
|--------|-------------|
| **I2S** | High-quality audio via external DAC (TDA1387, PCM5102, etc.) |
| **PWM** | Direct PWM audio output (no additional hardware needed) |

### Platform Variants

| Platform | PSRAM | Description |
|----------|-------|-------------|
| **RP2350 + PSRAM** | Yes | Full functionality, recommended configuration |
| **RP2350 no-PSRAM** | No | For boards without external PSRAM |
| **RP2040** | No | Limited support, reduced functionality due to memory constraints |

### Murmulator OS (MOS2)

For use with Murmulator OS, `.m1p2` and `.m2p2` firmware files are provided. These use a special linker script and are only available for RP2350 with PSRAM.

### Firmware File Naming

Files are named: `murmapple_<board>_<video>_<audio>_<memory>_<version>.<ext>`

Examples:
- `murmapple_m1_hdmi_i2s_psram_1_00.uf2` — M1 board, HDMI video, I2S audio, with PSRAM
- `murmapple_m2_vga_pwm_nopsram_1_00.uf2` — M2 board, VGA video, PWM audio, no PSRAM
- `murmapple_m1_hdmi_i2s_rp2040_1_00.uf2` — M1 board, HDMI video, I2S audio, RP2040
- `murmapple_m1_hdmi_i2s_psram_1_00.m1p2` — M1 board, MOS2 format

## Building

### Prerequisites

1. Install the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) (version 2.0+)
2. Set environment variable: `export PICO_SDK_PATH=/path/to/pico-sdk`
3. Install ARM GCC toolchain

### Build Steps

```bash
# Clone the repository with submodules
git clone --recursive https://github.com/user/murmapple.git
cd murmapple

# Or if already cloned, initialize submodules
git submodule update --init --recursive

# Build for M1 layout with PS/2 input (default)
mkdir build && cd build
cmake -DBOARD_VARIANT=M1 ..
make -j$(nproc)

# Build for M2 layout with PS/2 input
cmake -DBOARD_VARIANT=M2 ..
make -j$(nproc)

# Build with USB keyboard support (instead of PS/2)
cmake -DBOARD_VARIANT=M1 -DPS2_KEYBOARD_ENABLED=OFF ..
make -j$(nproc)
```

### Build Options (CMake)

| Option | Description |
|--------|-------------|
| `-DPICO_BOARD=pico2` | Build for RP2350 (default) |
| `-DPICO_BOARD=pico` | Build for RP2040 (limited support) |
| `-DBOARD_VARIANT=M1` | Use M1 GPIO layout (default) |
| `-DBOARD_VARIANT=M2` | Use M2 GPIO layout |
| `-DVIDEO_TYPE=HDMI` | Video output: HDMI (default) or VGA |
| `-DAUDIO_TYPE=I2S` | Audio output: I2S (default) or PWM |
| `-DCPU_SPEED=252` | CPU clock in MHz (125, 252, 378, 504) |
| `-DPSRAM_SPEED=100` | PSRAM clock in MHz (100, 133, 166) - omit for no PSRAM |
| `-DMOS2=ON` | Build for Murmulator OS (m1p2/m2p2 format) |
| `-DUSB_HID_ENABLED=OFF` | Enable USB keyboard (disables USB serial) |
| `-DPS2_KEYBOARD_ENABLED=ON` | Enable PS/2 keyboard input |
| `-DDEBUG_LOGS_ENABLED=ON` | Enable verbose debug logging |

### Build Script (build.sh)

The easiest way to build is using the build script:

```bash
# Default build: M1, HDMI, I2S, with PSRAM, 252 MHz
./build.sh

# Custom configurations
./build.sh --board M2 --video VGA --audio PWM
./build.sh --nopsram                    # Build without PSRAM
./build.sh --rp2040                     # Build for RP2040
./build.sh --mos2                       # Build for Murmulator OS
./build.sh --cpu 378                    # Overclock to 378 MHz
```

**build.sh options:**

| Option | Description |
|--------|-------------|
| `-b, --board <M1\|M2>` | Board variant (default: M1) |
| `-v, --video <HDMI\|VGA>` | Video output (default: HDMI) |
| `-a, --audio <I2S\|PWM>` | Audio output (default: I2S) |
| `-p, --psram <MHz>` | PSRAM speed in MHz (default: 100) |
| `--nopsram` | Build without PSRAM support |
| `-c, --cpu <MHz>` | CPU speed: 252, 378, 504 (default: 252) |
| `--mos2` | Build for Murmulator OS |
| `--rp2040` | Build for RP2040 instead of RP2350 |
| `-h, --help` | Show help |

### Release Builds

To build all 32 firmware variants with version numbering:

```bash
./release.sh
```

This creates versioned firmware files in the `release/` directory, packaged as ZIP archives:

| Archive | Contents |
|---------|----------|
| `murmapple_m1_psram_X_XX.zip` | M1 board, RP2350 with PSRAM (4 variants: HDMI/VGA × I2S/PWM) |
| `murmapple_m2_psram_X_XX.zip` | M2 board, RP2350 with PSRAM (4 variants) |
| `murmapple_m1_nopsram_X_XX.zip` | M1 board, RP2350 without PSRAM (4 variants) |
| `murmapple_m2_nopsram_X_XX.zip` | M2 board, RP2350 without PSRAM (4 variants) |
| `murmapple_m1_rp2040_X_XX.zip` | M1 board, RP2040 (4 variants) |
| `murmapple_m2_rp2040_X_XX.zip` | M2 board, RP2040 (4 variants) |
| `murmapple_mos2_X_XX.zip` | Murmulator OS builds (.m1p2/.m2p2, 8 variants) |

Each archive contains firmware for all video/audio combinations:
- `*_hdmi_i2s_*.uf2` — HDMI video, I2S audio
- `*_hdmi_pwm_*.uf2` — HDMI video, PWM audio
- `*_vga_i2s_*.uf2` — VGA video, I2S audio
- `*_vga_pwm_*.uf2` — VGA video, PWM audio

All release builds use 252 MHz CPU clock (no overclocking) for maximum stability.

### Flashing

```bash
# With device in BOOTSEL mode:
picotool load build/murmapple.uf2

# Or with device running:
picotool load -f build/murmapple.uf2

# Or use the flash script:
./flash.sh
```

## SD Card Setup

1. Format an SD card as FAT32
2. Copy Apple II disk images to the "apple" directory (`.dsk`, `.nib`, or `.woz` files)
3. Use the on-screen disk UI to select and load disk images

### Supported Disk Formats

- **DSK** — Standard 140KB sector-based disk images
- **NIB** — Nibble-based disk images (140KB)
- **WOZ** — Flux-accurate disk images (WOZ v1 and v2)
- **BDSK** — MurmApple write-back format (created automatically when saving changes)

> **Note:** When you modify a disk (e.g., save a game), changes are written to a `.bdsk` file with the same name, preserving the original disk image.

## Controls

### Keyboard
- Ctrl+Alt+Delete: Warm reset
- Open Apple (Left Alt/Left Windows): Left paddle button
- Closed Apple (Right Alt/Right Windows): Right paddle button
- F9: show CPU spped (kHz + %%)
- F11: open Disk UI
- F12: unlock CPU speed (while pressed, for short fastups)
- ScrLock: unlock CPU speed toggle

### Keyboard to joystick mapping
- KP 6 → RIGHT
- KP 4 → LEFT
- KP 2 & 5 → DOWN
- KP 8 → UP
- Any Control → A
- Any Alt → B
- Ins → START
- Del → SELECT

### Gamepad (NES/USB)
- A Button: Left paddle button (Open Apple)
- B Button: Right paddle button (Closed Apple)
- Start+A+B: Reset

## License

MIT License. See [LICENSE](LICENSE) for details.

## Acknowledgments

This project is based on the following open-source projects:

### MII Emulator
- **Project:** [MII Apple //e Emulator](https://github.com/buserror/mii_emu)
- **Author:** Michel Pollet <buserror+git@gmail.com>
- **License:** MIT License
- **Description:** The core Apple IIe emulation engine, including 65C02 CPU, memory banking, video rendering, disk drive emulation (DSK, NIB, WOZ), and slot-based peripheral architecture.

### Apple2TS
- **Project:** [Apple2TS - Apple II Emulator](https://github.com/ct6502/apple2ts)
- **Authors:** Chris Torrence and Michael Morrison
- **License:** CC BY-SA 4.0 (Creative Commons Attribution-ShareAlike)
- **Description:** Reference for Apple II implementation details and emulation techniques.

### Pico-Extras
- **Project:** [pico-extras](https://github.com/raspberrypi/pico-extras)
- **Author:** Raspberry Pi (Trading) Ltd.
- **License:** BSD 3-Clause
- **Description:** Audio I2S library for sound output.

### FatFs
- **Project:** [FatFs](http://elm-chan.org/fsw/ff/)
- **Author:** ChaN
- **License:** FatFs License (BSD-style)
- **Description:** Generic FAT filesystem module for SD card access.

## Authors & Contributors

**Mikhail Matveev** <<xtreme@rh1.tech>>
- Original MurmApple port and core development
- Website: [https://rh1.tech](https://rh1.tech)

**DnCraptor** ([GitHub](https://github.com/DnCraptor))
- Murmulator OS (MOS2) integration
- RP2040 platform support
- VGA video output driver
- No-PSRAM mode for boards without external PSRAM
- Disk write-back support (.bdsk format)
- Disk UI enhancements
- Various bug fixes and improvements
