#!/bin/bash
# Copyright (c) 2025-2026 Mikhail Matveev <xtreme@rh1.tech>
# Copyright (c) 2025-2026 DnCraptor <https://github.com/DnCraptor>
#
# release.sh - Build all release variants of murmapple
#
# Creates firmware files for each combination:
#
# RP2350 variants (M1, M2):
#   - HDMI + I2S (with and without PSRAM)
#   - HDMI + PWM (with and without PSRAM)
#   - VGA + I2S (with and without PSRAM)
#   - VGA + PWM (with and without PSRAM)
#
# MOS2 variants (M1, M2) - Murmulator OS:
#   - Same combinations as above (with PSRAM only)
#
# RP2040 variants (M1, M2) - no PSRAM:
#   - HDMI + I2S
#   - HDMI + PWM
#   - VGA + I2S
#   - VGA + PWM
#
# Output format: murmapple_<board>_<video>_<audio>_<psram>_<version>.{uf2,m1p2,m2p2}

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Version file
VERSION_FILE="version.txt"

# Read last version or initialize
if [[ -f "$VERSION_FILE" ]]; then
    read -r LAST_MAJOR LAST_MINOR < "$VERSION_FILE"
else
    LAST_MAJOR=1
    LAST_MINOR=0
fi

# Calculate next version (for default suggestion)
NEXT_MINOR=$((LAST_MINOR + 1))
NEXT_MAJOR=$LAST_MAJOR
if [[ $NEXT_MINOR -ge 100 ]]; then
    NEXT_MAJOR=$((NEXT_MAJOR + 1))
    NEXT_MINOR=0
fi

# Interactive version input
echo ""
echo -e "${CYAN}┌─────────────────────────────────────────────────────────────────┐${NC}"
echo -e "${CYAN}│                    MurmApple Release Builder                    │${NC}"
echo -e "${CYAN}└─────────────────────────────────────────────────────────────────┘${NC}"
echo ""
echo -e "Last version: ${YELLOW}${LAST_MAJOR}.$(printf '%02d' $LAST_MINOR)${NC}"
echo ""

DEFAULT_VERSION="${NEXT_MAJOR}.$(printf '%02d' $NEXT_MINOR)"
read -p "Enter version [default: $DEFAULT_VERSION]: " INPUT_VERSION
INPUT_VERSION=${INPUT_VERSION:-$DEFAULT_VERSION}

# Parse version (handle both "1.00" and "1 00" formats)
if [[ "$INPUT_VERSION" == *"."* ]]; then
    MAJOR="${INPUT_VERSION%%.*}"
    MINOR="${INPUT_VERSION##*.}"
else
    read -r MAJOR MINOR <<< "$INPUT_VERSION"
fi

# Remove leading zeros for arithmetic, then re-pad
MINOR=$((10#$MINOR))
MAJOR=$((10#$MAJOR))

# Validate
if [[ $MAJOR -lt 1 ]]; then
    echo -e "${RED}Error: Major version must be >= 1${NC}"
    exit 1
fi
if [[ $MINOR -lt 0 || $MINOR -ge 100 ]]; then
    echo -e "${RED}Error: Minor version must be 0-99${NC}"
    exit 1
fi

# Format version string
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"
echo ""
echo -e "${GREEN}Building release version: ${MAJOR}.$(printf '%02d' $MINOR)${NC}"

# Save new version
echo "$MAJOR $MINOR" > "$VERSION_FILE"

# Create release directory
RELEASE_DIR="$SCRIPT_DIR/release"
mkdir -p "$RELEASE_DIR"

# Configuration arrays
BOARDS=("M1" "M2")
VIDEO_TYPES=("HDMI" "VGA")
AUDIO_TYPES=("I2S" "PWM")
CPU_SPEED="252"  # No overclocking in release
PSRAM_SPEED="100"  # Default PSRAM speed when enabled

# Count total builds
# RP2350 with PSRAM: 2 boards * 2 video * 2 audio = 8
# RP2350 no PSRAM: 2 boards * 2 video * 2 audio = 8
# MOS2: 2 boards * 2 video * 2 audio = 8
# RP2040: 2 boards * 2 video * 2 audio = 8
TOTAL_BUILDS=$((8 + 8 + 8 + 8))
BUILD_COUNT=0

echo ""
echo -e "${YELLOW}Building $TOTAL_BUILDS firmware variants...${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# Function to build a single variant
build_variant() {
    local BOARD=$1
    local VIDEO=$2
    local AUDIO=$3
    local PSRAM=$4      # "100" or "" for no PSRAM
    local MOS2=$5       # "ON" or "OFF"
    local PLATFORM=$6   # "rp2350" or "rp2040"

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Determine board number for filename
    local BOARD_NUM=1
    [[ "$BOARD" == "M2" ]] && BOARD_NUM=2

    # Determine PICO_BOARD
    local PICO_BOARD="pico2"
    [[ "$PLATFORM" == "rp2040" ]] && PICO_BOARD="pico"

    # Build output filename
    local VIDEO_LC=$(echo "$VIDEO" | tr '[:upper:]' '[:lower:]')
    local AUDIO_LC=$(echo "$AUDIO" | tr '[:upper:]' '[:lower:]')
    local PSRAM_TAG="psram"
    [[ -z "$PSRAM" ]] && PSRAM_TAG="nopsram"
    [[ "$PLATFORM" == "rp2040" ]] && PSRAM_TAG="rp2040"

    # Determine file extension
    local EXT="uf2"
    if [[ "$MOS2" == "ON" ]]; then
        [[ "$BOARD" == "M1" ]] && EXT="m1p2"
        [[ "$BOARD" == "M2" ]] && EXT="m2p2"
    fi

    local OUTPUT_NAME="murmapple_m${BOARD_NUM}_${VIDEO_LC}_${AUDIO_LC}_${PSRAM_TAG}_${VERSION}.${EXT}"

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | Video: $VIDEO | Audio: $AUDIO | PSRAM: ${PSRAM:-none} | MOS2: $MOS2"

    # Clean and create build directory
    rm -rf build bin/Release
    mkdir -p build bin/Release
    cd build

    # Build cmake arguments
    local CMAKE_ARGS="-DPICO_BOARD=$PICO_BOARD"
    CMAKE_ARGS="$CMAKE_ARGS -DBOARD_VARIANT=$BOARD"
    CMAKE_ARGS="$CMAKE_ARGS -DVIDEO_TYPE=$VIDEO"
    CMAKE_ARGS="$CMAKE_ARGS -DAUDIO_TYPE=$AUDIO"
    CMAKE_ARGS="$CMAKE_ARGS -DCPU_SPEED=$CPU_SPEED"

    [[ -n "$PSRAM" ]] && CMAKE_ARGS="$CMAKE_ARGS -DPSRAM_SPEED=$PSRAM"
    [[ "$MOS2" == "ON" ]] && CMAKE_ARGS="$CMAKE_ARGS -DMOS2=ON"

    # Configure with CMake
    if cmake $CMAKE_ARGS .. > /dev/null 2>&1; then
        # Build
        if make -j8 > /dev/null 2>&1; then
            # Find and copy output file (CMake creates dynamic names like m1p2-murmapple-HDMI-252MHz-...)
            local SRC_FILE=""
            if [[ "$MOS2" == "ON" ]]; then
                # MOS2 builds produce .m1p2 or .m2p2 files
                SRC_FILE=$(find "$SCRIPT_DIR/bin/Release" -maxdepth 1 -name "*.${EXT}" -type f 2>/dev/null | head -1)
            else
                # UF2 builds
                SRC_FILE=$(find "$SCRIPT_DIR/bin/Release" -maxdepth 1 -name "*.uf2" -type f 2>/dev/null | head -1)
            fi

            if [[ -n "$SRC_FILE" && -f "$SRC_FILE" ]]; then
                cp "$SRC_FILE" "$RELEASE_DIR/$OUTPUT_NAME"
                echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
            else
                echo -e "  ${RED}✗ Output file not found${NC}"
            fi
        else
            echo -e "  ${RED}✗ Build failed${NC}"
        fi
    else
        echo -e "  ${RED}✗ CMake failed${NC}"
    fi

    cd "$SCRIPT_DIR"
}

# ============================================================================
# RP2350 with PSRAM (UF2)
# ============================================================================
echo ""
echo -e "${CYAN}=== Building RP2350 UF2 firmware (with PSRAM) ===${NC}"

for BOARD in "${BOARDS[@]}"; do
    for VIDEO in "${VIDEO_TYPES[@]}"; do
        for AUDIO in "${AUDIO_TYPES[@]}"; do
            build_variant "$BOARD" "$VIDEO" "$AUDIO" "$PSRAM_SPEED" "OFF" "rp2350"
        done
    done
done

# ============================================================================
# RP2350 without PSRAM (UF2)
# ============================================================================
echo ""
echo -e "${CYAN}=== Building RP2350 UF2 firmware (no PSRAM) ===${NC}"

for BOARD in "${BOARDS[@]}"; do
    for VIDEO in "${VIDEO_TYPES[@]}"; do
        for AUDIO in "${AUDIO_TYPES[@]}"; do
            build_variant "$BOARD" "$VIDEO" "$AUDIO" "" "OFF" "rp2350"
        done
    done
done

# ============================================================================
# MOS2 (Murmulator OS) - with PSRAM only
# ============================================================================
echo ""
echo -e "${CYAN}=== Building MOS2 firmware (Murmulator OS) ===${NC}"

for BOARD in "${BOARDS[@]}"; do
    for VIDEO in "${VIDEO_TYPES[@]}"; do
        for AUDIO in "${AUDIO_TYPES[@]}"; do
            build_variant "$BOARD" "$VIDEO" "$AUDIO" "$PSRAM_SPEED" "ON" "rp2350"
        done
    done
done

# ============================================================================
# RP2040 (no PSRAM)
# ============================================================================
echo ""
echo -e "${CYAN}=== Building RP2040 UF2 firmware ===${NC}"

for BOARD in "${BOARDS[@]}"; do
    for VIDEO in "${VIDEO_TYPES[@]}"; do
        for AUDIO in "${AUDIO_TYPES[@]}"; do
            build_variant "$BOARD" "$VIDEO" "$AUDIO" "" "OFF" "rp2040"
        done
    done
done

# ============================================================================
# Clean up and create ZIP archives
# ============================================================================
rm -rf build bin

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}Release build complete!${NC}"
echo ""

echo -e "${CYAN}=== Creating ZIP archives ===${NC}"
echo ""

cd "$RELEASE_DIR"

# Create ZIPs by category
for BOARD in "m1" "m2"; do
    # PSRAM versions
    ZIP_PSRAM="murmapple_${BOARD}_psram_${VERSION}.zip"
    zip -q "$ZIP_PSRAM" murmapple_${BOARD}_*_psram_${VERSION}.uf2 2>/dev/null && \
        echo -e "  ${GREEN}✓${NC} $ZIP_PSRAM" || echo -e "  ${YELLOW}⚠ No ${BOARD} PSRAM files${NC}"

    # No-PSRAM versions
    ZIP_NOPSRAM="murmapple_${BOARD}_nopsram_${VERSION}.zip"
    zip -q "$ZIP_NOPSRAM" murmapple_${BOARD}_*_nopsram_${VERSION}.uf2 2>/dev/null && \
        echo -e "  ${GREEN}✓${NC} $ZIP_NOPSRAM" || echo -e "  ${YELLOW}⚠ No ${BOARD} nopsram files${NC}"

    # RP2040 versions
    ZIP_RP2040="murmapple_${BOARD}_rp2040_${VERSION}.zip"
    zip -q "$ZIP_RP2040" murmapple_${BOARD}_*_rp2040_${VERSION}.uf2 2>/dev/null && \
        echo -e "  ${GREEN}✓${NC} $ZIP_RP2040" || echo -e "  ${YELLOW}⚠ No ${BOARD} rp2040 files${NC}"
done

# MOS2 archive
ZIP_MOS2="murmapple_mos2_${VERSION}.zip"
zip -q "$ZIP_MOS2" murmapple_*_${VERSION}.m?p2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_MOS2" || echo -e "  ${YELLOW}⚠ No MOS2 files${NC}"

# Remove individual files after zipping (keep only ZIPs)
rm -f murmapple_*.uf2 murmapple_*.m?p2 2>/dev/null

cd "$SCRIPT_DIR"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "Release archives in: $RELEASE_DIR/"
echo ""
ls -la "$RELEASE_DIR"/*.zip 2>/dev/null | awk '{print "  " $9 " (" $5 " bytes)"}'
echo ""
echo -e "Version: ${CYAN}${MAJOR}.$(printf '%02d' $MINOR)${NC}"
