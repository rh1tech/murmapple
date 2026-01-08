/*
 * mii_disk2_asm.h
 * 
 * Assembly helper for LSS tick - declares the asm function and struct offsets
 */

#pragma once

#ifdef MII_RP2350

#include "mii_disk2.h"
#include "mii_floppy.h"
#include "debug_log.h"
#include <stddef.h>

// Forward declaration of the assembly function
extern void mii_disk2_lss_tick_asm(
    mii_card_disk2_t *c,
    mii_floppy_t *f,
    const uint8_t *track,
    uint32_t bit_count,
    const uint8_t *lss_rom
);

// Compile-time offset verification macros
// These will cause build errors if offsets don't match assembly constants

#define STATIC_ASSERT_OFFSET(type, member, expected) \
    _Static_assert(offsetof(type, member) == expected, \
        "Offset mismatch for " #type "." #member ": expected " #expected)

// Call this function once at startup to print actual offsets for debugging
static inline void mii_disk2_print_offsets(void) {
#if ENABLE_DEBUG_LOGS
    MII_DEBUG_PRINTF("=== mii_card_disk2_t offsets ===\n");
    MII_DEBUG_PRINTF("  clock:          %zu\n", offsetof(mii_card_disk2_t, clock));
    // head, lss_state are bit-fields, can't use offsetof directly
    // They follow clock (uint16_t at +40) as: head (4 bits), then lss_state:4, lss_mode:4
    MII_DEBUG_PRINTF("  head:           ~%zu (bit-field after clock)\n", offsetof(mii_card_disk2_t, clock) + 2);
    MII_DEBUG_PRINTF("  data_register:  %zu\n", offsetof(mii_card_disk2_t, data_register));
    MII_DEBUG_PRINTF("  write_register: %zu\n", offsetof(mii_card_disk2_t, write_register));
    MII_DEBUG_PRINTF("=== mii_floppy_t offsets ===\n");
    // write_protected is also a bit-field at start
    MII_DEBUG_PRINTF("  bit_timing:     %zu\n", offsetof(mii_floppy_t, bit_timing));
    MII_DEBUG_PRINTF("  bit_position:   %zu\n", offsetof(mii_floppy_t, bit_position));
    MII_DEBUG_PRINTF("  random_position:%zu\n", offsetof(mii_floppy_t, random_position));
    MII_DEBUG_PRINTF("  random:         %zu\n", offsetof(mii_floppy_t, random));
#endif
}

#endif // MII_RP2350
