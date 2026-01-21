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

mii_dd_file_t *
mii_dd_file_load(
		mii_dd_system_t *dd,
		const char *pathname,
		uint16_t flags)
{
	// Not implemented for Pico - we use disk_loader.c instead
	MII_DEBUG_PRINTF("%s: ERROR - not implemented on Pico. Use disk_loader.c\n", __func__);
	return NULL;
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

int
mii_dd_read(
		mii_dd_t *	dd,
		struct mii_bank_t *bank,
		uint16_t 	addr,
		uint32_t	blk,
		uint16_t 	blockcount)
{
	// Block device read - used for SmartPort/ProDOS blocks
	// For floppy disk (Disk II), we use the track-based access instead
	/* not supported DD
	if (!dd->file || !dd->file->map)
		return -1;
		
	uint8_t *src = dd->file->map + (blk * 512);
	for (int i = 0; i < blockcount * 512; i++) {
		mii_bank_poke(bank, addr + i, src[i]);
	}
	return 0;
	*/
	return -1;
}

int
mii_dd_write(
		mii_dd_t *	dd,
		struct mii_bank_t *bank,
		uint16_t 	addr,
		uint32_t	blk,
		uint16_t 	blockcount)
{
	// Block device write - used for SmartPort/ProDOS blocks
	/* not supported DD
	if (!dd->file || !dd->file->map || dd->file->read_only)
		return -1;
		
	uint8_t *dst = dd->file->map + (blk * 512);
	for (int i = 0; i < blockcount * 512; i++) {
		dst[i] = mii_bank_peek(bank, addr + i);
	}
	return 0;
	*/
	return -1;
}
