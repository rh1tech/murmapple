/*
 * disk_loader.h
 * 
 * SD card disk image loader for murmapple
 * Scans /apple directory on SD card and mounts disk images into the emulator
 * without staging the entire image in PSRAM.
 */

#ifndef DISK_LOADER_H
#define DISK_LOADER_H

#include <stdint.h>
#include <stdbool.h>

// Maximum filename length
#define MAX_FILENAME_LEN 64

// Disk image types
typedef enum {
    DISK_TYPE_UNKNOWN = 0,
    DISK_TYPE_DSK,      // .dsk, .do, .po - 140KB sector images
    DISK_TYPE_NIB,      // .nib - 232KB nibble images
    DISK_TYPE_WOZ,      // .woz - WOZ format (variable size)
    DISK_TYPE_BDSK,     // .bdsk - binary disk dump, direct raw track nibbles with bit_count saved
    DIR_TYPE,           // W/A to use directories in the same list
} disk_type_t;

// Disk image sizes
#define DSK_IMAGE_SIZE  143360   // 35 tracks × 16 sectors × 256 bytes
#define NIB_IMAGE_SIZE  232960   // 35 tracks × 6656 bytes

// Disk image entry
typedef struct {
    char filename[MAX_FILENAME_LEN];
    uint32_t size;
    disk_type_t type;
} disk_entry_t;

// Selected/loaded disk image metadata (image data is read from SD on mount)
typedef struct {
    uint8_t *data;          // Unused on RP2350 (kept for compatibility)
    uint32_t size;          // Size of image data
    disk_type_t type;       // Type of disk image
    char filename[MAX_FILENAME_LEN];
    bool loaded;            // True if image is loaded
    bool write_back;        // Unused on RP2350 (kept for compatibility)
} loaded_disk_t;

#define BDSK_MAGIC "BDSK"
#define BDSK_VERSION 1
#define BDSK_TRACKS 35
#define BDSK_TRACK_DATA_SIZE 6656
#define BDSK_MAX_BITS (BDSK_TRACK_DATA_SIZE * 8)

// Track data: packed bits, MSB first in each byte.
// Bit 0 is MSB of data[0].
// native for RP2040/RP2350 bit-order
// Bits are circular: bit positions wrap at bit_count.
typedef struct bdsk_header {
    char     magic[4];      // "BDSK"
    uint16_t version;       // 1
    uint16_t tracks;        // 35
} bdsk_header_t;

typedef struct bdsk_track_desc {
    uint32_t bit_count;     // ≤ 6656*8
//    uint32_t byte_count;    // fixed for this version (v1): 6656 == NIBBLE_TRACK_SIZE
// Bits beyond bit_count up to BDSK_TRACK_DATA_SIZE*8 are undefined (padding).
} bdsk_track_desc_t;

#define BDSK_BYTES (sizeof(bdsk_header_t) + BDSK_TRACKS * (sizeof(bdsk_track_desc_t) + BDSK_TRACK_DATA_SIZE))

// Global state
extern disk_entry_t* g_disk_list;
extern int g_disk_count;
extern loaded_disk_t g_loaded_disks[2];  // Drive 1 and Drive 2

// Initialize SD card and scan for disk images
// Returns 0 on success, -1 on SD card error
int disk_loader_init(void);

// Scan /apple directory for disk images
// Returns number of images found
int disk_scan_directory(const char* __restrict path);

// Select a disk image for a drive (does not read the full image into PSRAM)
// drive: 0 or 1 (Drive 1 or Drive 2)
// index: index into g_disk_list
// Returns 0 on success, -1 on error
int disk_load_image(int drive, int index, bool write);

// Unload a disk image (clears selection)
void disk_unload_image(int drive);

// Get disk image type from filename extension
disk_type_t disk_get_type(const char *filename);

// Forward declarations
struct mii_t;

// Mount a loaded disk image to the emulator
// drive: 0 or 1 (Drive 1 or Drive 2)
// mii: pointer to the emulator instance
// slot: slot number where disk2 card is installed (usually 6)
// preserve_state: if true, keeps motor/head position (for disk swap during game)
// Returns 0 on success, -1 on error
int disk_mount_to_emulator(int drive, struct mii_t *mii, int slot, int preserve_state, bool read_only, bool bdsk_recreate);

// Eject a disk from the emulator
// drive: 0 or 1 (Drive 1 or Drive 2)
// mii: pointer to the emulator instance
// slot: slot number where disk2 card is installed (usually 6)
void disk_eject_from_emulator(int drive, struct mii_t *mii, int slot);

#endif // DISK_LOADER_H
