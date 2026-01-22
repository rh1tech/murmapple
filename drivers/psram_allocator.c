#include "psram_allocator.h"
#include "psram_allocator.h"
#include <pico.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PSRAM is mapped to XIP_SRAM_BASE + offset
// On RP2350, XIP base is 0x10000000.
// Flash is at 0x10000000.
// PSRAM (CS1) is usually mapped at 0x11000000.

#define PSRAM_BASE 0x11000000
#define PSRAM_SIZE ((size_t)MURM_PSRAM_SIZE_BYTES)

static uint8_t *psram_start = (uint8_t *)PSRAM_BASE;
// Reserve 512KB for scratch buffers at the beginning
// 0-64KB: Scratch 1 (Decompression)
// 64-128KB: Scratch 2 (Conversion)
// 128-384KB: File Load Buffer (256KB)
#define SCRATCH_SIZE (512 * 1024)
static size_t psram_offset = SCRATCH_SIZE;

// Temp allocator support
// Some MIDI files exceed available temp memory - game continues without music
#define TEMP_SIZE (4 * 1024 * 1024) // 4MB for temp (music)
#define PERM_SIZE (PSRAM_SIZE - TEMP_SIZE) // 4MB for permanent
static size_t psram_temp_offset = 0;
static int psram_temp_mode = 0;
static int psram_sram_mode = 0; // Force SRAM allocation (proper malloc/free)
static size_t psram_session_mark = 0; // Save point for game session memory

void psram_set_temp_mode(int enable) {
    psram_temp_mode = enable;
}

void psram_set_sram_mode(int enable) {
    psram_sram_mode = enable;
}

void psram_reset_temp(void) {
    psram_temp_offset = 0;
}

size_t psram_get_temp_offset(void) {
    return psram_temp_offset;
}

void psram_set_temp_offset(size_t offset) {
    psram_temp_offset = offset;
}

void *psram_malloc(size_t size) {
    // If SRAM mode is enabled, use regular malloc (for peels that need proper free)
    if (psram_sram_mode) {
        return malloc(size);
    }
    
    // Align to 4 bytes
    size = (size + 3) & ~3;
    
    // Add header for size tracking (needed for realloc)
    size_t total_size = size + sizeof(size_t);

    if (psram_temp_mode) {
        if (psram_temp_offset + total_size > TEMP_SIZE) {
            printf("PSRAM Temp OOM! Req %d, free %d\n", (int)size, (int)(TEMP_SIZE - psram_temp_offset));
            return NULL;
        }
        size_t *header = (size_t *)(psram_start + PERM_SIZE + psram_temp_offset);
        *header = size;
        void *ptr = (void *)(header + 1);
        psram_temp_offset += total_size;
        return ptr;
    } else {
        if (psram_offset + total_size > PERM_SIZE) {
            printf("PSRAM Perm OOM! Req %d, free %d\n", (int)size, (int)(PERM_SIZE - psram_offset));
            fflush(stdout);
            return NULL;
        }
        
        size_t *header = (size_t *)(psram_start + psram_offset);
        *header = size;
        
        void *ptr = (void *)(header + 1);
        // Only log large allocations or when getting low on memory
        size_t remaining = PERM_SIZE - (psram_offset + total_size);
        if (size >= 65536 || remaining < 256 * 1024) {
            printf("psram_malloc(%d) -> %p Total: %d Remaining: %d\n", 
                   (int)size, ptr, (int)(psram_offset + total_size), (int)remaining);
            fflush(stdout);
        }
        psram_offset += total_size;
        return ptr;
    }
}

void *psram_realloc(void *ptr, size_t new_size) {
    if (ptr == NULL) return psram_malloc(new_size);
    if (new_size == 0) { psram_free(ptr); return NULL; }

    if ((uintptr_t)ptr >= PSRAM_BASE && (uintptr_t)ptr < (PSRAM_BASE + PSRAM_SIZE)) {
        // It's in PSRAM
        size_t *header = (size_t *)ptr - 1;
        size_t old_size = *header;

        if (new_size <= old_size) {
            return ptr; // Shrink or same size: do nothing
        }

        void *new_ptr = psram_malloc(new_size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, old_size);
            // psram_free(ptr); // No-op for bump allocator
        }
        return new_ptr;
    }

    // Fallback for SRAM pointers
    return realloc(ptr, new_size);
}

void *psram_get_scratch_1(size_t size) {
    if (size > 128 * 1024) return NULL;
    return psram_start;
}

void *psram_get_scratch_2(size_t size) {
    if (size > 128 * 1024) return NULL;
    return psram_start + (128 * 1024);
}

void *psram_get_file_buffer(size_t size) {
    if (size > 256 * 1024) {
        printf("PSRAM File Buffer too small! Req: %d\n", (int)size);
        return NULL;
    }
    return psram_start + (256 * 1024);
}


void psram_free(void *ptr) {
    if (ptr >= (void*)PSRAM_BASE && ptr < (void*)(PSRAM_BASE + PSRAM_SIZE)) {
        // It's in PSRAM, do nothing (bump allocator)
        return;
    }
    // It's not in PSRAM, assume it's from malloc
    free(ptr);
}

void psram_reset(void) {
    psram_offset = SCRATCH_SIZE; // Reset to after scratch area
    psram_temp_offset = 0;
    psram_session_mark = 0;
}

void psram_mark_session(void) {
    psram_session_mark = psram_offset;
    printf("PSRAM: Session marked at offset %d (%.2f MB used)\n", 
           (int)psram_session_mark, psram_session_mark / (1024.0 * 1024.0));
}

void psram_restore_session(void) {
    if (psram_session_mark == 0) {
        printf("PSRAM: Warning - no session mark set, cannot restore\n");
        return;
    }
    size_t freed = psram_offset - psram_session_mark;
    psram_offset = psram_session_mark;
    psram_temp_offset = 0;
    printf("PSRAM: Session restored to offset %d (freed %.2f MB)\n",
           (int)psram_offset, freed / (1024.0 * 1024.0));
}

static int BUTTER_PSRAM_SIZE = -1;
#define MB16 (16ul << 20)
#define MB8 (8ul << 20)
#define MB4 (4ul << 20)
#define MB1 (1ul << 20)
unsigned int __not_in_flash_func() butter_psram_size() {
    if (BUTTER_PSRAM_SIZE != -1) return BUTTER_PSRAM_SIZE;
    for(register int i = MB8; i < MB16; i += 4096)
        PSRAM_DATA[i] = 16;
    for(register int i = MB4; i < MB8; i += 4096)
        PSRAM_DATA[i] = 8;
    for(register int i = MB1; i < MB4; i += 4096)
        PSRAM_DATA[i] = 4;
    for(register int i = 0; i < MB1; i += 4096)
        PSRAM_DATA[i] = 1;
    register uint32_t res = PSRAM_DATA[MB16 - 4096];
    for (register int i = MB16 - MB1; i < MB16; i += 4096) {
        if (res != PSRAM_DATA[i])
            return 0;
    }
    BUTTER_PSRAM_SIZE = res << 20;
    return BUTTER_PSRAM_SIZE;
}
