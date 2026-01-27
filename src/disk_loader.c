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
#include "../drivers/psram_allocator.h"

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
extern uint8_t vram[2 * RAM_PAGES_PER_POOL * RAM_PAGE_SIZE];
#define MAX_DISK_IMAGES (sizeof(vram) / sizeof(disk_entry_t))
disk_entry_t* g_disk_list = (disk_entry_t*)vram;//[MAX_DISK_IMAGES];

#if PICO_RP2350
uint8_t drive0_cache[BDSK_BYTES];
#endif

int g_disk_count = 0;
loaded_disk_t g_loaded_disks[2] = {0};

// Static mii_dd_file_t structures for the two drives
static mii_dd_file_t g_dd_files[2] = {0};

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

// reduce stack usage, by global variables
FIL fp;
char path[256];
char selected_dir[128] __scratch_y("selected_dir") = "/apple";

static bool disk_open_original_image_file(const char *filename, FIL *out_fp, char *out_path, size_t out_path_len) {
    if (!sd_mounted)
        return false;
    if (!filename || !out_fp)
        return false;

    snprintf(path, sizeof(path), "%s/%s", selected_dir, filename);
    FRESULT fr = f_open(out_fp, path, FA_READ);
    if (fr != FR_OK) {
        return false;
    }
    if (out_path && out_path_len) {
        strncpy(out_path, path, out_path_len - 1);
        out_path[out_path_len - 1] = '\0';
    }
    return true;
}

static bool disk_open_bdsk_image_file(FIL *out_fp, const char *filename, char *out_path, size_t out_path_len) {
    if (!sd_mounted)
        return false;
    if (!filename || !out_fp)
        return false;

    const char *dot = strrchr(filename, '.');
    bool is_bdsk = (dot && strcasecmp(dot, ".bdsk") == 0);

    if (is_bdsk) {
        snprintf(path, sizeof(path), "%s/%s", selected_dir, filename);
    } else {
        snprintf(path, sizeof(path), "%s/%s.bdsk", selected_dir, filename);
    }
    
    FRESULT fr = f_open(out_fp, path, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
    if (fr != FR_OK) {
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

static int
disk_dump_current_track(
    int drive,
    int track_id,
    mii_floppy_t *floppy,
    mii_dd_file_t *file,
    FIL *target
) {
    if (track_id < 0 || track_id >= DSK_TRACKS)
        return -1;

    mii_floppy_track_t *src = &floppy->tracks[track_id];

    if (src->bit_count == 0 || src->bit_count > BDSK_MAX_BITS)
        return -1;

    /* --- write header once (track 0 is enough) --- */
    if (track_id == 0) {
        bdsk_header_t hdr;
        memcpy(hdr.magic, BDSK_MAGIC, 4);
        hdr.version = BDSK_VERSION;
        hdr.tracks  = DSK_TRACKS;

        FRESULT fr = f_lseek(target, 0);
        if (fr != FR_OK)
            return -1;

        UINT bw;
        fr = f_write(target, &hdr, sizeof(hdr), &bw);
        if (fr != FR_OK || bw != sizeof(hdr))
            return -1;
    }

    /* --- compute track offset --- */
    const uint32_t track_offset =
        sizeof(bdsk_header_t) +
        track_id * (sizeof(bdsk_track_desc_t) + BDSK_TRACK_DATA_SIZE);

    bdsk_track_desc_t desc;
    desc.bit_count = src->bit_count;

    /* --- write descriptor --- */
    FRESULT fr = f_lseek(target, track_offset);
    if (fr != FR_OK)
        return -1;

    UINT bw = 0;
    fr = f_write(target, &desc, sizeof(desc), &bw);
    if (fr != FR_OK || bw != sizeof(desc))
        return -1;

    /* --- write track data --- */
    fr = f_write(
        target,
        floppy->curr_track_data,
        BDSK_TRACK_DATA_SIZE,
        &bw
    );
    if (fr != FR_OK || bw != BDSK_TRACK_DATA_SIZE)
        return -1;

#if PICO_RP2350
    if (!drive) { // drive #0
        memcpy(drive0_cache + track_offset, &desc, sizeof(desc));
        memcpy(drive0_cache + track_offset + sizeof(desc), floppy->curr_track_data, BDSK_TRACK_DATA_SIZE);
    } else if (butter_psram_size()) { // drive #1
        memcpy(PSRAM_DATA + track_offset, &desc, sizeof(desc));
        memcpy(PSRAM_DATA + track_offset + sizeof(desc), floppy->curr_track_data, BDSK_TRACK_DATA_SIZE);
    }
#endif

    return 0;
}

static int disk_load_floppy_dsk_from_fatfs(int drive, mii_floppy_t *floppy, mii_dd_file_t *file, FIL *fp) {
    FIL target;
    if (!disk_open_bdsk_image_file(&target, file->pathname, path, sizeof(path))) {
        return -1;
    }

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

        for (int phys_sector = 0; phys_sector < DSK_SECTORS; phys_sector++) {
            uint8_t sector_buf[DSK_SECTOR_SIZE];
            const uint8_t dos_sector = secmap[phys_sector];
            const uint32_t off = (uint32_t)((DSK_SECTORS * track + dos_sector) * DSK_SECTOR_SIZE);

            FRESULT fr = f_lseek(fp, off);
            if (fr != FR_OK) {
                printf("%s: f_lseek(%lu) failed: %d\n", __func__, (unsigned long)off, fr);
                goto fail;
            }
            UINT br = 0;
            fr = f_read(fp, sector_buf, sizeof(sector_buf), &br);
            if (fr != FR_OK || br != sizeof(sector_buf)) {
                printf("%s: f_read sector failed: fr=%d br=%u\n", __func__, fr, br);
                goto fail;
            }

            // Volume number is 254, as in mii_dsk.c
            mii_floppy_dsk_render_sector(254, (uint8_t)track, (uint8_t)phys_sector, sector_buf, dst, track_data);
        }
        if (disk_dump_current_track(drive, track, floppy, file, &target) < 0)
            goto fail;
    }
    f_close(&target);
    return 0;
fail:
    f_close(&target);
    return -1;
}

static int disk_load_floppy_nib_from_fatfs(int drive, mii_floppy_t *floppy, mii_dd_file_t *file, FIL *fp) {
    FIL target;
    if (!disk_open_bdsk_image_file(&target, file->pathname, path, sizeof(path))) {
        return -1;
    }

    for (int track = 0; track < 35; track++) {
        const uint32_t off = (uint32_t)(track * MII_FLOPPY_MAX_TRACK_SIZE);
        FRESULT fr = f_lseek(fp, off);
        if (fr != FR_OK) {
            printf("%s: f_lseek(%lu) failed: %d\n", __func__, (unsigned long)off, fr);
            goto fail;
        }
        UINT br = 0;
        fr = f_read(fp, track_buf, MII_FLOPPY_MAX_TRACK_SIZE, &br);
        if (fr != FR_OK || br != MII_FLOPPY_MAX_TRACK_SIZE) {
            printf("%s: f_read track failed: fr=%d br=%u\n", __func__, fr, br);
            goto fail;
        }
        uint8_t* track_data = floppy->curr_track_data;
        mii_floppy_nib_render_track(track_buf, &floppy->tracks[track], track_data);
        if (floppy->tracks[track].bit_count < 100) {
            printf("%s: invalid NIB track %d\n", __func__, track);
            goto fail;
        }
        floppy->tracks[track].dirty = 0;
        if (disk_dump_current_track(drive, track, floppy, file, &target) < 0)
            goto fail;
    }
    f_close(&target);
    return 0;
fail:
    f_close(&target);
    return -1;
}

static bool disk_woz_chunk_id_is(const mii_woz_chunk_t *chunk, const char id[4]) {
    return chunk && memcmp((const void *)&chunk->id_le, id, 4) == 0;
}

static int disk_load_floppy_woz_from_fatfs(int drive, mii_floppy_t *floppy, mii_dd_file_t *file, FIL *fp) {
    FIL target;
    if (!disk_open_bdsk_image_file(&target, file->pathname, path, sizeof(path))) {
        return -1;
    }

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
    	goto fail;
	}

	// Read TMAP
	uint8_t tmap_track_id[160];
	if (tmap_payload_size < sizeof(tmap_track_id)) {
        printf("%s: TMAP too small (%lu)\n", __func__, (unsigned long)tmap_payload_size);
    	goto fail;
	}
	fr = f_lseek(fp, tmap_payload_off);
	if (fr != FR_OK)
    	goto fail;
	br = 0;
	fr = f_read(fp, tmap_track_id, sizeof(tmap_track_id), &br);
	if (fr != FR_OK || br != sizeof(tmap_track_id))
    	goto fail;

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
    	goto fail;

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
        	goto fail;
		for (int i = 0; i < MII_FLOPPY_TRACK_COUNT; i++) {
			if (!(used_tracks & (1ULL << i)))
				continue;
			const uint32_t bit_count = le32toh(track[i].bit_count_le);
			const uint32_t byte_count = (bit_count + 7) >> 3;
			const uint32_t start_byte = (uint32_t)(le16toh(track[i].start_block_le) << 9);
			if (byte_count > MII_FLOPPY_MAX_TRACK_SIZE) {
                printf("%s: WOZ2 track %d too large (%lu bytes)\n", __func__, i, (unsigned long)byte_count);
				goto fail;
			}
			fr = f_lseek(fp, start_byte);
			if (fr != FR_OK)
				goto fail;
			br = 0;
		    fr = f_read(fp, floppy->curr_track_data, byte_count, &br);
			if (fr != FR_OK || br != byte_count)
				goto fail;
			floppy->tracks[i].virgin = 0;
			floppy->tracks[i].bit_count = bit_count;
            if (disk_dump_current_track(drive, i, floppy, file, &target) < 0)
                goto fail;
		}
        f_close(&target);
		return 2;
	}
    // WOZ1 TRKS payload is 35 fixed-size track entries (6656 bytes)
    for (int i = 0; i < 35 && i < MII_FLOPPY_TRACK_COUNT; i++) {
        uint8_t entry[6656];
        br = 0;
        fr = f_read(fp, entry, sizeof(entry), &br);
        if (fr != FR_OK || br != sizeof(entry))
            goto fail;
        if (!(used_tracks & (1ULL << i)))
            continue;
        // Layout: bits[6646] then byte_count_le at offset 6646
        const uint16_t byte_count = disk_le16(entry + 6646);
        const uint16_t bit_count = disk_le16(entry + 6648);
        if (byte_count > MII_FLOPPY_MAX_TRACK_SIZE) {
            printf("%s: WOZ1 track %d too large (%u bytes)\n", __func__, i, byte_count);
            goto fail;
        }
        floppy->tracks[i].virgin = 0;
        memcpy(floppy->curr_track_data, entry, byte_count);
        floppy->tracks[i].bit_count = bit_count;
        if (disk_dump_current_track(drive, i, floppy, file, &target) < 0)
            goto fail;
    }
    f_close(&target);
    return 1;
fail:
    f_close(&target);
    return -1;
}

static int
disk_load_floppy_bdsk_track_from_fatfs(
    int drive,
    mii_floppy_t   *floppy,
    mii_dd_file_t  *file,
    FIL            *fp,
    uint8_t         track_id
) {
    if (track_id >= DSK_TRACKS)
        return -1;
    
    /* --- compute track offset --- */
    uint32_t track_offset =
        sizeof(bdsk_header_t) +
        track_id * (sizeof(bdsk_track_desc_t) + BDSK_TRACK_DATA_SIZE);

    /* --- read descriptor --- */
    bdsk_track_desc_t desc;
#if PICO_RP2350
    if (!drive) { // drive #0
        memcpy(&desc, drive0_cache + track_offset, sizeof(bdsk_track_desc_t));
        memcpy(floppy->curr_track_data, drive0_cache + track_offset + sizeof(bdsk_track_desc_t), BDSK_TRACK_DATA_SIZE);
        goto ok;
    }
    if (butter_psram_size()) { // drive #1
        memcpy(&desc, PSRAM_DATA + track_offset, sizeof(bdsk_track_desc_t));
        memcpy(floppy->curr_track_data, PSRAM_DATA + track_offset + sizeof(bdsk_track_desc_t), BDSK_TRACK_DATA_SIZE);
        goto ok;
    }
#endif
    FRESULT fr = f_lseek(fp, track_offset);
    if (fr != FR_OK)
        return -1;

    UINT br;
    fr = f_read(fp, &desc, sizeof(desc), &br);
    if (fr != FR_OK || br != sizeof(desc))
        return -1;

    /* --- read track data --- */
    fr = f_read(
        fp,
        floppy->curr_track_data,
        BDSK_TRACK_DATA_SIZE,
        &br
    );
    if (fr != FR_OK || br != BDSK_TRACK_DATA_SIZE)
        return -1;
ok:
    if (desc.bit_count == 0 || desc.bit_count > BDSK_MAX_BITS)
        return -1;
    /* --- update floppy state --- */
    mii_floppy_track_t *dst = &floppy->tracks[track_id];
    dst->bit_count = desc.bit_count;
    dst->virgin    = 0;
    dst->dirty     = 0;

    return 0;
}

static int disk_load_floppy_bdsk_from_fatfs(int drive, mii_floppy_t *floppy, mii_dd_file_t *file, FIL *fp) {
    /* --- read and validate header --- */
    bdsk_header_t hdr;

    FRESULT fr = f_lseek(fp, 0);
    if (fr != FR_OK)
        return -1;

    UINT br;
#if PICO_RP2350
    if (!drive) { // drive #0
        fr = f_read(fp, drive0_cache, sizeof(drive0_cache), &br);
        if (fr != FR_OK || br != sizeof(drive0_cache))
            return -1;
        memcpy(&hdr, drive0_cache, sizeof hdr);
        goto ok;
    }
    if (butter_psram_size()) { // drive #1
        fr = f_read(fp, PSRAM_DATA, sizeof(drive0_cache), &br);
        if (fr != FR_OK || br != sizeof(drive0_cache))
            return -1;
        memcpy(&hdr, PSRAM_DATA, sizeof hdr);
        goto ok;
    }
#endif

    fr = f_read(fp, &hdr, sizeof(hdr), &br);
    if (fr != FR_OK || br != sizeof(hdr))
        return -1;
ok:
    if (memcmp(hdr.magic, BDSK_MAGIC, 4) != 0)
        return -1;

    if (hdr.version != BDSK_VERSION || hdr.tracks != BDSK_TRACKS)
        return -1;

    // all tracks validation loading
    for (int track = 0; track < hdr.tracks; track++) {
        if (disk_load_floppy_bdsk_track_from_fatfs(drive, floppy, file, fp, track) < 0) {
            return -1;
        }
    }
    return 0;
}

bool disk_bdsk_exists2(const char *filename) {
    FILINFO fno;
    const char *dot = strrchr(filename, '.');
    bool is_bdsk = (dot && strcasecmp(dot, ".bdsk") == 0);
    if (is_bdsk) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s.bdsk", selected_dir, filename);
    return f_stat(path, &fno) == FR_OK;
}

static bool disk_bdsk_exists(const char *filename) {
    FILINFO fno;
    const char *dot = strrchr(filename, '.');
    bool is_bdsk = (dot && strcasecmp(dot, ".bdsk") == 0);

    if (is_bdsk) {
        snprintf(path, sizeof(path), "%s/%s", selected_dir, filename);
    } else {
        snprintf(path, sizeof(path), "%s/%s.bdsk", selected_dir, filename);
    }

    return f_stat(path, &fno) == FR_OK;
}

#if HACK_DEBUG
void logMsg(char* msg) {
    static FIL fileD;
    f_open(&fileD, "/apple.log", FA_WRITE | FA_OPEN_APPEND);
    UINT bw;
    f_write(&fileD, msg, strlen(msg), &bw);
    f_close(&fileD);
}
#endif

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
    } else if (strcmp(ext, "bdsk") == 0) {
        return DISK_TYPE_BDSK;
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
    return 0;
}

static int disk_entry_cmp_name(const void *a, const void *b) {
    const disk_entry_t *da = (const disk_entry_t *)a;
    const disk_entry_t *db = (const disk_entry_t *)b;

    // 1. Directories first
    if (da->type == DIR_TYPE && db->type != DIR_TYPE) return -1;
    if (da->type != DIR_TYPE && db->type == DIR_TYPE) return  1;

    // 2. Same type -> sort by name
    return strcmp(da->filename, db->filename);
}

static const char *
disk_select_name(const FILINFO *fno)
{
    size_t fname_len = strlen(fno->fname);
#if FF_USE_LFN
    if (fname_len < (MAX_FILENAME_LEN - 5)) { // for ".bdsk" space
        return fno->fname;
    }
    return fno->altname;
    /* fallback: LFN will be truncated by caller */
    return fno->fname;
#else
    /* No LFN at all */
    return fno->fname;
#endif
}

// Scan directory for disk images and directories
int disk_scan_directory(const char* __restrict path) {
    if (!sd_mounted) {
        printf("SD card not mounted\n");
        return 0;
    }
    
    DIR dir;
    FILINFO fno;
    
    g_disk_count = 0;

    int wa_mark = 1;
    
    // First try /apple directory
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        // Try root directory (temporary W/A)
        printf("%s not found, checking root directory\n", path);
        fr = f_opendir(&dir, "/");
        if (fr != FR_OK) {
            printf("Failed to open directory: %d\n", fr);
            return 0;
        }
        wa_mark = -1; // return negative result, to mark directory was replaced by root
    } else {
        printf("Scanning %s directory...\n", path);
    }
    
    while (g_disk_count < MAX_DISK_IMAGES) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;

        disk_type_t type;
        if (fno.fattrib & AM_DIR) type = DIR_TYPE;
        else type = disk_get_type(fno.fname);

        if (type == DISK_TYPE_UNKNOWN) continue;
        
        // Add to list
        strncpy(g_disk_list[g_disk_count].filename, disk_select_name(&fno), MAX_FILENAME_LEN - 1);
        g_disk_list[g_disk_count].filename[MAX_FILENAME_LEN - 1] = '\0';
        g_disk_list[g_disk_count].size = fno.fsize;
        g_disk_list[g_disk_count].type = type;
        g_disk_count++;
    }
    
    f_closedir(&dir);


    if (g_disk_count > 1) {
        qsort(
            g_disk_list,
            g_disk_count,
            sizeof(disk_entry_t),
            disk_entry_cmp_name
        );
    }    
    return wa_mark * g_disk_count;
}

// Select a disk image for a drive (image is read from SD on mount)
int disk_load_image(int drive, int index, bool write) {
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
    if (!disk_open_original_image_file(entry->filename, &fp, path, sizeof(path))) {
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
    disk->write_back = write;

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
        case DISK_TYPE_BDSK:
            return MII_DD_FILE_BDSK;
        default:
            return MII_DD_FILE_DSK;
    }
}

void disk_write_track(uint8_t drive, uint8_t track_id, mii_t* mii);

// Mount a loaded disk image to the emulator
// preserve_state: if true, keeps motor/head position for disk swap during game
int disk_mount_to_emulator(int drive, mii_t *mii, int slot, int preserve_state, bool read_only, bool bdsk_recreate) {
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

    /* flush previous disk track before replacing it */
    uint8_t old_track = floppy->track_id[floppy->qtrack];
    if (!floppy->write_protected &&
        old_track < MII_FLOPPY_TRACK_COUNT &&
        floppy->tracks[old_track].dirty)
    {
        disk_write_track(drive, old_track, mii);
    }
    
    // Set up the mii_dd_file_t structure (no file->map backing on RP2350)
    memset(file, 0, sizeof(*file));
    strncpy(file->pathname, disk->filename, sizeof(file->pathname));  // Just point to our filename
    file->format = disk_type_to_mii_format(disk->type, disk->filename);
    file->read_only = read_only;  // Read-only: no in-memory backing for writes
    file->size = disk->size;
    
    printf("Mounting %s to drive %d (format=%d, size=%lu, preserve=%d)\n",
           disk->filename, drive + 1, file->format, (unsigned long)file->size, preserve_state);

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
    if (bdsk_recreate || !disk_bdsk_exists(file->pathname)) {
        // Open the image on SD
        if (!disk_open_original_image_file(disk->filename, &fp, path, sizeof(path))) {
            printf("Failed to open disk image %s\n", disk->filename);
            return -1;
        }
        switch (file->format) {
            case MII_DD_FILE_DSK:
            case MII_DD_FILE_DO:
            case MII_DD_FILE_PO:
                res = disk_load_floppy_dsk_from_fatfs(drive,floppy, file, &fp);
                break;
            case MII_DD_FILE_NIB:
                res = disk_load_floppy_nib_from_fatfs(drive, floppy, file, &fp);
                break;
            case MII_DD_FILE_WOZ:
                res = disk_load_floppy_woz_from_fatfs(drive, floppy, file, &fp);
                break;
            case MII_DD_FILE_BDSK:
                res = disk_load_floppy_bdsk_from_fatfs(drive, floppy, file, &fp);
                break;
            default:
                printf("%s: unsupported format %d\n", __func__, file->format);
                res = -1;
                break;
        }
    } else {
        // bdsk есть → НЕ КОНВЕРТИРУЕМ
        if (!disk_open_bdsk_image_file(&fp, file->pathname, path, sizeof(path)))
            return -1;
        res = disk_load_floppy_bdsk_from_fatfs(drive, floppy, file, &fp);
    }
    f_close(&fp);
    if (res >= 0) {
        if (!disk_open_bdsk_image_file(&fp, file->pathname, path, sizeof(path))) {
            return -1;
        }
        res = disk_load_floppy_bdsk_track_from_fatfs(drive, floppy, file, &fp, track_id);
        f_close(&fp);
    }

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
    if (!disk_open_bdsk_image_file(&fp, disk->filename, path, sizeof(path))) {
        printf("Failed to open disk image %s\n", disk->filename);
        return;
    }
    mii_floppy_t *floppy = floppies[drive];
    mii_dd_file_t *file = &g_dd_files[drive];
    res = disk_load_floppy_bdsk_track_from_fatfs(drive, floppy, file, &fp, track_id);
    f_close(&fp);

    if (res < 0) {
        printf("Failed to load disk image track %d to floppy: %d\n", track_id, res);
    }
}

static int
disk_write_floppy_bdsk_track_to_fatfs(
    int drive,
    mii_floppy_t   *floppy,
    mii_dd_file_t  *file,
    FIL            *fp,
    uint8_t         track_id
) {
    if (track_id >= DSK_TRACKS)
        return 0;

    mii_floppy_track_t *src = &floppy->tracks[track_id];

    if (!src->dirty)
        return 0;

    if (disk_dump_current_track(drive, track_id, floppy, file, fp) < 0)
        return -1;

    if (f_sync(fp) != FR_OK)
        return -1;

    src->dirty = 0;
    floppy->seed_saved = floppy->seed_dirty;
    return 0;
}

static int
disk_write_floppy_nib_track_to_fatfs(
    mii_floppy_t *floppy,
    FIL *fp,
    uint8_t track_id
) {
    /// unsupported for now
    return 0;
}

static int
disk_write_floppy_woz_track_to_fatfs(
    mii_floppy_t *floppy,
    FIL *fp,
    uint8_t track_id
) {
    /// unsupported for now
    return 0;
}

void disk_write_track(uint8_t drive, uint8_t track_id, mii_t* mii) {
    loaded_disk_t *disk = &g_loaded_disks[drive];
    if (!disk->loaded || !disk->filename[0]) {
        printf("No disk loaded in drive %d\n", drive + 1);
        return;
    }
    if (!disk->write_back) {
        printf("RO disk in drive %d\n", drive + 1);
        return;
    }
    // Get the floppy structures from the disk2 card
    mii_floppy_t *floppies[2] = {NULL, NULL};
    int res = mii_slot_command(mii, g_disk2_slot, MII_SLOT_D2_GET_FLOPPY, floppies);
    if (res < 0 || !floppies[drive]) {
        printf("Failed to get floppy structure for drive %d (slot %d)\n", drive + 1, g_disk2_slot);
        return;
    }
    if (!disk_open_bdsk_image_file(&fp, disk->filename, path, sizeof(path))) {
        printf("Failed to open disk image %s\n", disk->filename);
        return;
    }
    mii_floppy_t *floppy = floppies[drive];
    mii_dd_file_t *file = &g_dd_files[drive];
    res = disk_write_floppy_bdsk_track_to_fatfs(drive, floppy, file, &fp, track_id);
    f_close(&fp);

    if (res < 0) {
        printf("Failed to write disk image track %d to floppy: %d\n", track_id, res);
    }
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
    
    /* 🔴 flush current track before eject */
    uint8_t track_id = floppies[drive]->track_id[floppies[drive]->qtrack];
    if (!floppies[drive]->write_protected &&
        track_id < MII_FLOPPY_TRACK_COUNT &&
        floppies[drive]->tracks[track_id].dirty)
    {
        disk_write_track(drive, track_id, mii);
    }
    
    // Re-initialize the floppy (clears all data, makes it "empty")
    mii_floppy_init(floppies[drive]);
    
    // Clear the static file structure
    memset(&g_dd_files[drive], 0, sizeof(g_dd_files[drive]));
    
    printf("Drive %d ejected\n", drive + 1);
}
