# MurmApple

Apple IIe Emulator for RP2040/RP2350 (Raspberry Pi Pico, Pico 2 or similar) with HDMI output, SD card, PS/2 and USB keyboard, and audio.

## Supported Boards

This firmware is designed for the following RP2350-based boards with integrated HDMI, SD card and PS/2 or USB HID:

- **[Murmulator](https://murmulator.ru)** — A compact retro-computing platform based on RP Pico / Pico 2, designed for emulators and classic games.
- **[FRANK](https://rh1.tech/projects/frank?area=about)** — A versatile development board based on RP Pico 2, HDMI output, and extensive I/O options.

Both boards provide all necessary peripherals out of the box—no additional wiring required.

## Features

- Full Apple IIe emulation with 65C02 CPU
- Native 320×240 HDMI video output via PIO
- All video modes: Text, Lo-Res, Hi-Res, Double Hi-Res
- SD card support for DSK, NIB, and WOZ disk images
- PS/2 keyboard input
- USB keyboard input (via native USB Host)
- NES/USB gamepad support (via USB HID)
- USB hub support for multiple devices
- I2S audio output for speaker and Mockingboard sound

## Hardware Requirements

- **Raspberry Pi Pico** (RP2040), or **Raspberry Pi Pico 2** (RP2350), or compatible board
- **HDMI connector** (directly connected via resistors, no HDMI encoder needed)
- **SD card module** (SPI mode)
- **PS/2 keyboard** (directly connected) — OR —
- **USB keyboard** (via native USB port, hub supported)
- **I2S DAC module** (e.g., TDA1387 or PCM5102) for audio output

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

## Firmware Versions

MurmApple is available in three speed configurations:

| Speed | CPU Clock | Description |
|-------|-----------|-------------|
| Stock | 252 MHz   | Default, stable operation |
| Medium OC | 378 MHz | Moderate overclock, improved performance |
| Max OC | 504 MHz  | Maximum overclock, best performance |

Choose based on your hardware's overclocking capability. Most boards work reliably at 378/133 MHz.

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

### Build Options

| Option | Description |
|--------|-------------|
| `-DBOARD_VARIANT=M1` | Use M1 GPIO layout (default) |
| `-DBOARD_VARIANT=M2` | Use M2 GPIO layout |
| `-DUSB_HID_ENABLED=ON` | Enable USB keyboard (disables USB serial) |
| `-DPS2_KEYBOARD_ENABLED=ON` | Enable PS/2 keyboard input |
| `-DDEBUG_LOGS_ENABLED=ON` | Enable verbose debug logging |
| `-DCPU_SPEED=504` | CPU overclock in MHz (252, 378, 504) |

Or use the build script (builds M1 by default):

```bash
./build.sh
```

### Release Builds

To build both M1 and M2 variants with version numbering:

```bash
./release.sh
```

This creates versioned UF2 files in the `release/` directory:
- `murmapple_m1_252_X_XX.uf2` (stock speed)
- `murmapple_m1_378_X_XX.uf2` (medium overclock)
- `murmapple_m1_504_X_XX.uf2` (max overclock)
- `murmapple_m2_252_X_XX.uf2` (stock speed)
- `murmapple_m2_378_X_XX.uf2` (medium overclock)
- `murmapple_m2_504_X_XX.uf2` (max overclock)

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

## Controls

### Keyboard
- Ctrl+Alt+Delete: Warm reset
- Open Apple (Left Alt/Left Windows): Left paddle button
- Closed Apple (Right Alt/Right Windows): Right paddle button
- F11: open Disk UI

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

## Author

Mikhail Matveev <<xtreme@rh1.tech>>

Website: [https://rh1.tech](https://rh1.tech)
