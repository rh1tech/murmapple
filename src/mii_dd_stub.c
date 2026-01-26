/*
 * mii_dd_stub.c
 *
 * Simplified disk drive system for RP2350
 * The full mii_dd.c uses mmap/file I/O which isn't available on Pico.
 * We provide minimal stubs since we load disk images directly to PSRAM.
 *
 * Based on mii_dd.c Copyright (C) 2023 Michel Pollet
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mii_bank.h"
#include "mii_dd.h"
#include "debug_log.h"

void
mii_dd_system_init(
		struct mii_t *mii,
		mii_dd_system_t *dd )
{
	dd->drive = NULL;
	dd->file = NULL;
}

void
mii_dd_system_dispose(
		mii_dd_system_t *dd )
{
	// Nothing to dispose - we don't mmap files on Pico
	dd->drive = NULL;
	dd->file = NULL;
}

void
mii_dd_register_drives(
		mii_dd_system_t *dd,
		mii_dd_t * drives,
		uint8_t count )
{
	for (int i = 0; i < count; i++) {
		drives[i].next = dd->drive;
		dd->drive = &drives[i];
		drives[i].dd = dd;
	}
}

int
mii_dd_drive_load(
		mii_dd_t *dd,
		mii_dd_file_t *file )
{
	if (dd->file == file)
		return 0;
	// On Pico, we just update the pointer - no mmap to manage
	dd->file = file;
	if (file) {
		MII_DEBUG_PRINTF("%s: %s loading %s\n", __func__,
					dd->name, file->pathname);
	}
	return 0;
}

void
mii_dd_file_dispose(
		mii_dd_system_t *dd,
		mii_dd_file_t *file )
{
	if (!file)
		return;
		
	// Remove from dd's file queue
	if (dd->file == file)
		dd->file = file->next;
	else {
		mii_dd_file_t *f = dd->file;
		while (f) {
			if (f->next == file) {
				f->next = file->next;
				break;
			}
			f = f->next;
		}
	}

	// Don't free pathname since it's statically allocated in disk_loader
	// free(file);  // Don't free - we use static allocation
}

static mii_dd_file_t mii_dd_files[2] = { 0 };

mii_dd_file_t *
mii_dd_file_load(
		mii_dd_system_t *dd,
		const char *pathname,
		uint16_t flags)
{
	if (flags >= 2) return 0;
	FIL f;
	strncpy(mii_dd_files[flags].pathname, pathname, sizeof(mii_dd_files[flags].pathname));
	mii_dd_files[flags].read_only = false;
	mii_dd_files[flags].size = 32L << 20; // W/A 32 MB
	f_open(&f, pathname, FA_WRITE | FA_OPEN_ALWAYS);
	f_lseek(&f, mii_dd_files[0].size - 1);
	f_close(&f);
	return &mii_dd_files[flags];
}

mii_dd_file_t *
mii_dd_file_in_ram(
		mii_dd_system_t *dd,
		const char *pathname,
		uint32_t size,
		uint16_t flags)
{
	// Not implemented for Pico - we use disk_loader.c instead
	MII_DEBUG_PRINTF("%s: ERROR - not implemented on Pico. Use disk_loader.c\n", __func__);
	return NULL;
}

#include "ff.h"

int
mii_dd_read(
    mii_dd_t *dd,
    struct mii_bank_t *bank,
    uint16_t addr,
    uint32_t blk,
    uint16_t blockcount)
{
	if (!dd->file || !dd->file->pathname) return -1;
	FIL f;
	if (FR_OK != f_open(&f, dd->file->pathname, FA_READ | FA_OPEN_ALWAYS)) return -1;

    if (f_lseek(&f, 512 * blk) != FR_OK) goto err;

    uint8_t buf[512];
    UINT br = 0;

    for (uint16_t b = 0; b < blockcount; b++) {
        if (f_read(&f, buf, 512, &br) != FR_OK || br != 512)
			goto err;
        uint16_t base = addr + (uint16_t)(b * 512u);
        for (uint16_t i = 0; i < 512; i++)
            mii_bank_poke(bank, base + i, buf[i]);
    }
	f_close(&f);
    return 0;
err:
	f_close(&f);
    return -1;
}

#include "ff.h"

int
mii_dd_write(
    mii_dd_t *dd,
    struct mii_bank_t *bank,
    uint16_t addr,
    uint32_t blk,
    uint16_t blockcount)
{
	if (!dd->file || !dd->file->pathname) return -1;
	FIL f;
	if (FR_OK != f_open(&f, dd->file->pathname, FA_WRITE | FA_OPEN_ALWAYS)) return -1;

    if (f_lseek(&f, 512 * blk) != FR_OK)
        goto err;

    uint8_t buf[512];
    UINT bw = 0;
    for (uint16_t b = 0; b < blockcount; b++) {
        uint16_t base = addr + (uint16_t)(b * 512u);
        for (uint16_t i = 0; i < 512; i++)
            buf[i] = mii_bank_peek(bank, base + i);
        if (f_write(&f, buf, 512, &bw) != FR_OK || bw != 512)
	        goto err;
    }

	f_close(&f);
    return 0;
err:
	f_close(&f);
    return -1;
}
