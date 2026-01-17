/*
 * mii_bank.c
 *
 * Copyright (C) 2023 Michel Pollet <buserror@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <pico.h>
#include <pico/stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mii.h"
#include "mii_bank.h"
#include "debug_log.h"


void
mii_bank_init(
		mii_bank_t *bank)
{
	if (bank->ua.raw)
		return;
	if (bank->logical_mem_offset == 0 && !bank->no_alloc) {
		bank->ua.raw = calloc(1, bank->size * 256);
		bank->alloc = 1;
	}
}

void
mii_bank_dispose(
		mii_bank_t *bank)
{
//	printf("%s %s\n", __func__, bank->name);
	if (bank->alloc)
		free(bank->ua.raw);
	bank->ua.raw = NULL;
	bank->alloc = 0;
#if WITH_BANK_ACCESS
	if (bank->access) {
		// Allow callback to free anything it wants
		for (int i = 0; i < bank->size; i++)
			if (bank->access[i].cb)
				bank->access[i].cb(NULL, bank->access[i].param, 0, NULL, false);
		free(bank->access);
	}
	bank->access = NULL;
#endif
}

bool
mii_bank_access(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t *data,
		uint16_t len,
		bool write)
{
#if WITH_BANK_ACCESS
	uint8_t page_index = (addr - bank->base) >> 8;
	if (bank->access && bank->access[page_index].cb) {
		if (bank->access[page_index].cb(bank, bank->access[page_index].param,
					addr, (uint8_t *)data, write))
			return true;
	}
#endif
	return false;
}

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

void
mii_bank_write(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t *data,
		uint16_t len)
{
    if (mii_bank_access(bank, addr, data, len, true))
        return;
	if (!bank->vram) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		do {
			bank->ua.raw[phy++] = *data++;
		} while (likely(--len));
		return;
	}
	vram_t* v = bank->ua.vram_desc;
	while (len) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		uint32_t off  = phy & RAM_IN_PAGE_ADDR_MASK;
		uint32_t n    = MIN(len, RAM_PAGE_SIZE - off);
		uint32_t lba_page = get_ram_page_for(v, phy);
		v->s_desc[lba_page].dirty = 1;
		uint8_t *dst = v->raw + (lba_page << SHIFT_AS_DIV) + off;
		memcpy(dst, data, n);
		addr += n;
		data += n;
		len  -= n;
	}
}

void
mii_bank_read(
		mii_bank_t *bank,
		uint16_t addr,
		uint8_t *data,
		uint16_t len)
{
	if (mii_bank_access(bank, addr, data, len, false))
		return;
	if (!bank->vram) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		do {
			*data++ = bank->ua.raw[phy++];
		} while (likely(--len));
		return;
	}
	vram_t* v = bank->ua.vram_desc;
	while (len) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		uint32_t off  = phy & RAM_IN_PAGE_ADDR_MASK;
		uint32_t n    = MIN(len, RAM_PAGE_SIZE - off);
		uint32_t lba_page = get_ram_page_for(v, phy);
		uint8_t *dst = v->raw + (lba_page << SHIFT_AS_DIV) + off;
		memcpy(data, dst, n);
		addr += n;
		data += n;
		len  -= n;
	}
}

#if WITH_BANK_ACCESS
void
mii_bank_install_access_cb(
		mii_bank_t *bank,
		mii_bank_access_cb cb,
		void *param,
		uint8_t page,
		uint8_t end)
{
	if (!end)
		end = page;
	if ((page << 8) < bank->base || (end << 8) > (bank->base + bank->size * 256)) {
		MII_DEBUG_PRINTF("%s %s INVALID install access cb %p param %p page %02x-%02x\n",
					__func__, bank->name, cb, param, page, end);
		return;
	}
	page -= bank->base >> 8;
	end -= bank->base >> 8;
	if (!bank->access) {
		bank->access = calloc(1, bank->size * sizeof(bank->access[0]));
	}
	MII_DEBUG_PRINTF("%s %s install access cb page %02x:%02x\n",
			__func__, bank->name, page, end);
	for (int i = page; i <= end; i++) {
		if (bank->access[i].cb)
			MII_DEBUG_PRINTF("%s %s page %02x already has a callback\n",
					__func__, bank->name, i);
		bank->access[i].cb = cb;
		bank->access[i].param = param;
	}
}
#endif

// To save vpage
inline static
void flush_vram_block(vram_t* __restrict vram, vram_page_t* desc, const uint8_t vpage) {
    gpio_put(PICO_DEFAULT_LED_PIN, true);
    const uint32_t file_off = ((uint32_t)vpage) * RAM_PAGE_SIZE;
    const uint32_t ram_off  = ((uint32_t)desc->lba) * RAM_PAGE_SIZE;
	f_lseek(&vram->f, file_off);
    UINT wb;
    f_write(&vram->f,
            vram->raw + ram_off,
            RAM_PAGE_SIZE,
            &wb);
	// mark page as not more stored
	desc->in_ram = 0;
    gpio_put(PICO_DEFAULT_LED_PIN, false);
}

// To load vpage
inline static
void read_vram_block(vram_t* __restrict vram, const uint8_t vpage, const uint8_t lba_page) {
    gpio_put(PICO_DEFAULT_LED_PIN, true);
	register vram_page_t* desc = &vram->v_desc[vpage]; // target
    const uint32_t file_off = ((uint32_t)vpage) * RAM_PAGE_SIZE;
    const uint32_t ram_off  = ((uint32_t)lba_page) * RAM_PAGE_SIZE;
	f_lseek(&vram->f, file_off);
    UINT rb;
    f_read(&vram->f,
           vram->raw + ram_off,
           RAM_PAGE_SIZE,
           &rb);
	// mew owner for the lba page
	desc->lba = lba_page;
	desc->in_ram = 1;
	vram->s_desc[lba_page].dirty = 0; // just read, not yet changed
    gpio_put(PICO_DEFAULT_LED_PIN, false);
}

uint8_t get_ram_page_for(vram_t* __restrict vram, const uint16_t addr16) {
    const register uint8_t vpage = addr16 >> SHIFT_AS_DIV; // page idx in Aplle II space
	register vram_page_t* desc = &vram->v_desc[vpage];
	if (desc->in_ram)
    	return desc->lba; // page idx in swap RAM

	// lookup for a page to be unload
    uint8_t* pold = &vram->oldest_vpage;
    uint8_t invalidate_vpage = *pold;
    while (vram->v_desc[invalidate_vpage].pinned || !vram->v_desc[invalidate_vpage].in_ram) {
        invalidate_vpage++;
    }
    // advance oldest to next position after selected victim
    *pold = (uint8_t)(invalidate_vpage + 1);
	
	desc = &vram->v_desc[invalidate_vpage]; // victim
	register uint8_t lba_page = desc->lba; // this lba will be owned by other vpage
	// save changed block into swap file first
	if (vram->s_desc[lba_page].dirty) {
		flush_vram_block(vram, desc, invalidate_vpage);
	} else {
		desc->in_ram = 0;
	}

	read_vram_block(vram, vpage, lba_page);
	return lba_page;
}

inline static
void pin_ram_pages_for_core0(
        vram_t* v,
        const uint32_t start_addr,
        const uint16_t len_bytes)
{
    for (uint32_t off = 0; off < len_bytes; off += RAM_PAGE_SIZE) {
		uint32_t addr = start_addr + off;
		get_ram_page_for(v, addr); // ensure page is in RAM
        v->v_desc[addr / RAM_PAGE_SIZE].pinned = 1; // mark to not unload related SRAM page
    }
}

void init_ram_pages_for(vram_t* v, uint8_t* raw, uint32_t raw_size) {
	memset(raw, 0, raw_size);
//	memset(v->v_desc, 0, sizeof(v->v_desc));
//	memset(v->s_desc, 0, sizeof(v->s_desc));
	// mark free pages to me in RAM
	for (int i = 0; i < raw_size / RAM_PAGE_SIZE; ++i) {
		uint8_t lba = i;
		v->s_desc[lba].dirty = 0;
		v->v_desc[i].in_ram = 1;
		v->v_desc[i].lba = lba;
	}
	// always in mem (zero-page and CPU stack) = 2 pages
	v->v_desc[0].pinned = 1;
	v->v_desc[1].pinned = 1;
	v->oldest_vpage = 2;
	// TODO: error handling later
	f_open(&v->f, v->filename, FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
	for (int i = 0; i < 256; ++i) { // file should contain max possible pages
		UINT wb;
		f_write(&v->f, raw, 256, &wb); // save zero-bytes pages
	}

	/* ---------- TEMPORARY VIDEO W/A ---------- */
	// TEXT / LORES page 1 + 2 (2 x 1 KB = 8 pages)
	pin_ram_pages_for_core0(v, 0x0400, 0x0400); // $0400–$07FF
	pin_ram_pages_for_core0(v, 0x0800, 0x0400); // $0800–$0BFF

	// HIRES / DHIRES page 1 (8K = 32 pages)
	pin_ram_pages_for_core0(v, 0x2000, 0x2000); // $2000–$3FFF

	// HIRES / DHIRES page 2 (8k = 32 pages)
	pin_ram_pages_for_core0(v, 0x4000, 0x2000); // $4000–$5FFF
	
	// TOTAL pinned 74 pages
}
