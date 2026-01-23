/*
 * mii_bank.h
 *
 * Copyright (C) 2023 Michel Pollet <buserror@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

struct mii_bank_t;
/*
 * Bank access callback can be register with banks and will be called
 * when a specific page (or pages) are read/written to.
 * Parameter will be the bank specified, the 'real' address beind read/written
 * and the 'param' passed when the callback was registered.
 * The callback should return true if it handled the access, false otherwise.
 * If it return true, the bank will not be accessed for this read/write.
 *
 * Note: the callback will be called once with bank == NULL when the bank
 * is being disposed, this allow any memory allocated by the callback to be
 * freed.
 */
typedef bool (*mii_bank_access_cb)(
		struct mii_bank_t *bank,
		void *param,
		uint16_t addr,
		uint8_t * byte,
		bool write);

typedef struct mii_bank_access_t {
	mii_bank_access_cb cb;
	void *param;
} mii_bank_access_t;

typedef struct vram_page_t {
    uint8_t pinned : 1, // do not use it in swap, the page should be in SRAM
			in_ram : 1; // already in RAM now
	uint8_t lba; // page number in real RAM (mii_bank_t.raw) file 0..255
} vram_page_t;

typedef struct sram_page_t {
    uint8_t dirty  : 1; // page was changed, so it should be saved to swap (not just unload)
} sram_page_t;

#define RAM_IN_PAGE_ADDR_MASK (0x000000FF) // one byte adresses 256 pages
#define RAM_PAGE_SIZE (0x00000100L)

#if PICO_RP2040
	#ifdef VIDEO_HDMI
		#if defined(FEATURE_AUDIO_I2S)
			#define RAM_PAGES_PER_POOL (232-80)
		#else
			#define RAM_PAGES_PER_POOL (252)
		#endif
	#else
		#if defined(FEATURE_AUDIO_I2S)
			#define RAM_PAGES_PER_POOL (242-66)
		#else
			#define RAM_PAGES_PER_POOL (256)
		#endif
	#endif
#else
	#define RAM_PAGES_PER_POOL (256)
#endif

#define MAX_PAGES_PER_POOL (256)
#define SHIFT_AS_DIV (8)

#include <fatfs/ff.h>

typedef struct vram_t {
	uint8_t*	raw;			// pointer to direct (raw) RAM/ROM or 
	const char* filename;  		// use this filename for case swap this VRAM instance
	vram_page_t	v_desc[MAX_PAGES_PER_POOL];	// index - virtual page number 0..0xFF (Apple II >> SHIFT_AS_DIV)
	sram_page_t s_desc[RAM_PAGES_PER_POOL]; // index - (addr32 >> SHIFT_AS_DIV) - real SRAM stored (on RP2040/RP2350) page descriptors
	uint8_t		oldest_vpage;	// rolling page (for invalidation), it is virtual page number (desc[#], Aplle II address related)
	FIL f;
} vram_t;

typedef struct mii_bank_t {
#if WITH_BANK_ACCESS
	mii_bank_access_t* access;
#endif
	uint16_t	base;		// base Aplle II address
	uint8_t		size;		// total size in pages (including VRAM case), 0..255
	char*		name;
	uint32_t 	logical_mem_offset; // for case .raw[0] not points to .base[0], used .raw[0] <- .base[logical_mem_offset]
	union {
		uint8_t*	raw;			// pointer to direct (raw) RAM/ROM or 
		vram_t*     vram_desc;      // VRAM descriptor
	} ua; // union access to raw SRAM or to related VirtualRAM .vram should be 1 for last case
	uint8_t		no_alloc: 1,	// not allocated memory (static)
				alloc   : 1,	// been callocated()
				ro      : 1,	// read only
				vram    : 1;	// *raw has no space for all pages
} mii_bank_t;

void init_ram_pages_for(vram_t* v, uint8_t* raw, uint32_t raw_size);

#if defined(PICO_RP2350) || (defined(RAM_PAGES_PER_POOL) && defined(MAX_PAGES_PER_POOL) && (RAM_PAGES_PER_POOL == MAX_PAGES_PER_POOL))
inline static
uint8_t get_ram_page_for(vram_t* __restrict vram, const uint16_t addr16) {
    // all pages in memory, not required to calculate something
	(vram);
	return addr16 >> SHIFT_AS_DIV;
}
#else
uint8_t get_ram_page_for(vram_t* __restrict vram, const uint16_t addr16);
#endif

inline static
void pin_ram_pages_for(
        vram_t* v,
        const uint32_t start_addr,
        const uint16_t len_bytes)
{
#if defined(PICO_RP2350) || (defined(RAM_PAGES_PER_POOL) && defined(MAX_PAGES_PER_POOL) && (RAM_PAGES_PER_POOL == MAX_PAGES_PER_POOL))
    // all pages in memory, not required to pin something
	(v);
	(start_addr);
	(len_bytes);
	return;
#else
    if (!v)
        return;
    const uint32_t first = start_addr >> SHIFT_AS_DIV;
    const uint32_t last  = (len_bytes == 0)
        ? first - 1
        : (start_addr + len_bytes - 1) >> SHIFT_AS_DIV;

	for (uint32_t p = 2; p < MAX_PAGES_PER_POOL; ++p) {
        v->v_desc[p].pinned = 0;
    }
	// pages 0 & 1 always pinned
    for (uint32_t vpage = 2; vpage < MAX_PAGES_PER_POOL; ++vpage) {
        if (vpage >= first && vpage <= last) {
            if (!v->v_desc[vpage].pinned) {
                get_ram_page_for(v, vpage << SHIFT_AS_DIV);
                v->v_desc[vpage].pinned = 1;
            }
        } else {
            v->v_desc[vpage].pinned = 0;
        }
    }
#endif	
}

inline static
uint8_t ram_page_read(vram_t* v, const uint32_t addr32) {
    const register uint8_t ram_page = get_ram_page_for(v, addr32);
    const register uint32_t addr_in_page = addr32 & RAM_IN_PAGE_ADDR_MASK;
    return v->raw[(ram_page * RAM_PAGE_SIZE) + addr_in_page];
}

inline static
void ram_page_write(vram_t* v, const uint32_t addr32, const uint8_t val) {
    const register uint8_t ram_page = get_ram_page_for(v, addr32);
    const register uint32_t addr_in_page = addr32 & RAM_IN_PAGE_ADDR_MASK;
    v->raw[(ram_page * RAM_PAGE_SIZE) + addr_in_page] = val;
	v->s_desc[ram_page].dirty = 1;
}

void
mii_bank_init(
		mii_bank_t *bank);
void
mii_bank_dispose(
		mii_bank_t *bank);
void
mii_bank_write(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t *data,
		uint16_t len);
void
mii_bank_read(
		mii_bank_t *bank,
		uint16_t addr,
		uint8_t *data,
		uint16_t len);
bool
mii_bank_access(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t *data,
		uint16_t len,
		bool write);

/* return the number of pages dirty (written into since last time) between
 * addr1 and addr2 (inclusive) */
uint8_t
mii_bank_is_dirty(
		mii_bank_t *bank,
		uint16_t addr1,
		uint16_t addr2);
void
mii_bank_install_access_cb(
		mii_bank_t *bank,
		mii_bank_access_cb cb,
		void *param,
		uint8_t page,
		uint8_t end);

#ifdef MII_RP2350
// Ultra-fast inline peek for RP2350 - avoids function call overhead
// Only works for banks without access callbacks (most common case)
static inline __attribute__((always_inline)) uint8_t
mii_bank_peek(
		mii_bank_t *bank,
		uint16_t addr)
{
	uint32_t phy = bank->logical_mem_offset + addr - bank->base;
	return bank->vram ? ram_page_read(bank->ua.vram_desc, phy) : bank->ua.raw[phy];
}

static inline __attribute__((always_inline)) void
mii_bank_poke(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t data)
{
	uint32_t phy = bank->logical_mem_offset + addr - bank->base;
	if (bank->vram)
		ram_page_write(bank->ua.vram_desc, phy, data);
	else
		bank->ua.raw[phy] = data;
}
#else
static inline void
mii_bank_poke(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t data)
{
	mii_bank_write(bank, addr, &data, 1);
}

static inline uint8_t
mii_bank_peek(
		mii_bank_t *bank,
		uint16_t addr)
{
	uint8_t res = 0;
	mii_bank_read(bank, addr, &res, 1);
	return res;
}
#endif
