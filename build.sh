#!/bin/bash
# Build murmapple - Apple IIe emulator for RP2350

rm -rf ./build
mkdir build
cd build
cmake -DPICO_PLATFORM=rp2350 -DBOARD_VARIANT=M1 -DUSB_HID_ENABLED=0 ..
make -j4
