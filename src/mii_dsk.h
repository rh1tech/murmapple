/*
 * mii_dsk.h
 *
 * Copyright (C) 2024 Michel Pollet <buserror@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */


#pragma once

#include "mii_floppy.h"

int
mii_floppy_dsk_load(
		mii_floppy_t *f,
		mii_dd_file_t *file );

// Render one 256-byte sector into the on-disk bitstream for a track.
// Exposed so platforms without mmap/PSRAM staging can stream-load images.
void
mii_floppy_dsk_render_sector(
		uint8_t vol,
		uint8_t track,
		uint8_t sector,
		const uint8_t *data,
		mii_floppy_track_t *dst,
		uint8_t *track_data);
void
_mii_floppy_dsk_write_sector(
		mii_dd_file_t *file,
		uint8_t *track_data,
		mii_floppy_track_map_t *map,
		uint8_t track_id,
		uint8_t sector,
		uint8_t data_sector[342 + 1] );
int
mii_floppy_decode_sector(
		uint8_t data_sector[342 + 1],
		uint8_t data[256]);

void
mii_floppy_dsk_recover_sector(
        uint8_t vol,
        uint8_t track,
        uint8_t sector,
        uint8_t *data,                 // whole DSK track buffer (16*256 bytes)
        const mii_floppy_track_t *src, // source descriptor
        const uint8_t *track_data      // source bitstream (curr_track_data)
);
