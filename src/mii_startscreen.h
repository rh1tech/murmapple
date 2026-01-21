/*
 * mii_startscreen.h
 *
 * Start screen display for MurmApple before emulator initialization
 * Shows system information and waits for user input to proceed
 */

#ifndef MII_STARTSCREEN_H
#define MII_STARTSCREEN_H

#ifndef PICO_RP2040 // for RP2350 only

#include <stdint.h>

typedef struct {
    const char *title;
    const char *subtitle;
    const char *version;
    uint32_t cpu_mhz;
#if PSRAM_MAX_FREQ_MHZ
    uint32_t psram_mhz;
#endif
    uint8_t board_variant;
} mii_startscreen_info_t;

/**
 * Display a start screen with system information
 * Returns 0 when ready to proceed (user presses a key or auto-timeout)
 */
int mii_startscreen_show(mii_startscreen_info_t *info);

#endif

#endif // MII_STARTSCREEN_H
