#!/bin/bash
# Build murmapple - Apple IIe emulator for RP2350
#
# Usage: ./build.sh [OPTIONS]
#   -b, --board      Board variant: M1 (default) or M2
#   -v, --video      Video output: HDMI (default) or VGA
#   -a, --audio      Audio output: I2S (default) or PWM
#   -p, --psram      PSRAM speed in MHz (default: 100)
#   --nopsram        Build without PSRAM support
#   -c, --cpu        CPU speed in MHz: 252 (default), 378, 504
#   --mos2           Build for Murmulator OS (m1p2/m2p2 format)
#   --rp2040         Build for RP2040 instead of RP2350
#   -h, --help       Show this help

# Defaults
BOARD="M1"
VIDEO="HDMI"
AUDIO="I2S"
PSRAM="100"
CPU="252"
MOS2="OFF"
PLATFORM="rp2350"
PICO_BOARD="pico2"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--board)
            BOARD="$2"
            shift 2
            ;;
        -v|--video)
            VIDEO="$2"
            shift 2
            ;;
        -a|--audio)
            AUDIO="$2"
            shift 2
            ;;
        -p|--psram)
            PSRAM="$2"
            shift 2
            ;;
        --nopsram)
            PSRAM=""
            shift
            ;;
        -c|--cpu)
            CPU="$2"
            shift 2
            ;;
        --mos2)
            MOS2="ON"
            shift
            ;;
        --rp2040)
            PLATFORM="rp2040"
            PICO_BOARD="pico"
            shift
            ;;
        -h|--help)
            head -15 "$0" | tail -13
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Build cmake arguments
CMAKE_ARGS="-DPICO_BOARD=$PICO_BOARD"
CMAKE_ARGS="$CMAKE_ARGS -DBOARD_VARIANT=$BOARD"
CMAKE_ARGS="$CMAKE_ARGS -DVIDEO_TYPE=$VIDEO"
CMAKE_ARGS="$CMAKE_ARGS -DAUDIO_TYPE=$AUDIO"
CMAKE_ARGS="$CMAKE_ARGS -DCPU_SPEED=$CPU"
CMAKE_ARGS="$CMAKE_ARGS -DUSB_HID_ENABLED=0"

if [[ -n "$PSRAM" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DPSRAM_SPEED=$PSRAM"
fi

if [[ "$MOS2" == "ON" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DMOS2=ON"
fi

echo "Building murmapple:"
echo "  Board: $BOARD"
echo "  Video: $VIDEO"
echo "  Audio: $AUDIO"
echo "  CPU: $CPU MHz"
echo "  PSRAM: ${PSRAM:-none}"
echo "  Platform: $PLATFORM"
echo "  MOS2: $MOS2"
echo ""

rm -rf ./build
mkdir build
cd build
cmake $CMAKE_ARGS ..
make -j4
