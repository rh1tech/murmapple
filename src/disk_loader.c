/*
 * disk_loader.c
 * 
 * SD card disk image loader for murmapple
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "disk_loader.h"
#include "ff.h"
#include "pico/stdlib.h"

// MII emulator headers
#include "mii.h"
#include "mii_dd.h"
#include "mii_floppy.h"
#include "mii_slot.h"
#include "mii_dsk.h"
#include "mii_nib.h"
#include "mii_woz.h"
#include "mii_video.h"
#include "debug_log.h"

// Global state
disk_entry_t g_disk_list[MAX_DISK_IMAGES];
int g_disk_count = 0;
loaded_disk_t g_loaded_disks[2] = {0};

// FatFS objects
static FATFS fs;
static bool sd_mounted = false;

// Endian conversion macros for little-endian platforms (ARM is LE)
#ifndef le32toh
#define le32toh(x) (x)
#define htole32(x) (x)
#define le16toh(x) (x)
#define htole16(x) (x)
#endif

static bool disk_open_image_file(const char *filename, FIL *out_fp, char *out_path, size_t out_path_len) {
    if (!sd_mounted)
        return false;
    if (!filename || !out_fp)
        return false;

    char path[128];
    snprintf(path, sizeof(path), "/apple/%s", filename);
    FRESULT fr = f_open(out_fp, path, FA_READ);
    if (fr != FR_OK) {
        snprintf(path, sizeof(path), "/%s", filename);
        fr = f_open(out_fp, path, FA_READ);
        if (fr != FR_OK)
            return false;
    }
    if (out_path && out_path_len) {
        strncpy(out_path, path, out_path_len - 1);
        out_path[out_path_len - 1] = '\0';
    }
    return true;
}

//  DOS 3.3 Physical sector order (index is physical sector, value is DOS sector)
static const uint8_t DO_SECMAP[16] = {
    0x0, 0x7, 0xE, 0x6, 0xD, 0x5, 0xC, 0x4,
    0xB, 0x3, 0xA, 0x2, 0x9, 0x1, 0x8, 0xF
};
// ProDOS Physical sector order (index is physical sector, value is ProDOS sector)
static const uint8_t PO_SECMAP[16] = {
    0x0, 0x8, 0x1, 0x9, 0x2, 0xa, 0x3, 0xb,
    0x4, 0xc, 0x5, 0xd, 0x6, 0xe, 0x7, 0xf
};

#define DSK_SECTOR_SIZE 256
#define DSK_TRACKS 35
#define DSK_SECTORS 16
#define DSK_TRACK_BYTES (DSK_SECTOR_SIZE * DSK_SECTORS)

static inline uint16_t disk_le16(const void *p) {
	const uint8_t *b = (const uint8_t *)p;
	return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static int disk_load_floppy_dsk_from_fatfs(mii_floppy_t *floppy, mii_dd_file_t *file, FIL *fp) {
    const char *dot = strrchr(file->pathname, '.');
    const uint8_t *secmap = DO_SECMAP;
    if (dot && (!strcasecmp(dot, ".po") || !strcasecmp(dot, ".PO"))) {
        secmap = PO_SECMAP;
    }

    for (int track = 0; track < DSK_TRACKS; ++track) {
        mii_floppy_track_t *dst = &floppy->tracks[track];
        uint8_t *track_data = floppy->curr_track_data;
        dst->bit_count = 0;
        dst->virgin = 0;
        dst->has_map = 1;

        for (int phys_sector = 0; phys_sector < DSK_SECTORS; phys_sector++) {
            uint8_t sector_buf[DSK_SECTOR_SIZE];
            const uint8_t dos_sector = secmap[phys_sector];
            const uint32_t off = (uint32_t)((DSK_SECTORS * track + dos_sector) * DSK_SECTOR_SIZE);

            FRESULT fr = f_lseek(fp, off);
            if (fr != FR_OK) {
                printf("%s: f_lseek(%lu) failed: %d\n", __func__, (unsigned long)off, fr);
                return -1;
            }
            UINT br = 0;
            fr = f_read(fp, sector_buf, sizeof(sector_buf), &br);
            if (fr != FR_OK || br != sizeof(sector_buf)) {
                printf("%s: f_read sector failed: fr=%d br=%u\n", __func__, fr, br);
                return -1;
            }

            // Volume number is 254, as in mii_dsk.c
            mii_floppy_dsk_render_sector(254, (uint8_t)track, (uint8_t)phys_sector, sector_buf, dst, track_data);
            dst->map.sector[phys_sector].dsk_position = off;
        }
    }
    return 0;
}


static int
disk_load_floppy_dsk_track_from_fatfs(
    mii_floppy_t   *floppy,
    mii_dd_file_t  *file,
    FIL            *fp,
    uint8_t         track_id
) {
    printf("load track: %d\n", track_id);
    if (track_id >= DSK_TRACKS)
        return 0;

    const char *dot = strrchr(file->pathname, '.');
    const uint8_t *secmap = DO_SECMAP;
    if (dot && (!strcasecmp(dot, ".po") || !strcasecmp(dot, ".PO"))) {
        secmap = PO_SECMAP;
    }

    mii_floppy_track_t *dst = &floppy->tracks[track_id];

    uint8_t *track_data = floppy->curr_track_data;

    dst->bit_count = 0;
    dst->virgin = 0;
    dst->has_map = 1;

    for (int phys_sector = 0; phys_sector < DSK_SECTORS; phys_sector++) {
        uint8_t sector_buf[DSK_SECTOR_SIZE];

        const uint8_t dos_sector = secmap[phys_sector];
        const uint32_t off =
            (uint32_t)((DSK_SECTORS * track_id + dos_sector) * DSK_SECTOR_SIZE);

        FRESULT fr = f_lseek(fp, off);
        if (fr != FR_OK)
            goto fail;

        UINT br = 0;
        fr = f_read(fp, sector_buf, sizeof(sector_buf), &br);
        if (fr != FR_OK || br != sizeof(sector_buf))
            goto fail;

        mii_floppy_dsk_render_sector(
            254,
            track_id,
            phys_sector,
            sector_buf,
            dst,
            track_data
        );

        dst->map.sector[phys_sector].dsk_position = off;
    }

    dst->dirty = 0;
    return 0;

fail:
    return -1;
}

static int disk_load_floppy_nib_from_fatfs(mii_floppy_t *floppy, FIL *fp) {
    uint8_t *track_buf = (uint8_t *)malloc(MII_FLOPPY_MAX_TRACK_SIZE);
    if (!track_buf) {
        printf("%s: out of memory\n", __func__);
        return -1;
    }
    for (int track = 0; track < 35; track++) {
        const uint32_t off = (uint32_t)(track * MII_FLOPPY_MAX_TRACK_SIZE);
        FRESULT fr = f_lseek(fp, off);
        if (fr != FR_OK) {
            printf("%s: f_lseek(%lu) failed: %d\n", __func__, (unsigned long)off, fr);
            free(track_buf);
            return -1;
        }
        UINT br = 0;
        fr = f_read(fp, track_buf, MII_FLOPPY_MAX_TRACK_SIZE, &br);
        if (fr != FR_OK || br != MII_FLOPPY_MAX_TRACK_SIZE) {
            printf("%s: f_read track failed: fr=%d br=%u\n", __func__, fr, br);
            free(track_buf);
            return -1;
        }
        uint8_t* track_data = floppy->curr_track_data;
        mii_floppy_nib_render_track(track_buf, &floppy->tracks[track], track_data);
        if (floppy->tracks[track].bit_count < 100) {
            printf("%s: invalid NIB track %d\n", __func__, track);
            free(track_buf);
            return -1;
        }
        floppy->tracks[track].dirty = 0;
    }
    free(track_buf);
    return 0;
}

static int
disk_load_floppy_nib_track_from_fatfs(
    mii_floppy_t *floppy,
    FIL *fp,
    uint8_t track_id
) {
    printf("load track: %d\n", track_id);
    if (track_id >= 35)
        return 0;

    uint8_t *track_buf = (uint8_t *)malloc(MII_FLOPPY_MAX_TRACK_SIZE);
    if (!track_buf) {
        printf("%s: out of memory\n", __func__);
        return -1;
    }

    const uint32_t off =
        (uint32_t)track_id * MII_FLOPPY_MAX_TRACK_SIZE;

    FRESULT fr = f_lseek(fp, off);
    if (fr != FR_OK) {
        printf("%s: f_lseek(%lu) failed: %d\n",
                         __func__, (unsigned long)off, fr);
        free(track_buf);
        return -1;
    }

    UINT br = 0;
    fr = f_read(fp, track_buf, MII_FLOPPY_MAX_TRACK_SIZE, &br);
    if (fr != FR_OK || br != MII_FLOPPY_MAX_TRACK_SIZE) {
        printf("%s: f_read track %d failed: fr=%d br=%u\n",
                         __func__, track_id, fr, br);
        free(track_buf);
        return -1;
    }

    mii_floppy_track_t *dst = &floppy->tracks[track_id];
    uint8_t *track_data = floppy->curr_track_data;
    mii_floppy_nib_render_track(track_buf, dst, track_data);

    if (dst->bit_count < 100) {
        printf("%s: invalid NIB track %d\n",
                         __func__, track_id);
        free(track_buf);
        return -1;
    }

    dst->dirty  = 0;
    dst->virgin = 0;

    free(track_buf);
    return 0;
}

static bool disk_woz_chunk_id_is(const mii_woz_chunk_t *chunk, const char id[4]) {
    return chunk && memcmp((const void *)&chunk->id_le, id, 4) == 0;
}

static int disk_load_floppy_woz_from_fatfs(mii_floppy_t *floppy, FIL *fp) {
	// Read header magic
	uint8_t magic[4];
	FRESULT fr = f_lseek(fp, 0);
	if (fr != FR_OK)
		return -1;
	UINT br = 0;
	fr = f_read(fp, magic, sizeof(magic), &br);
	if (fr != FR_OK || br != sizeof(magic))
		return -1;

	bool is_woz2 = (memcmp(magic, "WOZ2", 4) == 0);
	bool is_woz1 = (memcmp(magic, "WOZ", 3) == 0 && !is_woz2);
	if (!is_woz2 && !is_woz1) {
		printf("%s: not a WOZ file\n", __func__);
		return -1;
	}

	// Scan chunks (WOZ chunk ordering is not guaranteed)
	const uint32_t file_size = (uint32_t)f_size(fp);
	uint32_t tmap_payload_off = 0, tmap_payload_size = 0;
	uint32_t trks_payload_off = 0, trks_payload_size = 0;

	uint32_t off = (uint32_t)sizeof(mii_woz_header_t);
	mii_woz_chunk_t chunk;
	while (off + sizeof(chunk) <= file_size) {
		fr = f_lseek(fp, off);
		if (fr != FR_OK)
			return -1;
		br = 0;
		fr = f_read(fp, &chunk, sizeof(chunk), &br);
		if (fr != FR_OK || br != sizeof(chunk))
			return -1;
		const uint32_t size = le32toh(chunk.size_le);
		const uint32_t payload_off = off + (uint32_t)sizeof(chunk);
		if (payload_off + size > file_size)
			break;
		if (disk_woz_chunk_id_is(&chunk, "TMAP")) {
			tmap_payload_off = payload_off;
			tmap_payload_size = size;
		} else if (disk_woz_chunk_id_is(&chunk, "TRKS")) {
			trks_payload_off = payload_off;
			trks_payload_size = size;
		}
		off = payload_off + size;
	}

	if (!tmap_payload_off || !trks_payload_off) {
		printf("%s: missing required chunks (TMAP/TRKS)\n", __func__);
		return -1;
	}

	// Read TMAP
	uint8_t tmap_track_id[160];
	if (tmap_payload_size < sizeof(tmap_track_id)) {
        printf("%s: TMAP too small (%lu)\n", __func__, (unsigned long)tmap_payload_size);
		return -1;
	}
	fr = f_lseek(fp, tmap_payload_off);
	if (fr != FR_OK)
		return -1;
	br = 0;
	fr = f_read(fp, tmap_track_id, sizeof(tmap_track_id), &br);
	if (fr != FR_OK || br != sizeof(tmap_track_id))
		return -1;

	uint64_t used_tracks = 0;
	for (int ti = 0; ti < (int)sizeof(floppy->track_id) && ti < (int)sizeof(tmap_track_id); ti++) {
		uint8_t tid = tmap_track_id[ti];
		floppy->track_id[ti] = (tid == 0xff) ? MII_FLOPPY_NOISE_TRACK : tid;
		if (tid != 0xff && tid < 64)
			used_tracks |= (1ULL << tid);
	}

	// Load tracks from TRKS
	fr = f_lseek(fp, trks_payload_off);
	if (fr != FR_OK)
		return -1;

	if (is_woz2) {
		// Track entries (160)
		struct {
			uint16_t start_block_le;
			uint16_t block_count_le;
			uint32_t bit_count_le;
		} track[160];
		br = 0;
		fr = f_read(fp, track, sizeof(track), &br);
		if (fr != FR_OK || br != sizeof(track))
			return -1;
		for (int i = 0; i < MII_FLOPPY_TRACK_COUNT; i++) {
			if (!(used_tracks & (1ULL << i)))
				continue;
			const uint32_t bit_count = le32toh(track[i].bit_count_le);
			const uint32_t byte_count = (bit_count + 7) >> 3;
			const uint32_t start_byte = (uint32_t)(le16toh(track[i].start_block_le) << 9);
			if (byte_count > MII_FLOPPY_MAX_TRACK_SIZE) {
                printf("%s: WOZ2 track %d too large (%lu bytes)\n", __func__, i, (unsigned long)byte_count);
				return -1;
			}
			fr = f_lseek(fp, start_byte);
			if (fr != FR_OK)
				return -1;
			br = 0;
		    fr = f_read(fp, floppy->curr_track_data, byte_count, &br);
			if (fr != FR_OK || br != byte_count)
				return -1;
			floppy->tracks[i].virgin = 0;
			floppy->tracks[i].bit_count = bit_count;
		}
		return 2;
	} else {
		// WOZ1 TRKS payload is 35 fixed-size track entries (6656 bytes)
		for (int i = 0; i < 35 && i < MII_FLOPPY_TRACK_COUNT; i++) {
			uint8_t entry[6656];
			br = 0;
			fr = f_read(fp, entry, sizeof(entry), &br);
			if (fr != FR_OK || br != sizeof(entry))
				return -1;
			if (!(used_tracks & (1ULL << i)))
				continue;
			// Layout: bits[6646] then byte_count_le at offset 6646
			const uint16_t byte_count = disk_le16(entry + 6646);
			const uint16_t bit_count = disk_le16(entry + 6648);
			if (byte_count > MII_FLOPPY_MAX_TRACK_SIZE) {
				printf("%s: WOZ1 track %d too large (%u bytes)\n", __func__, i, byte_count);
				return -1;
			}
			floppy->tracks[i].virgin = 0;
		    memcpy(floppy->curr_track_data, entry, byte_count);
			floppy->tracks[i].bit_count = bit_count;
		}
		return 1;
	}
}

static int
disk_load_floppy_woz_track_from_fatfs(
    mii_floppy_t *floppy,
    FIL *fp,
    uint8_t track_id
) {
    printf("load track: %d\n", track_id);
    if (track_id >= MII_FLOPPY_TRACK_COUNT)
        return 0;

    // Read header
    uint8_t magic[4];
    FRESULT fr = f_lseek(fp, 0);
    if (fr != FR_OK)
        return -1;

    UINT br = 0;
    fr = f_read(fp, magic, sizeof(magic), &br);
    if (fr != FR_OK || br != sizeof(magic))
        return -1;

    bool is_woz2 = (memcmp(magic, "WOZ2", 4) == 0);
    bool is_woz1 = (memcmp(magic, "WOZ", 3) == 0 && !is_woz2);
    if (!is_woz1 && !is_woz2)
        return -1;

    // Scan chunks
    uint32_t off = sizeof(mii_woz_header_t);
    uint32_t tmap_off = 0, trks_off = 0;
    mii_woz_chunk_t chunk;

    const uint32_t file_size = (uint32_t)f_size(fp);

    while (off + sizeof(chunk) <= file_size) {
        fr = f_lseek(fp, off);
        if (fr != FR_OK)
            return -1;
        fr = f_read(fp, &chunk, sizeof(chunk), &br);
        if (fr != FR_OK || br != sizeof(chunk))
            return -1;

        uint32_t size = le32toh(chunk.size_le);
        uint32_t payload = off + sizeof(chunk);

        if (!memcmp(&chunk.id_le, "TMAP", 4))
            tmap_off = payload;
        else if (!memcmp(&chunk.id_le, "TRKS", 4))
            trks_off = payload;

        off = payload + size;
    }

    if (!tmap_off || !trks_off)
        return -1;

    // Read TMAP entry for this quarter-track
    uint8_t tmap[160];
    fr = f_lseek(fp, tmap_off);
    if (fr != FR_OK)
        return -1;
    fr = f_read(fp, tmap, sizeof(tmap), &br);
    if (fr != FR_OK || br != sizeof(tmap))
        return -1;

    uint8_t real_track = tmap[track_id * 4];
    if (real_track == 0xff)
        return 0;

    uint8_t *track_data = floppy->curr_track_data;

    if (is_woz2) {
        struct {
            uint16_t start_block_le;
            uint16_t block_count_le;
            uint32_t bit_count_le;
        } trk[160];

        fr = f_lseek(fp, trks_off);
        if (fr != FR_OK)
            goto fail;
        fr = f_read(fp, trk, sizeof(trk), &br);
        if (fr != FR_OK || br != sizeof(trk))
            goto fail;

        uint32_t bit_count = le32toh(trk[real_track].bit_count_le);
        uint32_t byte_count = (bit_count + 7) >> 3;
        uint32_t start =
            (uint32_t)(le16toh(trk[real_track].start_block_le) << 9);

        fr = f_lseek(fp, start);
        if (fr != FR_OK)
            goto fail;
        fr = f_read(fp, track_data, byte_count, &br);
        if (fr != FR_OK || br != byte_count)
            goto fail;

        floppy->tracks[real_track].bit_count = bit_count;
    } else {
        uint32_t entry_off =
            trks_off + real_track * MII_FLOPPY_MAX_TRACK_SIZE;

        uint8_t entry[MII_FLOPPY_MAX_TRACK_SIZE];
        fr = f_lseek(fp, entry_off);
        if (fr != FR_OK)
            goto fail;
        fr = f_read(fp, entry, sizeof(entry), &br);
        if (fr != FR_OK || br != sizeof(entry))
            goto fail;

        uint16_t byte_count = disk_le16(entry + 6646);
        uint16_t bit_count  = disk_le16(entry + 6648);

        memcpy(track_data, entry, byte_count);
        floppy->tracks[real_track].bit_count = bit_count;
    }

    floppy->tracks[real_track].virgin = 0;
    floppy->tracks[real_track].dirty  = 0;
    return 0;

fail:
    return -1;
}

void logMsg(char* msg) {
    static FIL fileD;
    f_open(&fileD, "/apple.log", FA_WRITE | FA_OPEN_APPEND);
    UINT bw;
    f_write(&fileD, msg, strlen(msg), &bw);
    f_close(&fileD);
}

// Get disk type from filename extension
disk_type_t disk_get_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return DISK_TYPE_UNKNOWN;
    
    // Convert extension to lowercase for comparison
    char ext[8];
    int i;
    for (i = 0; i < 7 && dot[i+1]; i++) {
        ext[i] = tolower((unsigned char)dot[i+1]);
    }
    ext[i] = '\0';
    
    if (strcmp(ext, "dsk") == 0 || strcmp(ext, "do") == 0 || strcmp(ext, "po") == 0) {
        return DISK_TYPE_DSK;
    } else if (strcmp(ext, "nib") == 0) {
        return DISK_TYPE_NIB;
    } else if (strcmp(ext, "woz") == 0) {
        return DISK_TYPE_WOZ;
    }
    
    return DISK_TYPE_UNKNOWN;
}

// Initialize SD card
int disk_loader_init(void) {
    printf("Initializing SD card...\n");
    
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD card mount failed: %d\n", fr);
        return -1;
    }
    f_mkdir("/tmp");
    f_mkdir("/apple");
    /// TODO: is log
    f_unlink("/apple.log");
    
    sd_mounted = true;
    printf("SD card mounted successfully\n");
    
    // Scan for disk images
    int count = disk_scan_directory();
    printf("Found %d disk images\n", count);
    
    return 0;
}

// Scan /apple directory for disk images
int disk_scan_directory(void) {
    if (!sd_mounted) {
        printf("SD card not mounted\n");
        return 0;
    }
    
    DIR dir;
    FILINFO fno;
    
    g_disk_count = 0;
    
    // First try /apple directory
    FRESULT fr = f_opendir(&dir, "/apple");
    if (fr != FR_OK) {
        // Try root directory
        printf("/apple not found, checking root directory\n");
        fr = f_opendir(&dir, "/");
        if (fr != FR_OK) {
            printf("Failed to open directory: %d\n", fr);
            return 0;
        }
    } else {
        printf("Scanning /apple directory...\n");
    }
    
    while (g_disk_count < MAX_DISK_IMAGES) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        
        // Skip directories
        if (fno.fattrib & AM_DIR) continue;
        
        // Check if it's a disk image
        disk_type_t type = disk_get_type(fno.fname);
        if (type == DISK_TYPE_UNKNOWN) continue;
        
        // Add to list
        strncpy(g_disk_list[g_disk_count].filename, fno.fname, MAX_FILENAME_LEN - 1);
        g_disk_list[g_disk_count].filename[MAX_FILENAME_LEN - 1] = '\0';
        g_disk_list[g_disk_count].size = fno.fsize;
        g_disk_list[g_disk_count].type = type;
        
        printf("  [%d] %s (%lu bytes, type %d)\n", 
               g_disk_count, fno.fname, fno.fsize, type);
        
        g_disk_count++;
    }
    
    f_closedir(&dir);
    return g_disk_count;
}

// Select a disk image for a drive (image is read from SD on mount)
int disk_load_image(int drive, int index) {
    if (drive < 0 || drive > 1) {
        printf("Invalid drive: %d\n", drive);
        return -1;
    }
    if (index < 0 || index >= g_disk_count) {
        printf("Invalid disk index: %d\n", index);
        return -1;
    }
    
    disk_entry_t *entry = &g_disk_list[index];
    loaded_disk_t *disk = &g_loaded_disks[drive];

    // Clear previous selection
    memset(disk, 0, sizeof(*disk));

    // Validate the file exists by opening it, then close immediately.
    FIL fp;
    char path[128];
    if (!disk_open_image_file(entry->filename, &fp, path, sizeof(path))) {
        printf("Failed to open image for %s\n", entry->filename);
        return -1;
    }
    f_close(&fp);

    // Update selected disk info
    disk->size = entry->size;
    disk->type = entry->type;
    strncpy(disk->filename, entry->filename, MAX_FILENAME_LEN - 1);
    disk->filename[MAX_FILENAME_LEN - 1] = '\0';
    disk->loaded = true;
    disk->write_back = false;

    printf("Selected %s for drive %d (%lu bytes)\n", entry->filename, drive + 1, (unsigned long)entry->size);
    
    return 0;
}

// Unload a disk image
void disk_unload_image(int drive) {
    if (drive < 0 || drive > 1) return;
    
    loaded_disk_t *disk = &g_loaded_disks[drive];
    
    if (!disk->loaded) return;

    memset(disk, 0, sizeof(*disk));
    
    printf("Unloaded drive %d\n", drive + 1);
}

// Write back modified disk image to SD card
int disk_writeback(int drive) {
    if (drive < 0 || drive > 1) return -1;
    printf("%s: writeback not supported in this loader\n", __func__);
    return -1;
}

// Convert our disk_type_t to mii_dd format enum
static uint8_t disk_type_to_mii_format(disk_type_t type, const char *filename) {
    switch (type) {
        case DISK_TYPE_DSK: {
            // Check if it's a .do or .po file
            const char *dot = strrchr(filename, '.');
            if (dot) {
                if (strcasecmp(dot, ".po") == 0) return MII_DD_FILE_PO;
                if (strcasecmp(dot, ".do") == 0) return MII_DD_FILE_DO;
            }
            return MII_DD_FILE_DSK;
        }
        case DISK_TYPE_NIB:
            return MII_DD_FILE_NIB;
        case DISK_TYPE_WOZ:
            return MII_DD_FILE_WOZ;
        default:
            return MII_DD_FILE_DSK;
    }
}

// Static mii_dd_file_t structures for the two drives
static mii_dd_file_t g_dd_files[2] = {0};
// reduce stack usage, by global variables
static FIL fp;
static char path[128];

// Mount a loaded disk image to the emulator
// preserve_state: if true, keeps motor/head position for disk swap during game
int disk_mount_to_emulator(int drive, mii_t *mii, int slot, int preserve_state) {
    if (drive < 0 || drive > 1) {
        printf("Invalid drive: %d\n", drive);
        return -1;
    }
    
    loaded_disk_t *disk = &g_loaded_disks[drive];
    if (!disk->loaded || !disk->filename[0]) {
        printf("No disk loaded in drive %d\n", drive + 1);
        return -1;
    }
    
    // Get the floppy structures from the disk2 card
    mii_floppy_t *floppies[2] = {NULL, NULL};
    int res = mii_slot_command(mii, slot, MII_SLOT_D2_GET_FLOPPY, floppies);
    if (res < 0 || !floppies[drive]) {
        printf("Failed to get floppy structure for drive %d (slot %d)\n", drive + 1, slot);
        return -1;
    }
    
    mii_floppy_t *floppy = floppies[drive];
    mii_dd_file_t *file = &g_dd_files[drive];
    
    // Set up the mii_dd_file_t structure (no file->map backing on RP2350)
    memset(file, 0, sizeof(*file));
    file->pathname = disk->filename;  // Just point to our filename
    file->format = disk_type_to_mii_format(disk->type, disk->filename);
    file->read_only = 1;  // Read-only: no in-memory backing for writes
    file->size = disk->size;
    
    printf("Mounting %s to drive %d (format=%d, size=%lu, preserve=%d)\n",
           disk->filename, drive + 1, file->format, (unsigned long)file->size, preserve_state);

    // Open the image on SD
    if (!disk_open_image_file(disk->filename, &fp, path, sizeof(path))) {
        printf("Failed to open disk image %s\n", disk->filename);
        return -1;
    }
    printf("Reading disk from SD: %s\n", path);
    
    // Save drive state if we need to preserve it (for INSERT mode)
    uint8_t saved_motor = floppy->motor;
    uint8_t saved_stepper = floppy->stepper;
    uint8_t saved_qtrack = floppy->qtrack;
    uint32_t saved_bit_position = floppy->bit_position;
    
    // Initialize the floppy (clears all tracks)
    mii_floppy_init(floppy);
    
    // Restore drive state if preserving (INSERT mode)
    if (preserve_state) {
        floppy->motor = saved_motor;
        floppy->stepper = saved_stepper;
        floppy->qtrack = saved_qtrack;
        floppy->bit_position = saved_bit_position;
        printf("Preserved drive state: motor=%d qtrack=%d bit_pos=%lu\n",
               saved_motor, saved_qtrack, (unsigned long)saved_bit_position);
    }
    // we shoul load selected track as last operation, to make floppy->curr_track_data persistent
    uint8_t track_id = floppy->track_id[floppy->qtrack];

    // Load the disk image into the floppy structure
    res = -1;
    switch (file->format) {
        case MII_DD_FILE_DSK:
        case MII_DD_FILE_DO:
        case MII_DD_FILE_PO:
            res = disk_load_floppy_dsk_from_fatfs(floppy, file, &fp);
            if (res >= 0)
                res = disk_load_floppy_dsk_track_from_fatfs(floppy, file, &fp, track_id);
            break;
        case MII_DD_FILE_NIB:
            res = disk_load_floppy_nib_from_fatfs(floppy, &fp);
            if (res >= 0)
                res = disk_load_floppy_nib_track_from_fatfs(floppy, &fp, track_id);
            break;
        case MII_DD_FILE_WOZ:
            res = disk_load_floppy_woz_from_fatfs(floppy, &fp);
            if (res >= 0)
                res = disk_load_floppy_woz_track_from_fatfs(floppy, &fp, track_id);
            break;
        default:
            printf("%s: unsupported format %d\n", __func__, file->format);
            res = -1;
            break;
    }
    f_close(&fp);

    if (res < 0) {
        printf("Failed to load disk image to floppy: %d\n", res);
        return -1;
    }
    
    // Enable the boot signature so the slot is now bootable
    int enable = 1;
    mii_slot_command(mii, slot, MII_SLOT_D2_SET_BOOT, &enable);
    
    // Reset VBL timer after disk loading - the long SD card read
    // may have caused the timer to accumulate negative cycles
    mii_video_reset_vbl_timer(mii);
    
    printf("Disk %s mounted successfully to drive %d\n", disk->filename, drive + 1);
    return 0;
}

extern int g_disk2_slot; // slot for Disk II

void disk_reload_track(uint8_t drive, uint8_t track_id, mii_t* mii) {
    loaded_disk_t *disk = &g_loaded_disks[drive];
    if (!disk->loaded || !disk->filename[0]) {
        printf("No disk loaded in drive %d\n", drive + 1);
        return;
    }
    // Get the floppy structures from the disk2 card
    mii_floppy_t *floppies[2] = {NULL, NULL};
    int res = mii_slot_command(mii, g_disk2_slot, MII_SLOT_D2_GET_FLOPPY, floppies);
    if (res < 0 || !floppies[drive]) {
        printf("Failed to get floppy structure for drive %d (slot %d)\n", drive + 1, g_disk2_slot);
        return;
    }
    if (!disk_open_image_file(disk->filename, &fp, path, sizeof(path))) {
        printf("Failed to open disk image %s\n", disk->filename);
        return;
    }
    mii_floppy_t *floppy = floppies[drive];
    mii_dd_file_t *file = &g_dd_files[drive];
    res = -1;
    switch (file->format) {
        case MII_DD_FILE_DSK:
        case MII_DD_FILE_DO:
        case MII_DD_FILE_PO:
            res = disk_load_floppy_dsk_track_from_fatfs(floppy, file, &fp, track_id);
            break;
        case MII_DD_FILE_NIB:
            res = disk_load_floppy_nib_track_from_fatfs(floppy, &fp, track_id);
            break;
        case MII_DD_FILE_WOZ:
            res = disk_load_floppy_woz_track_from_fatfs(floppy, &fp, track_id);
            break;
        default:
            printf("%s: unsupported format %d\n", __func__, file->format);
            res = -1;
            break;
    }
    f_close(&fp);

    if (res < 0) {
        printf("Failed to load disk image track %d to floppy: %d\n", track_id, res);
    }
    // Reset VBL timer after disk loading - the long SD card read
    // may have caused the timer to accumulate negative cycles
    mii_video_reset_vbl_timer(mii);
}

// Eject a disk from the emulator
void disk_eject_from_emulator(int drive, mii_t *mii, int slot) {
    if (drive < 0 || drive > 1) return;
    
    // Get the floppy structures from the disk2 card
    mii_floppy_t *floppies[2] = {NULL, NULL};
    int res = mii_slot_command(mii, slot, MII_SLOT_D2_GET_FLOPPY, floppies);
    if (res < 0 || !floppies[drive]) {
        printf("Failed to get floppy structure for drive %d\n", drive + 1);
        return;
    }
    
    // Re-initialize the floppy (clears all data, makes it "empty")
    mii_floppy_init(floppies[drive]);
    
    // Clear the static file structure
    memset(&g_dd_files[drive], 0, sizeof(g_dd_files[drive]));
    
    printf("Drive %d ejected\n", drive + 1);
}
