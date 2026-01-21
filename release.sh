#!/bin/bash
# Copyright (c) 2025-2026 Mikhail Matveev <xtreme@rh1.tech>
# Copyright (c) 2025-2026 DnCraptor <https://github.com/DnCraptor>
#
# release.sh - Build all release variants of murmapple
#
# Creates firmware files for each board variant:
#
# RP2350 with PSRAM (M1, M2) at each clock speed:
#   - Non-overclocked: 252 MHz CPU, 100 MHz PSRAM
#   - Medium overclock: 378 MHz CPU, 133 MHz PSRAM
#   - Max overclock: 504 MHz CPU, 166 MHz PSRAM
#
# RP2350 without PSRAM (M1, M2):
#   - Same clock speeds as above, for boards without PSRAM
#
# RP2040 (limited support, no PSRAM):
#   - Standard: 125 MHz CPU
#   - Overclocked: 252 MHz CPU
#
# Output formats:
#   - UF2 files for direct flashing via BOOTSEL mode
#   - m1p2/m2p2 files for Murmulator OS (RP2350 only)
#
# Output format: murmapple_mX_Y_Z_A_BB.{uf2,m1p2,m2p2}
#   X  = Board variant (1 or 2)
#   Y  = CPU clock in MHz
#   Z  = PSRAM clock in MHz, 'nopsram' for RP2350 without PSRAM, or 'rp2040' for RP2040
#   A  = Major version
#   BB = Minor version (zero-padded)
#

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

# RP2350 Build configurations (with PSRAM): "BOARD CPU_SPEED PSRAM_SPEED DESCRIPTION"
RP2350_CONFIGS=(
    "M1 252 100 non-overclocked"
    "M1 378 133 medium-overclock"
    "M1 504 166 max-overclock"
    "M2 252 100 non-overclocked"
    "M2 378 133 medium-overclock"
    "M2 504 166 max-overclock"
)

# RP2350 Build configurations (no PSRAM): "BOARD CPU_SPEED DESCRIPTION"
RP2350_NOPSRAM_CONFIGS=(
    "M1 252 non-overclocked"
    "M1 378 medium-overclock"
    "M1 504 max-overclock"
    "M2 252 non-overclocked"
    "M2 378 medium-overclock"
    "M2 504 max-overclock"
)

# RP2040 Build configurations (limited support, no PSRAM): "BOARD CPU_SPEED DESCRIPTION"
RP2040_CONFIGS=(
    "M1 125 standard"
    "M1 252 overclocked"
    "M2 125 standard"
    "M2 252 overclocked"
)

BUILD_COUNT=0
# Total builds: RP2350 UF2 + RP2350 no-PSRAM UF2 + RP2350 MOS2 + RP2040 UF2
TOTAL_BUILDS=$((${#RP2350_CONFIGS[@]} * 2 + ${#RP2350_NOPSRAM_CONFIGS[@]} + ${#RP2040_CONFIGS[@]}))

echo ""
echo -e "${YELLOW}Building $TOTAL_BUILDS firmware variants (RP2350 UF2 + MOS2 + RP2040)...${NC}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo ""
echo -e "${CYAN}=== Building RP2350 UF2 firmware files ===${NC}"

for config in "${RP2350_CONFIGS[@]}"; do
    read -r BOARD CPU PSRAM DESC <<< "$config"

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Board variant number
    if [[ "$BOARD" == "M1" ]]; then
        BOARD_NUM=1
    else
        BOARD_NUM=2
    fi

    # Output filename
    OUTPUT_NAME="murmapple_m${BOARD_NUM}_${CPU}_${PSRAM}_${VERSION}.uf2"

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | CPU: ${CPU} MHz | PSRAM: ${PSRAM} MHz | $DESC"

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Configure with CMake
    cmake .. \
        -DPICO_BOARD=pico2 \
        -DBOARD_VARIANT="$BOARD" \
        -DCPU_SPEED="$CPU" \
        -DPSRAM_SPEED="$PSRAM" \
        > /dev/null 2>&1

    # Build
    if make -j8 > /dev/null 2>&1; then
        # Copy UF2 to release directory (UF2 is output to bin/Release/)
        if [[ -f "$SCRIPT_DIR/bin/Release/murmapple.uf2" ]]; then
            cp "$SCRIPT_DIR/bin/Release/murmapple.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
            echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
        else
            echo -e "  ${RED}✗ UF2 not found${NC}"
        fi
    else
        echo -e "  ${RED}✗ Build failed${NC}"
    fi

    cd "$SCRIPT_DIR"
done

echo ""
echo -e "${CYAN}=== Building RP2350 UF2 firmware files (no PSRAM) ===${NC}"

for config in "${RP2350_NOPSRAM_CONFIGS[@]}"; do
    read -r BOARD CPU DESC <<< "$config"

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Board variant number
    if [[ "$BOARD" == "M1" ]]; then
        BOARD_NUM=1
    else
        BOARD_NUM=2
    fi

    # Output filename (no PSRAM)
    OUTPUT_NAME="murmapple_m${BOARD_NUM}_${CPU}_nopsram_${VERSION}.uf2"

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | CPU: ${CPU} MHz | No PSRAM (RP2350) | $DESC"

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Configure with CMake for RP2350 without PSRAM
    cmake .. \
        -DPICO_BOARD=pico2 \
        -DBOARD_VARIANT="$BOARD" \
        -DCPU_SPEED="$CPU" \
        > /dev/null 2>&1

    # Build
    if make -j8 > /dev/null 2>&1; then
        # Copy UF2 to release directory
        if [[ -f "$SCRIPT_DIR/bin/Release/murmapple.uf2" ]]; then
            cp "$SCRIPT_DIR/bin/Release/murmapple.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
            echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
        else
            echo -e "  ${RED}✗ UF2 not found${NC}"
        fi
    else
        echo -e "  ${RED}✗ Build failed${NC}"
    fi

    cd "$SCRIPT_DIR"
done

echo ""
echo -e "${CYAN}=== Building RP2350 MOS2 firmware files (Murmulator OS) ===${NC}"

for config in "${RP2350_CONFIGS[@]}"; do
    read -r BOARD CPU PSRAM DESC <<< "$config"

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Board variant number and MOS2 extension
    if [[ "$BOARD" == "M1" ]]; then
        BOARD_NUM=1
        MOS2_EXT="m1p2"
    else
        BOARD_NUM=2
        MOS2_EXT="m2p2"
    fi

    # Output filename for MOS2
    OUTPUT_NAME="murmapple_m${BOARD_NUM}_${CPU}_${PSRAM}_${VERSION}.${MOS2_EXT}"

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | CPU: ${CPU} MHz | PSRAM: ${PSRAM} MHz | $DESC | MOS2"

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Configure with CMake (MOS2 enabled)
    cmake .. \
        -DPICO_BOARD=pico2 \
        -DBOARD_VARIANT="$BOARD" \
        -DCPU_SPEED="$CPU" \
        -DPSRAM_SPEED="$PSRAM" \
        -DMOS2=ON \
        > /dev/null 2>&1

    # Build
    if make -j8 > /dev/null 2>&1; then
        # MOS2 builds produce murmapple.m1p2 or murmapple.m2p2 (renamed from .uf2)
        MOS2_BUILD_NAME="murmapple.${MOS2_EXT}"
        if [[ -f "$SCRIPT_DIR/bin/Release/${MOS2_BUILD_NAME}" ]]; then
            cp "$SCRIPT_DIR/bin/Release/${MOS2_BUILD_NAME}" "$RELEASE_DIR/$OUTPUT_NAME"
            echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
        else
            echo -e "  ${RED}✗ ${MOS2_EXT} file not found${NC}"
        fi
    else
        echo -e "  ${RED}✗ Build failed${NC}"
    fi

    cd "$SCRIPT_DIR"
done

echo ""
echo -e "${CYAN}=== Building RP2040 UF2 firmware files (limited support) ===${NC}"

for config in "${RP2040_CONFIGS[@]}"; do
    read -r BOARD CPU DESC <<< "$config"

    BUILD_COUNT=$((BUILD_COUNT + 1))

    # Board variant number
    if [[ "$BOARD" == "M1" ]]; then
        BOARD_NUM=1
    else
        BOARD_NUM=2
    fi

    # Output filename (RP2040 specific naming)
    OUTPUT_NAME="murmapple_m${BOARD_NUM}_${CPU}_rp2040_${VERSION}.uf2"

    echo ""
    echo -e "${CYAN}[$BUILD_COUNT/$TOTAL_BUILDS] Building: $OUTPUT_NAME${NC}"
    echo -e "  Board: $BOARD | CPU: ${CPU} MHz | RP2040 | $DESC"

    # Clean and create build directory
    rm -rf build
    mkdir build
    cd build

    # Configure with CMake for RP2040 (no PSRAM_SPEED)
    cmake .. \
        -DPICO_BOARD=pico \
        -DBOARD_VARIANT="$BOARD" \
        -DCPU_SPEED="$CPU" \
        > /dev/null 2>&1

    # Build
    if make -j8 > /dev/null 2>&1; then
        # Copy UF2 to release directory
        if [[ -f "$SCRIPT_DIR/bin/Release/murmapple.uf2" ]]; then
            cp "$SCRIPT_DIR/bin/Release/murmapple.uf2" "$RELEASE_DIR/$OUTPUT_NAME"
            echo -e "  ${GREEN}✓ Success${NC} → release/$OUTPUT_NAME"
        else
            echo -e "  ${RED}✗ UF2 not found${NC}"
        fi
    else
        echo -e "  ${RED}✗ Build failed${NC}"
    fi

    cd "$SCRIPT_DIR"
done

# Clean up build directory
rm -rf build bin

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${GREEN}Release build complete!${NC}"
echo ""

# Create ZIP archives
echo -e "${CYAN}=== Creating ZIP archives ===${NC}"
echo ""

cd "$RELEASE_DIR"

# M1 with PSRAM
ZIP_M1="murmapple_m1_${VERSION}.zip"
zip -q "$ZIP_M1" murmapple_m1_*_1??_${VERSION}.uf2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_M1" || echo -e "  ${YELLOW}⚠ No M1 PSRAM files${NC}"

# M1 without PSRAM
ZIP_M1_NOPSRAM="murmapple_m1_nopsram_${VERSION}.zip"
zip -q "$ZIP_M1_NOPSRAM" murmapple_m1_*_nopsram_${VERSION}.uf2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_M1_NOPSRAM" || echo -e "  ${YELLOW}⚠ No M1 nopsram files${NC}"

# M2 with PSRAM
ZIP_M2="murmapple_m2_${VERSION}.zip"
zip -q "$ZIP_M2" murmapple_m2_*_1??_${VERSION}.uf2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_M2" || echo -e "  ${YELLOW}⚠ No M2 PSRAM files${NC}"

# M2 without PSRAM
ZIP_M2_NOPSRAM="murmapple_m2_nopsram_${VERSION}.zip"
zip -q "$ZIP_M2_NOPSRAM" murmapple_m2_*_nopsram_${VERSION}.uf2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_M2_NOPSRAM" || echo -e "  ${YELLOW}⚠ No M2 nopsram files${NC}"

# MOS2 (Murmulator OS)
ZIP_MOS2="murmapple_mos2_${VERSION}.zip"
zip -q "$ZIP_MOS2" murmapple_*_${VERSION}.m?p2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_MOS2" || echo -e "  ${YELLOW}⚠ No MOS2 files${NC}"

# RP2040
ZIP_RP2040="murmapple_rp2040_${VERSION}.zip"
zip -q "$ZIP_RP2040" murmapple_*_rp2040_${VERSION}.uf2 2>/dev/null && \
    echo -e "  ${GREEN}✓${NC} $ZIP_RP2040" || echo -e "  ${YELLOW}⚠ No RP2040 files${NC}"

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
