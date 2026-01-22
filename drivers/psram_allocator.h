#ifndef PSRAM_ALLOCATOR_H
#define PSRAM_ALLOCATOR_H

#include <stddef.h>

// Total external PSRAM size (bytes). Keep in sync with the hardware used.
// Used by UI/status display and allocator partitioning.
#ifndef MURM_PSRAM_SIZE_BYTES
#define MURM_PSRAM_SIZE_BYTES (8u * 1024u * 1024u)
#endif
#define PSRAM_DATA ((uint8_t*)(intptr_t)0x11000000)

void *psram_malloc(size_t size);
void *psram_realloc(void *ptr, size_t size);
void psram_free(void *ptr);
void psram_reset(void);
void psram_mark_session(void);    // Mark current offset for game session
void psram_restore_session(void); // Restore to marked offset
void *psram_get_scratch_1(size_t size);
void *psram_get_scratch_2(size_t size);
void *psram_get_file_buffer(size_t size);

void psram_set_temp_mode(int enable);
void psram_reset_temp(void);
size_t psram_get_temp_offset(void);
void psram_set_temp_offset(size_t offset);

void psram_set_sram_mode(int enable); // Force SRAM allocation for proper malloc/free

unsigned int butter_psram_size();

#endif
