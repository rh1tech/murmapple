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

// Maximum number of disk images we can list
#define MAX_DISK_IMAGES 500

// Maximum filename length
#define MAX_FILENAME_LEN 64

// Disk image types
typedef enum {
    DISK_TYPE_UNKNOWN = 0,
    DISK_TYPE_DSK,      // .dsk, .do, .po - 140KB sector images
    DISK_TYPE_NIB,      // .nib - 232KB nibble images
    DISK_TYPE_WOZ,      // .woz - WOZ format (variable size)
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

// Global state
extern disk_entry_t g_disk_list[MAX_DISK_IMAGES];
extern int g_disk_count;
extern loaded_disk_t g_loaded_disks[2];  // Drive 1 and Drive 2

// Initialize SD card and scan for disk images
// Returns 0 on success, -1 on SD card error
int disk_loader_init(void);

// Scan /apple directory for disk images
// Returns number of images found
int disk_scan_directory(void);

// Select a disk image for a drive (does not read the full image into PSRAM)
// drive: 0 or 1 (Drive 1 or Drive 2)
// index: index into g_disk_list
// Returns 0 on success, -1 on error
int disk_load_image(int drive, int index);

// Unload a disk image (clears selection)
void disk_unload_image(int drive);

// Write back any modified disk image to SD card
// NOTE: not supported in the no-PSRAM loader; returns an error if called.
int disk_writeback(int drive);

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
int disk_mount_to_emulator(int drive, struct mii_t *mii, int slot, int preserve_state);

// Eject a disk from the emulator
// drive: 0 or 1 (Drive 1 or Drive 2)
// mii: pointer to the emulator instance
// slot: slot number where disk2 card is installed (usually 6)
void disk_eject_from_emulator(int drive, struct mii_t *mii, int slot);

#endif // DISK_LOADER_H
