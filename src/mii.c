/*
 * mii.c
 *
 * Copyright (C) 2023 Michel Pollet <buserror@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <pico.h>
#include <hardware/sync.h>

#include "mii.h"
#include "mii_bank.h"
#include "mii_video.h"
#include "mii_sw.h"
#include "mii_65c02.h"
#include "minipt.h"
#include "debug_log.h"
#include "ff.h"

#if MII_65C02_DIRECT_ACCESS
static mii_cpu_state_t
_mii_cpu_direct_access_cb(
		struct mii_cpu_t *cpu,
		mii_cpu_state_t   access );
#endif

mii_slot_drv_t * mii_slot_drv_list = NULL;

extern const uint8_t mii_rom_iiee[16384];

// Static memory arrays for RP2350 (no malloc/realloc)
static uint8_t rp2350_sw_mem[256] __aligned(256);        // Soft switch area
static uint8_t rp2350_card_rom[0x0F00] __aligned(256);   // Card ROM area ($C100-$CFFF = 15 pages)

uint8_t vram[2 * RAM_PAGES_PER_POOL * RAM_PAGE_SIZE] __aligned(4096);

static vram_t main_vram_d = {
	.filename = "/tmp/mii_main.swap",
	.raw = vram
};
static vram_t aux_vram_d = { 
	.filename = "/tmp/mii_aux.swap",
	.raw = vram + RAM_PAGES_PER_POOL * RAM_PAGE_SIZE
};

static const mii_bank_t	_mii_banks_init[MII_BANK_COUNT] = {
	[MII_BANK_MAIN] = {
		.name = "MAIN",
		.base = 0x0000,
		.size = 0xc0,
		.ua.vram_desc = &main_vram_d,
		.logical_mem_offset = 0x0000,
		.no_alloc = 1,
		.vram = 1,
	},
	[MII_BANK_BSR] = {
		.name = "BSR",
		.base = 0xd000,
		.size = 64,
		.logical_mem_offset = 0xd000,
		.ua.vram_desc = &main_vram_d,
		.no_alloc = 1,
		.vram = 1,
	},
	[MII_BANK_BSR_P2] = {
		.name = "BSR P2",
		.base = 0xd000,
		.size = 16,
		.logical_mem_offset = 0xc000,
		.ua.vram_desc = &main_vram_d,
		.no_alloc = 1,
		.vram = 1,
	},
	/*
	 * Aux memory is multiple times 64KB (with the ramworks card). *This* bank
	 * is always mapped to that aux bank zero, and is used for video as the
	 * video is always locked to that bank.
	 *
	 * So in MOST cases MII_BANK_AUX_BASE is pretty much the same as
	 * MII_BANK_AUX whent he ramworks is not in use.
	 */
	[MII_BANK_AUX_BASE] = {
		.name = "AUX_BASE",
		.base = 0x0000,
		.size = 0xd0, // 208 pages, 48KB
		.logical_mem_offset = 0x0000,
		.ua.vram_desc = &aux_vram_d,
		.no_alloc = 1,
		.vram = 1,
	},
	/* This one is the one that is remapped with ramworks is in use */
	[MII_BANK_AUX] = {
		.name = "AUX",
		.base = 0x0000,
		.size = 0xd0, // 208 pages, 48KB
		.logical_mem_offset = 0x0000,
		.ua.vram_desc = &aux_vram_d,
		.no_alloc = 1,
		.vram = 1,
	},
	[MII_BANK_AUX_BSR] = {
		.name = "AUX BSR",
		.base = 0xd000,
		.size = 64,
		.logical_mem_offset = 0xd000,
		.ua.vram_desc = &aux_vram_d,
		.no_alloc = 1,
		.vram = 1,
	},
	[MII_BANK_AUX_BSR_P2] = {
		.name = "AUX BSR P2",
		.base = 0xd000,
		.size = 16,
		.logical_mem_offset = 0xc000,
		.ua.vram_desc = &aux_vram_d,
		.no_alloc = 1,
		.vram = 1,
	},
	[MII_BANK_ROM] = {
		.name = "ROM",
		.base = 0xc000,
		.size = 0x40, // 64 pages, 16KB
		.ua.raw = (uint8_t*)mii_rom_iiee,
		.ro = 1,
		.no_alloc = 1,
	},
	[MII_BANK_CARD_ROM] = {
		.name = "CARD ROM",
		.base = 0xc100,
		// c100-cfff = 15 pages
		// 7 * 2KB for extended ROMs for cards (not addressable directly)
		// Car roms are 'banked' as well, so we don't need to copy them around
		.size = 15,// + (7 * 8),
		.ua.raw = rp2350_card_rom,
		.no_alloc = 1,
		.ro = 1,
	},
	[MII_BANK_SW] = {
		.name = "SW",
		.base = 0xc000,
		.size = 0x1,
		.ua.raw = rp2350_sw_mem,
		.no_alloc = 1
	},
};


#include "mii_65c02_ops.h"
#include "mii_65c02_disasm.h"

void
mii_dump_trace_state(
	mii_t *mii)
{
	mii_cpu_t * cpu = &mii->cpu;
	mii_cpu_state_t s = mii->cpu_state;
	MII_DEBUG_PRINTF("PC:%04X A:%02X X:%02X Y:%02X S:%02x #%d %c AD:%04X D:%02x %c ",
		cpu->PC, cpu->A, cpu->X, cpu->Y, cpu->S, cpu->cycle,
		s.sync ? 'I' : ' ', s.addr, s.data, s.w ? 'W' : 'R');
	// display the S flags
	static const char *s_flags = "CZIDBRVN";
	for (int i = 0; i < 8; i++)
		MII_DEBUG_PRINTF("%c", MII_GET_P_BIT(cpu, i) ? s_flags[i] : tolower(s_flags[i]));
//	if (s.sync)
	{
		uint8_t op[16];
		for (int i = 0; i < 4; i++) {
			mii_mem_access(mii, mii->cpu.PC + i, op + i, false, false);
		}
		mii_op_t d = mii_cpu_op[op[0]];
		MII_DEBUG_PRINTF(" ");
		char dis[32];
		mii_cpu_disasm_one(op, cpu->PC, dis, sizeof(dis),
					MII_DUMP_DIS_DUMP_HEX);
		MII_DEBUG_PRINTF(": %s", dis);
		if (d.desc.branch) {
			if (MII_GET_P_BIT(cpu, d.desc.s_bit) == d.desc.s_bit_value)
				MII_DEBUG_PRINTF(" ; taken");
		}
		MII_DEBUG_PRINTF("\n");
	}
//	else
//		printf("\n");
}

void
mii_dump_run_trace(
	mii_t *mii)
{
	// walk all the previous PC values in mii->trace, and display a line
	// of disassembly for all of them
	for (int li = 0; li < MII_PC_LOG_SIZE; li++) {
		int idx = (mii->trace.idx + li) & (MII_PC_LOG_SIZE - 1);
		uint16_t pc = mii->trace.log[idx];
		uint8_t op[16];
		for (int i = 0; i < 4; i++)
			mii_mem_access(mii, pc + i, op + i, false, false);
//		mii_op_t d = mii_cpu_op[op[0]];
		char dis[64];
		mii_cpu_disasm_one(op, pc, dis, sizeof(dis),
						MII_DUMP_DIS_PC | MII_DUMP_DIS_DUMP_HEX);
		MII_DEBUG_PRINTF("%s\n", dis);
	}
}

#define _SAME 0xf

static inline void
mii_page_set(
		mii_t * mii,
		uint8_t read,
		uint8_t write,
		uint8_t bank,
		uint8_t end )
{
	for (int i = bank; i <= end; i++) {
		if (read != _SAME)
			mii->mem[i].read = read;
		if (write != _SAME)
			mii->mem[i].write = write;
	}
}

static inline uint8_t
mii_sw(
		mii_t *mii,
		uint16_t sw)
{
	return mii_bank_peek(&mii->bank[MII_BANK_SW], sw);
}

static void
mii_page_table_update(
		mii_t *mii)
{
	if (likely(!mii->mem_dirty))
		return;
	mii->mem_dirty = 0;
	uint32_t sw = mii->sw_state;
	bool altzp 		= SWW_GETSTATE(sw, SWALTPZ);
	bool page2 		= SWW_GETSTATE(sw, SWPAGE2);
	bool store80 	= SWW_GETSTATE(sw, SW80STORE);
	bool hires 		= SWW_GETSTATE(sw, SWHIRES);
	bool ramrd 		= SWW_GETSTATE(sw, SWRAMRD);
	bool ramwrt 	= SWW_GETSTATE(sw, SWRAMWRT);
	bool intcxrom 	= SWW_GETSTATE(sw, SWINTCXROM);
	bool slotc3rom 	= SWW_GETSTATE(sw, SWSLOTC3ROM);
	bool intc8rom	= SWW_GETSTATE(sw, INTC8ROM);

	if (unlikely(mii->trace_cpu))
		MII_DEBUG_PRINTF("%04x: MEM update altzp:%d page2:%d store80:%d "
			"hires:%d ramrd:%d ramwrt:%d intcxrom:%d "
			"slotc3rom:%d\n", mii->cpu.PC,
			altzp, page2, store80, hires, ramrd, ramwrt, intcxrom, slotc3rom);
	// clean slate
	mii_page_set(mii, MII_BANK_MAIN, MII_BANK_MAIN, 0x00, 0xbf);
	mii_page_set(mii, MII_BANK_SW, MII_BANK_SW, 0xc0, 0xc0);
	mii_page_set(mii, MII_BANK_ROM, MII_BANK_ROM, 0xc1, 0xff);
	if (altzp)
		mii_page_set(mii, MII_BANK_AUX, MII_BANK_AUX, 0x00, 0x01);
	mii_page_set(mii,
		ramrd ? MII_BANK_AUX : MII_BANK_MAIN,
		ramwrt ? MII_BANK_AUX : MII_BANK_MAIN, 0x02, 0xbf);
	if (store80) {
		mii_page_set(mii,
			page2 ? MII_BANK_AUX : MII_BANK_MAIN,
			page2 ? MII_BANK_AUX : MII_BANK_MAIN, 0x04, 0x07);
		if (hires)
			mii_page_set(mii,
				page2 ? MII_BANK_AUX : MII_BANK_MAIN,
				page2 ? MII_BANK_AUX : MII_BANK_MAIN, 0x20, 0x3f);
	}
	// c1-cf are at ROM state when we arrive here
	if (mii->emu == MII_EMU_IIC) {
	//	mii_page_set(mii, MII_BANK_ROM, MII_BANK_ROM, 0xc1, 0xcf);
	//	mii_page_set(mii, MII_BANK_ROM, MII_BANK_ROM, 0xd0, 0xff);
	} else {
		if (!intcxrom) {
			mii_page_set(mii, MII_BANK_CARD_ROM, MII_BANK_CARD_ROM, 0xc1, 0xcf);
			if (!slotc3rom)
				mii_page_set(mii, MII_BANK_ROM, _SAME, 0xc3, 0xc3);
			if (intc8rom)
				mii_page_set(mii, MII_BANK_ROM, _SAME, 0xc8, 0xcf);
		}
	}
	bool bsrread 	= SWW_GETSTATE(sw, BSRREAD);
	bool bsrwrite 	= SWW_GETSTATE(sw, BSRWRITE);
	bool bsrpage2 	= SWW_GETSTATE(sw, BSRPAGE2);
	mii_page_set(mii,
		bsrread ?
			altzp ? MII_BANK_AUX_BSR : MII_BANK_BSR :
					MII_BANK_ROM,
		bsrwrite ?
			altzp ? MII_BANK_AUX_BSR : MII_BANK_BSR :
					MII_BANK_ROM,
				0xd0, 0xff);
	// BSR P2
	mii_page_set(mii,
		bsrread ?
			(altzp ? MII_BANK_AUX_BSR : MII_BANK_BSR) + bsrpage2 :
					MII_BANK_ROM,
		bsrwrite ?
			(altzp ? MII_BANK_AUX_BSR : MII_BANK_BSR) + bsrpage2 :
					MII_BANK_ROM,
				0xd0, 0xdf);
}

#if !MII_RP2350
static void
mii_bank_update_ramworks(
		mii_t *mii,
		uint8_t bank)
{
	if (bank > 127 ||
			!(mii->ramworks.avail & ((unsigned __int128)1ULL << bank)))
		bank = 0;
	if (!mii->ramworks.bank[bank]) {
		mii->ramworks.bank[bank] = malloc(0x10000);
		int c = 0, a = 0;
		for (int i = 0; i < 128; i++ ) {
			if (mii->ramworks.bank[i])
				c++;
			if (mii->ramworks.avail & ((unsigned __int128)1ULL << i))
				a++;
		}
		MII_DEBUG_PRINTF("%s: RAMWORKS alloc bank %2d (%dKB / %dKB)\n", __func__,
			bank, c * 64, a * 64);
	}
	mii->bank[MII_BANK_AUX_BASE].mem = mii->ramworks.bank[0];
	mii->bank[MII_BANK_AUX].mem = mii->ramworks.bank[bank];
	mii->bank[MII_BANK_AUX_BSR].mem = mii->ramworks.bank[bank];
	mii->bank[MII_BANK_AUX_BSR_P2].mem = mii->ramworks.bank[bank];
}
#endif

void
mii_set_sw_override(
		mii_t *mii,
		uint16_t sw_addr,
		mii_bank_access_cb cb,
		void *param)
{
	if (!mii->soft_switches_override)
		mii->soft_switches_override = calloc(256,
				sizeof(*mii->soft_switches_override));
	sw_addr &= 0xff;
	mii->soft_switches_override[sw_addr].cb = cb;
	mii->soft_switches_override[sw_addr].param = param;
}

/*
 * This watches for any write to 0xcfff -- if a card had it's aux rom
 * selected, it will deselect it.
 */
static bool
_mii_deselect_cXrom(
		mii_t * mii,
		uint16_t addr,
		uint8_t * byte,
		bool write)
{
	if (addr != 0xcfff)
		return false;
	if (!SW_GETSTATE(mii, INTC8ROM))
		return false;
	for (int i = 0; i < 7; i++) {
		mii_slot_t * slot = &mii->slot[i];
		if (slot->aux_rom_selected) {
			MII_DEBUG_PRINTF("%s %d: %s\n", __func__,
					i, slot->drv ? slot->drv->name : "(none?)");
			slot->aux_rom_selected = false;
		}
	}
	SW_SETSTATE(mii, INTC8ROM, 0);
	mii->mem_dirty = true;
	mii_page_table_update(mii);
	return false;
}

static bool
_mii_select_c3introm(
		struct mii_bank_t *bank,
		void *param,
		uint16_t addr,
		uint8_t * byte,
		bool write)
{
	mii_t * mii = param;
	if (!SW_GETSTATE(mii, SWSLOTC3ROM) && !SW_GETSTATE(mii, INTC8ROM)) {
		SW_SETSTATE(mii, INTC8ROM, 1);
		mii->mem_dirty = true;
		mii_page_table_update(mii);
	}
	return false;
}

static bool
mii_access_soft_switches(
		mii_t *mii,
		uint16_t addr,
		uint8_t * byte,
		bool write)
{
	if (!(addr >= 0xc000 && addr <= 0xc0ff))
		return false;
	bool res = false;
	uint8_t on = 0;
	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	const uint16_t sw_save = mii->sw_state;

	/*
	 * This allows driver (titan accelerator etc) to have their own
	 * soft switches, and override/supplement any default ones.
	 */
	if (mii->soft_switches_override && mii->soft_switches_override[addr & 0xff].cb) {
		res = mii->soft_switches_override[addr & 0xff].cb(
					sw, mii->soft_switches_override[addr & 0xff].param,
					addr, byte, write);
		if (res)
			return res;
	}
	switch (addr) {
		case 0xc090 ... 0xc0ff: {
			res = true;
			int slot = ((addr >> 4) & 7) - 1;
#if 0
			printf("SLOT %d addr %04x write %d %02x drv %s\n",
				slot + 1, addr, write, *byte,
				mii->slot[slot].drv ? mii->slot[slot].drv->name : "none");
#endif
			if (mii->slot[slot].drv) {
				on = mii->slot[slot].drv->access(mii,
							&mii->slot[slot], addr, *byte, write);
				if (!write)
					*byte = on;
			}
		}	break;
/*
 SATHER-SATHER-SATHER-SATHER-SATHER-SATHER-SATHER-SATHER-SATHER-SATHER

 Writing to high RAM is enabled when the HRAMWRT' soft switch is reset.
 The controlling MPU program must set the PRE-WRITE soft switch before
 it can reset HRAMWRT'. PRE-WRITE is set in the by odd read access ub the
 C08X range. It is reset by even read access or any write access in the
 $C08X range.
 HRAMWRT' is reset by odd read access in the $C08X range when PRE-WRITE is
 set. It is set by even acce.ss in the $C08X range. Any other type of access
 causes HRAMWRT' to hold its current state.
*/
		case 0xc080 ... 0xc08f: {
			res = true;
			const int offSwitch		= addr & 0x02;
			if (addr & 0x01) {	// Write switch
				// 0xC081, 0xC083
				if (!write) {
					if (SW_GETSTATE(mii, BSRPREWRITE)) {
						SW_SETSTATE(mii, BSRWRITE, 1);
					}
				}
				SW_SETSTATE(mii, BSRPREWRITE, !write);
				// 0xC08B
				SW_SETSTATE(mii, BSRREAD, offSwitch);
			} else {
				// 0xC080, 0xC082
				SW_SETSTATE(mii, BSRWRITE, 0);
				SW_SETSTATE(mii, BSRPREWRITE, 0);
					// 0xC082
				SW_SETSTATE(mii, BSRREAD, !offSwitch);
			}
			SW_SETSTATE(mii, BSRPAGE2, !(addr & 0x08));
			mii->mem_dirty = sw_save != mii->sw_state;
		}	break;
		case SWPAGE2OFF:
		case SWPAGE2ON:
			// ACTUAL switch is already done in mii_access_video()
			res = true; // mii_access_video(mii, addr, byte, write);
			mii->mem_dirty = true;
			break;
		case SWHIRESOFF:
		case SWHIRESON:
			// ACTUAL switch is already done in mii_access_video()
			res = true; // mii_access_video(mii, addr, byte, write);
			mii->mem_dirty = true;
			break;
		case SWSPEAKER:
			res = true;
			mii_speaker_click(&mii->speaker);
			break;
		case 0xc064 ... 0xc067: // Joystick, buttons
		case 0xc070: // Analog reset
			res = true;
			mii_analog_access(mii, &mii->analog, addr, byte, write);
			break;
		case 0xc068:
			res = true;
			// IIgs register, read by prodos tho
			break;
	}
	if (res) {
		mii_page_table_update(mii);
		return res;
	}
	if (mii->emu == MII_EMU_IIC) {
		switch (addr) {
			case 0xc020 ... 0xc02f:
				res = true;
				if (mii->bank[MII_BANK_ROM].ua.raw == mii->rom->rom) {
					MII_DEBUG_PRINTF("BANKING IIC SECOND ROM\n");
					mii->bank[MII_BANK_ROM].ua.raw = (uint8_t*)
						mii->rom->rom + (16 * 1024);
				} else {
					MII_DEBUG_PRINTF("BANKING IIC FIRST ROM\n");
					mii->bank[MII_BANK_ROM].ua.raw = (uint8_t*)mii->rom->rom;
				}
				return res;
				break;
		}
	}
	if (write) {
		switch (addr) {
			case SW80STOREOFF:
			case SW80STOREON:
				res = true;
				SW_SETSTATE(mii, SW80STORE, addr & 1);
				mii_bank_poke(sw, SW80STORE, (addr & 1) << 7);
				break;
			case SWRAMRDOFF:
			case SWRAMRDON:
				res = true;
				SW_SETSTATE(mii, SWRAMRD, addr & 1);
				mii_bank_poke(sw, SWRAMRD, (addr & 1) << 7);
				break;
			case SWRAMWRTOFF:
			case SWRAMWRTON:
				res = true;
				SW_SETSTATE(mii, SWRAMWRT, addr & 1);
				mii_bank_poke(sw, SWRAMWRT, (addr & 1) << 7);
				break;
			case SWALTPZOFF:
			case SWALTPZON:
				res = true;
				SW_SETSTATE(mii, SWALTPZ, addr & 1);
				mii_bank_poke(sw, SWALTPZ, (addr & 1) << 7);
				break;
			case SWINTCXROMOFF:
			case SWINTCXROMON:
				res = true;
				if (mii->emu == MII_EMU_IIC) {
					// IIc always has the internal rom on, obs
					SW_SETSTATE(mii, SWINTCXROM, 1);
					break;
				}
				SW_SETSTATE(mii, SWINTCXROM, addr & 1);
				mii_bank_poke(sw, SWINTCXROM, (addr & 1) << 7);
				break;
			case SWSLOTC3ROMOFF:
			case SWSLOTC3ROMON:
				res = true;
				SW_SETSTATE(mii, SWSLOTC3ROM, addr & 1);
				mii_bank_poke(sw, SWSLOTC3ROM, (addr & 1) << 7);
				break;
#if !MII_RP2350
			case SWRAMWORKS_BANK:	// 0xc073
			/*
			 * From the reading, it seems only Proterm ever assumes these
			 * are the same. For all other software, everyone only ever use
			 * $c073 as a bank register. Only added these for completeness,
			 * and they don't seem to break anything so...
			 */
			case SWRAMWORKS_ALT1:
			case SWRAMWORKS_ALT5:
			case SWRAMWORKS_ALT7:
				mii_bank_poke(sw, SWRAMWORKS_BANK, *byte);
				mii_bank_update_ramworks(mii, *byte);
				break;
#endif
		}
		mii->mem_dirty += sw_save != mii->sw_state;
	} else {
		switch (addr) {
			case SWBSRBANK2:
				SW_READ(*byte, mii, BSRPAGE2);
				res = true;
				break;
			case SWBSRREADRAM:
				SW_READ(*byte, mii, BSRREAD);
				res = true;
				break;
			case SWPAGE2OFF:
			case SWPAGE2ON:	// already done by the video code
				res = true;
				break;
			case SWRAMRD:
			case SWRAMWRT:
			case SW80STORE:
			case SWINTCXROM:
			case SWALTPZ:
				res = true;
				*byte |= mii_bank_peek(sw, addr);
				break;
			case SWSLOTC3ROM:
				res = true;
				if (mii->emu == MII_EMU_IIC) {
					break;
				}
				*byte |= mii_bank_peek(sw, addr);
				break;
			case 0xc068:
				res = true;
				// IIgs register, read by prodos tho
				break;
			// toggle TAPE output ?!?!
			// IIc is switch ROM banking
			case 0xc020 ... 0xc02f:
				res = true;
				break;
			default:
				res = true;
			//	if (addr != 0xc00b)
			//		printf("VAPOR LOCK %04x\n", addr);
				// this doesn't work. Well it does the job of returning
				// something semi random, but it's not ready to be a TRUE
				// vapor lock.
				*byte = mii_video_get_vapor(mii);
#if 0
				/*
				 * this is moderately important, return some random value
				 * as it is supposed to represent what's on the bus at the time,
				 * typically video being decoded etc.
				 */
//				*byte = mii->random[mii->random_index++];
//				mii->random_index &= 0xff;
#endif
				break;
		}
	}
	mii_page_table_update(mii);
	return res;
}

// External function to get currently held key (from main.c)
extern uint8_t get_held_key(void);

/*
 * Keyboard (and joystick buttons) related access. The soft switches
 * from 0xc000 to 0xc01f all return the ascii value in the 7 lower bits.
 * The 8th bit is set when the key is pressed
 */
static bool
mii_access_keyboard(
		mii_t *mii,
		uint16_t addr,
		uint8_t * byte,
		bool write)
{
	bool res = false;
	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	if (!write && (addr & 0xff) <= 0x1f) {
		*byte = mii_bank_peek(sw, SWKBD);
	}
	switch (addr) {
		case SWKBD:
			if (!write) {
				res = true;
				*byte = mii_bank_peek(sw, SWAKD);
			}
			break;
		case SWAKD: {
			res = addr == SWAKD;
			uint8_t r = mii_bank_peek(sw, SWAKD);
			if (!write)
				*byte = r;
			// Clear strobe (bit 7) - do NOT re-latch here
			// Games expect strobe to stay clear until a new key event
			mii_bank_poke(sw, SWAKD, r & 0x7f);
		}	break;
		case 0xc061 ... 0xc063: // Push Button 0, 1, 2 (Apple Keys)
			res = true;
			if (!write) {
				*byte = mii_bank_peek(sw, addr);
			}
			break;
	}
	return res;
}

void
mii_keypress(
		mii_t *mii,
		uint8_t key)
{
	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	mii_bank_poke(sw, SWAKD, key | 0x80);
	mii_bank_poke(sw, SWKBD, key & 0x7f);
}

#if !MII_RP2350
/* ramworks came populated in chunks, this duplicates these rows of chips */
#define B(x) ((unsigned __int128)1ULL << (x))
static const unsigned __int128 _mii_ramworks3_config[] = {
	B(0x00)|B(0x01)|B(0x02)|B(0x03),
	B(0x04)|B(0x05)|B(0x06)|B(0x07),
	B(0x08)|B(0x09)|B(0x0a)|B(0x0b),
	B(0x0c)|B(0x0d)|B(0x0e)|B(0x0f),
	// 512K Expander
	B(0x10)|B(0x11)|B(0x12)|B(0x13),
	B(0x14)|B(0x15)|B(0x16)|B(0x17),
	// 2MB Expander A to one meg
	B(0x30),B(0x31),B(0x32),B(0x33),
	B(0x34),B(0x35),B(0x36),B(0x37),
	// 2MB Expander B
	B(0x50),B(0x51),B(0x52),B(0x53),
	B(0x54),B(0x55),B(0x56),B(0x57),
	B(0x70),B(0x71),B(0x72),B(0x73),
	B(0x74),B(0x75),B(0x76),B(0x77),
};
#undef B
#endif

#if MII_RP2350

extern bool volatile vram_locked;

void
mii_init(
		mii_t *mii )
{
	MII_DEBUG_PRINTF("  mii_init: clearing struct...\n");
	memset(mii, 0, sizeof(*mii));
	mii->speed = MII_SPEED_NTSC;
	mii->timer.map = 0;

//	__dmb();          // Data Memory Barrier
//	vram_locked = true;

	MII_DEBUG_PRINTF("  mii_init: setting up banks...\n");
	for (int i = 0; i < MII_BANK_COUNT; i++) {
		mii->bank[i] = _mii_banks_init[i];
	}

	// Initialize banks (won't allocate since no_alloc is set)
	MII_DEBUG_PRINTF("  mii_init: initializing bank memory...\n");

	MII_DEBUG_PRINTF("    Clearing main memory (64KB)\n");
	init_ram_pages_for(&main_vram_d, vram, RAM_PAGES_PER_POOL * RAM_PAGE_SIZE);
	MII_DEBUG_PRINTF("    Clearing aux memory (52KB)\n");
	init_ram_pages_for(&aux_vram_d, vram, RAM_PAGES_PER_POOL * RAM_PAGE_SIZE);

	vram_locked = false;
	__dmb();          // Data Memory Barrier

	MII_DEBUG_PRINTF("    Clearing soft switch area\n");
	memset(rp2350_sw_mem, 0, sizeof(rp2350_sw_mem));
	MII_DEBUG_PRINTF("    Clearing card ROM area\n");
	// Unpopulated slots are effectively open-bus; $FF avoids accidental BRKs
	// when firmware probes/executes slot ROM entry points.
	memset(rp2350_card_rom, 0xFF, sizeof(rp2350_card_rom));

	mii->cpu.trap = MII_TRAP;
	
	// Skip desktop-specific subsystems on RP2350:
	// - mii_dd_system_init (disk drive)
	// - mii_audio_init (audio subsystem)
	// - mii_speaker_init (speaker)
	// Note: mii_analog_init IS needed for paddle/joystick timer-based timing!
	mii_analog_init(mii, &mii->analog);
	// Note: mii_video_init IS needed for VBL timer (games depend on VBL timing)
	// Our version registers a lightweight VBL-only timer, not the full renderer
	mii_video_init(mii);

	MII_DEBUG_PRINTF("  mii_init: resetting CPU...\n");
	mii_reset(mii, true);
	mii->cpu_state = mii_cpu_init(&mii->cpu);
	
	mii->cpu.access_param = mii;
	mii->cpu.access = _mii_cpu_direct_access_cb;
	
	for (int i = 0; i < 7; i++)
		mii->slot[i].id = i;
	
	MII_DEBUG_PRINTF("  mii_init: complete\n");
}

#else
// Original desktop version
void
mii_init(
		mii_t *mii )
{
	memset(mii, 0, sizeof(*mii));
	mii->speed = MII_SPEED_NTSC;
	mii->timer.map = 0;

	for (int i = 0; i < MII_BANK_COUNT; i++)
		mii->bank[i] = _mii_banks_init[i];
//	mii->bank[MII_BANK_ROM].mem = (uint8_t*)&mii_rom_iiee[0];
	for (int i = 0; i < MII_BANK_COUNT; i++)
		mii_bank_init(&mii->bank[i]);
	uint8_t *mem = realloc(mii->bank[MII_BANK_MAIN].mem, 0x10000);
	mii->bank[MII_BANK_MAIN].mem = mem;
	mii->bank[MII_BANK_BSR].mem = mem;
	mii->bank[MII_BANK_BSR_P2].mem = mem;
	mii->ramworks.avail = 0;
	mii_bank_update_ramworks(mii, 0);

	mii->cpu.trap = MII_TRAP;
	// these are called once, regardless of reset
	mii_dd_system_init(mii, &mii->dd);
	mii_analog_init(mii, &mii->analog);
	mii_video_init(mii);
	mii_audio_init(mii, &mii->audio);
	mii_speaker_init(mii, &mii->speaker);

	mii_reset(mii, true);
	mii->cpu_state = mii_cpu_init(&mii->cpu);
#if MII_65C02_DIRECT_ACCESS
	mii->cpu.access_param = mii;
	mii->cpu.access = _mii_cpu_direct_access_cb;
#endif
	for (int i = 0; i < 7; i++)
		mii->slot[i].id = i;
	mii_bank_install_access_cb(&mii->bank[MII_BANK_ROM],
			_mii_select_c3introm, mii, 0xc3, 0xc3);
}
#endif

#if !MII_RP2350
void
mii_prepare(
		mii_t *mii,
		uint32_t flags )
{
//	if (mii->emu == MII_EMU_IIC) {
//		printf("IIC Mode engaged\n");
//		mii->bank[MII_BANK_ROM].mem = (uint8_t*)&mii_rom_iic[0];
//	}

//	int banks = (flags >> MII_INIT_RAMWORKS_BIT) & 0xf;
	// hard code it for now, doesn't seem to have any detrimental effect
	int banks = 12;
	if (banks > 12)
		banks = 12;
	for (int i = 0; i < banks; i++)	// add available banks
		mii->ramworks.avail |= _mii_ramworks3_config[i];

	mii_slot_drv_t * drv = mii_slot_drv_list;
	while (drv) {
		MII_DEBUG_PRINTF("%s driver: %s\n", __func__, drv->name);
		if (drv->probe && drv->probe(mii, flags)) {
		//	printf("%s %s probe done\n", __func__, drv->name);
		}
		drv = drv->next;
	}
	mii_audio_start(&mii->audio);
}

void
mii_dispose(
		mii_t *mii )
{
	for (int i = 0; i < 7; i++) {
		if (mii->slot[i].drv && mii->slot[i].drv->dispose)
			mii->slot[i].drv->dispose(mii, &mii->slot[i]);
	}
	for (int i = 0; i < MII_BANK_COUNT; i++)
		mii_bank_dispose(&mii->bank[i]);
	for (int i = 0; i < 128; i++ ) {
		if (mii->ramworks.bank[i]) {
			free(mii->ramworks.bank[i]);
			mii->ramworks.bank[i] = NULL;
		}
	}
	mii_speaker_dispose(&mii->speaker);
	mii_audio_dispose(&mii->audio);
	mii_dd_system_dispose(&mii->dd);
	mii->state = MII_INIT;
}
#endif

void
mii_reset(
		mii_t *mii,
		bool cold)
{
//	printf("%s cold %d\n", __func__, cold);
	mii->rom = mii_rom_get("iiee");
	if (mii->emu == MII_EMU_IIC) {
		MII_DEBUG_PRINTF("IIC Mode engaged\n");
		mii->rom = mii_rom_get("iic");
	}
	mii->bank[MII_BANK_ROM].ua.raw = (uint8_t*)mii->rom->rom;
	mii->state = MII_RUNNING;
	mii->cpu_state.reset = 1;
	mii_bank_t * main = &mii->bank[MII_BANK_MAIN];
	mii_bank_t * sw = &mii->bank[MII_BANK_SW];
	/*
	 * Default soft-switch states on reset.
	 *
	 * Keep INTCXROM ON during reset/boot so the internal firmware view of
	 * the $C100-$CFFF space matches what the IIe ROM expects (stable BASIC boot).
	 * Disk boot/PR#6 can explicitly turn INTCXROM OFF once the system is up.
	 */
	mii->sw_state = M_BSRWRITE | M_BSRPAGE2 | M_SWINTCXROM;
	mii_bank_poke(sw, SWSLOTC3ROM, 0);
	mii_bank_poke(sw, SWRAMRD, 0);
	mii_bank_poke(sw, SWRAMWRT, 0);
	mii_bank_poke(sw, SWALTPZ, 0);
	mii_bank_poke(sw, SW80STORE, 0);
	mii_bank_poke(sw, SW80COL, 0);
	mii_bank_poke(sw, SWINTCXROM, 0x80);
	mii_bank_poke(sw, SWRAMWORKS_BANK, 0);
	// Clear video soft switches on reset (fixes games checking mode on boot)
	mii_bank_poke(sw, SWTEXT, 0);
	mii_bank_poke(sw, SWMIXED, 0);
	mii_bank_poke(sw, SWHIRES, 0);
	mii_bank_poke(sw, SWPAGE2, 0);
	mii_bank_poke(sw, SWALTCHARSET, 0);
	mii_bank_poke(sw, SWRDDHIRES, 0);
	mii_bank_poke(sw, SWVBL, 0x80);  // VBL starts in active display
	// Clear button states on reset (fixes games seeing button as pressed)
	mii_bank_poke(sw, 0xc061, 0);  // Button 0
	mii_bank_poke(sw, 0xc062, 0);  // Button 1
	mii_bank_poke(sw, 0xc063, 0);  // Button 2
	// Clear keyboard state (fixes games skipping intro/title screens)
	mii_bank_poke(sw, SWKBD, 0);   // Keyboard data
	mii_bank_poke(sw, SWAKD, 0);   // Any key down (strobe)
	mii->mem_dirty = 1;
	if (cold) {
		/*  these HAS to be reset in that state somehow */
		/*
		 * Apple IIe ROM uses $03F2-$03F4 to detect and execute a WARM start.
		 * If we leave a valid signature here, the ROM will skip cold init
		 * (no "Apple //e" banner, no screen clear) and jump straight into BASIC.
		 *
		 * For a true cold boot experience, invalidate the warm-start signature
		 * and keep the vector low byte at 0 to avoid accidental indirect jumps.
		 */
		uint8_t warm_start_sig[3] = { 0x00, 0x00, 0x00 };
		mii_bank_write(main, 0x3f2, warm_start_sig, sizeof(warm_start_sig));
	//	mii_bank_write(main, 0x3fe, z, 2); // also reset IRQ vectors
	}
	mii->mem_dirty = 1;
	mii_page_table_update(mii);
	for (int i = 0; i < 7; i++) {
		if (mii->slot[i].drv && mii->slot[i].drv->reset)
			mii->slot[i].drv->reset(mii, &mii->slot[i]);
	}
}

void
mii_mem_access(
		mii_t *mii,
		uint16_t addr,
		uint8_t * d,
		bool wr,
		bool do_sw)
{
	if (!do_sw && addr >= 0xc000 && addr <= 0xc0ff && addr != 0xcfff)
		return;
	
#ifdef MII_RP2350
	// Fast path: check address range first to minimize function calls
	// Slot I/O: $C090-$C0FF - most frequent during disk access
	if (addr >= 0xc090 && addr <= 0xc0ff) {
		int slot = ((addr >> 4) & 7) - 1;
		if (mii->slot[slot].drv) {
			uint8_t on = mii->slot[slot].drv->access(mii,
						&mii->slot[slot], addr, *d, wr);
			if (!wr)
				*d = on;
		}
		return;
	}
	
	// Keyboard: $C000-$C01F and Apple/joystick buttons: $C061-$C063
	uint8_t low = addr & 0xff;
	if (low <= 0x1f || (low >= 0x61 && low <= 0x63)) {
		if (mii_access_keyboard(mii, addr, d, wr))
			return;
	}
	
	// Check other handlers
	uint8_t done =
		_mii_deselect_cXrom(mii, addr, d, wr) ||
		mii_access_video(mii, addr, d, wr) ||
		mii_access_soft_switches(mii, addr, d, wr);
	if (done)
		return;
#else
	uint8_t done =
		_mii_deselect_cXrom(mii, addr, d, wr) ||
		mii_access_keyboard(mii, addr, d, wr) ||
		mii_access_video(mii, addr, d, wr) ||
		mii_access_soft_switches(mii, addr, d, wr);
	if (done)
		return;
#endif

	uint8_t page = addr >> 8;
	if (wr) {
		uint8_t m = mii->mem[page].write;
		mii_bank_t * b = &mii->bank[m];
		if (!b->ro)
			mii_bank_write(b, addr, d, 1);
		else {
			mii_bank_access(b, addr, d, 1, true);
		}
	} else {
		uint8_t m = mii->mem[page].read;
		mii_bank_t * b = &mii->bank[m];
		*d = mii_bank_peek(b, addr);
	}
}

static void
_mii_handle_trap(
		mii_t *mii)
{
//	printf("%s TRAP hit PC: %04x\n", __func__, mii->cpu.PC);
	mii->cpu_state.sync = 1;
	mii->cpu_state.trap = 0;
#if MII_65C02_DIRECT_ACCESS == 0
	mii->cpu.state = NULL;
#endif
	uint8_t trap = mii_read_one(mii, mii->cpu.PC);
	mii->cpu.PC += 1;
//	printf("%s TRAP %02x return PC %04x\n", __func__, trap, mii->cpu.PC);
	if (mii->trap.map & (1 << trap)) {
		if (mii->trap.trap[trap].cb)
			mii->trap.trap[trap].cb(mii, trap);
	} else {
		MII_DEBUG_PRINTF("%s TRAP %02x not handled\n", __func__, trap);
		mii->state = MII_STOPPED;
	}
//	mii->state = MII_STOPPED;
}

uint8_t
mii_register_trap(
		mii_t *mii,
		mii_trap_handler_cb cb)
{
	if (mii->trap.map == 0xffff) {
		MII_DEBUG_PRINTF("%s no more traps!!\n", __func__);
		return 0xff;
	}
	for (int i = 0; i < (int)sizeof(mii->trap.map) * 8; i++) {
		if (!(mii->trap.map & (1 << i))) {
			mii->trap.map |= 1 << i;
			mii->trap.trap[i].cb = cb;
			return i;
		}
	}
	return 0xff;
}

uint8_t
mii_timer_register(
		mii_t *mii,
		mii_timer_p cb,
		void *param,
		int64_t when,
		const char *name)
{
	if (mii->timer.map == (uint64_t)-1ll) {
		MII_DEBUG_PRINTF("%s no more timers!!\n", __func__);
		return 0xff;
	}
	int i = ffsll(~mii->timer.map) - 1;
	mii->timer.map |= 1ull << i;
	mii->timer.timers[i].cb = cb;
	mii->timer.timers[i].param = param;
	mii->timer.timers[i].when = when;
	mii->timer.timers[i].name = name;
	return i;
}

int64_t
mii_timer_get(
		mii_t *mii,
		uint8_t timer_id)
{
	if (timer_id >= (int)sizeof(mii->timer.map) * 8)
		return 0;
	return mii->timer.timers[timer_id].when;
}

int
mii_timer_set(
		mii_t *mii,
		uint8_t timer_id,
		int64_t when)
{
	if (timer_id >= (int)sizeof(mii->timer.map) * 8)
		return -1;
	mii->timer.timers[timer_id].when = when;
	return 0;
}

void
mii_timer_run(
		mii_t *mii,
		uint64_t cycles)
{
#ifdef MII_RP2350
	// Process ALL active timers
	uint64_t timer = mii->timer.map;
	if (!timer) return;
	
	for (int i = 0; i < 64 && timer; i++) {
		if (timer & (1ull << i)) {
			timer &= ~(1ull << i);
			if (mii->timer.timers[i].when > 0) {
				mii->timer.timers[i].when -= cycles;
				if (mii->timer.timers[i].when <= 0) {
					if (mii->timer.timers[i].cb) {
						uint64_t period = mii->timer.timers[i].cb(mii,
								mii->timer.timers[i].param);
						// If timer got very behind (missed multiple periods),
						// just reset to the period instead of adding to huge negative
						if (mii->timer.timers[i].when < -(int64_t)period) {
							mii->timer.timers[i].when = period;
						} else {
							mii->timer.timers[i].when += period;
						}
					} else {
						// Timer with no callback (like paddle timers) - just stop at 0
						mii->timer.timers[i].when = 0;
					}
				}
			}
		}
	}
#else
	uint64_t timer = mii->timer.map;
	while (timer) {
		int i = ffsll(timer) - 1;
		timer &= ~(1ull << i);
		if (mii->timer.timers[i].when > 0) {
			mii->timer.timers[i].when -= cycles;
			if (mii->timer.timers[i].when <= 0) {
				if (mii->timer.timers[i].cb)
					mii->timer.timers[i].when += mii->timer.timers[i].cb(mii,
							mii->timer.timers[i].param);
			}
		}
	}
#endif
}

uint8_t
mii_irq_register(
		mii_t *mii,
		const char *name )
{
	if (mii->irq.map == 0xff) {
		MII_DEBUG_PRINTF("%s no more IRQs!!\n", __func__);
		return 0xff;
	}
	for (int i = 0; i < (int)sizeof(mii->irq.map) * 8; i++) {
		if (!(mii->irq.map & (1 << i))) {
			mii->irq.map |= 1 << i;
			mii->irq.irq[i].name = name;
			return i;
		}
	}
	return 0xff;
}

void
mii_irq_unregister(
		mii_t *mii,
		uint8_t irq_id )
{
	if (irq_id >= (int)sizeof(mii->irq.map) * 8)
		return;
	mii->irq.map &= ~(1 << irq_id);
}

void
mii_irq_raise(
		mii_t *mii,
		uint8_t irq_id )
{
	if (irq_id >= (int)sizeof(mii->irq.map) * 8)
		return;
	if (!(mii->irq.raised & (1 << irq_id)))
		mii->irq.irq[irq_id].count++;
	mii->irq.raised |= 1 << irq_id;
}

void
mii_irq_clear(
		mii_t *mii,
		uint8_t irq_id )
{
	if (irq_id >= (int)sizeof(mii->irq.map) * 8)
		return;
	mii->irq.raised &= ~(1 << irq_id);
}


#if MII_65C02_DIRECT_ACCESS == 0
#error "MII_65C02_DIRECT_ACCESS *has* to be enabled here"
#endif

#if MII_RP2350
/*
 * Optimized CPU access callback for RP2350.
 * This is the HOT PATH - called for every single memory access!
 * Optimizations:
 * 1. No debug/breakpoint support (saves loop iteration)
 * 2. No trace logging (saves memory writes)
 * 3. Timer run batched every N cycles
 * 4. Fast path for RAM and ROM (direct memory access)
 * 5. Only I/O ($C000-$C0FF) goes through slow path
 */
static mii_cpu_state_t
_mii_cpu_direct_access_cb(
		struct mii_cpu_t *cpu,
		mii_cpu_state_t   access )
{
	mii_t *mii = cpu->access_param;
	mii->cpu_state = access;
	
	const uint16_t addr = access.addr;
	const uint8_t page = addr >> 8;
	
	// Timers are run from inline macros in mii_65c02.c for direct access mode
	// Only run here if we somehow got called through the callback
#if !MII_65C02_DIRECT_ACCESS || !MII_RP2350
	uint64_t total = mii->cpu.total_cycle + mii->cpu.cycle;
	uint64_t last = mii->timer.last_run;
	if (total - last >= 8) {
		mii_timer_run(mii, total - last);
		mii->timer.last_run = total;
	}
#endif
	
	// Fast path for non-I/O memory (RAM < $C000 or ROM $C100-$FFFF)
	// Only $C000-$C0FF is I/O that needs special handling
	if (likely(page != 0xC0)) {
		if (access.w) {
			// Write
			uint8_t m = mii->mem[page].write;
			mii_bank_t *b = &mii->bank[m];
			if (likely(!b->ro)) {
				mii_bank_poke(b, addr, access.data);
			}
		} else {
			// Read
			uint8_t m = mii->mem[page].read;
			mii_bank_t *b = &mii->bank[m];
			mii->cpu_state.data = mii_bank_peek(b, addr);
		}
	} else {
		// Slow path for I/O only ($C000-$C0FF)
		mii_mem_access(mii, addr, &mii->cpu_state.data, access.w, true);
	}
	
	// IRQ check
	mii->cpu_state.irq = mii->cpu_state.irq | !!mii->irq.raised;
	
	return mii->cpu_state;
}
#else
// Original desktop version with full debug support
static mii_cpu_state_t
_mii_cpu_direct_access_cb(
		struct mii_cpu_t *cpu,
		mii_cpu_state_t   access )
{
	mii_t *mii = cpu->access_param;

	mii->cpu_state = access;
	uint64_t total = mii->cpu.total_cycle + mii->cpu.cycle;
	uint64_t last = mii->timer.last_run;
	if (total - last >= 1) {
		mii_timer_run(mii, total - last);
		mii->timer.last_run = total;
	}

	const uint16_t addr = access.addr;
	int wr = access.w;

	if (access.sync) {
		mii->trace.log[mii->trace.idx] = mii->cpu.PC;
		mii->trace.idx = (mii->trace.idx + 1) & (MII_PC_LOG_SIZE - 1);
	}
	if (unlikely(mii->debug.bp_map)) {
		for (int i = 0; i < (int)sizeof(mii->debug.bp_map) * 8; i++) {
			if (!(mii->debug.bp_map & (1 << i)))
				continue;
			if (addr >= mii->debug.bp[i].addr &&
					addr < mii->debug.bp[i].addr + mii->debug.bp[i].size) {
				if (((mii->debug.bp[i].kind & MII_BP_R) && !wr) ||
					((mii->debug.bp[i].kind & MII_BP_W) && wr)) {
					if (1 || !mii->debug.bp[i].silent) {
						MII_DEBUG_PRINTF("BREAKPOINT %d at %04x PC:%04x\n",
							i, addr, mii->cpu.PC);
						mii_dump_run_trace(mii);
						mii_dump_trace_state(mii);
						mii->cpu.instruction_run = 0;
						mii->state = MII_STOPPED;
					}
				}
				if (!(mii->debug.bp[i].kind & MII_BP_STICKY))
					mii->debug.bp_map &= ~(1 << i);
				mii->debug.bp[i].kind |= MII_BP_HIT;
			}
		}
	}
	mii_mem_access(mii, addr, &mii->cpu_state.data, wr, true);
	mii->cpu_state.irq = mii->cpu_state.irq | !!mii->irq.raised;
	return mii->cpu_state;
}
#endif // MII_RP2350

void
mii_run(
		mii_t *mii)
{
	/* this runs all cycles for one instruction */
	if (unlikely(mii->state != MII_RUNNING || mii->trace_cpu > 1)) {
//		printf("tracing\n");
		mii->cpu.instruction_run = 0;
	} else
		mii->cpu.instruction_run = 100000;

	mii->cpu_state = mii_cpu_run(&mii->cpu, mii->cpu_state);

	if (unlikely(mii->cpu_state.trap))
		_mii_handle_trap(mii);
}

//! Read one byte from and addres, using the current memory mapping
uint8_t
mii_read_one(
		mii_t *mii,
		uint16_t addr)
{
	uint8_t d = 0;
	mii_mem_access(mii, addr, &d, 0, false);
	return d;
}
//! Read a word from addr, using current memory mapping (little endian)
uint16_t
mii_read_word(
		mii_t *mii,
		uint16_t addr)
{
	uint8_t d = 0;
	uint16_t res = 0;
	mii_mem_access(mii, addr, &d, 0, false);
	res = d;
	mii_mem_access(mii, addr + 1, &d, 0, false);
	res |= d << 8;
	return res;
}
/* same accessors, for write
 */
void
mii_write_one(
		mii_t *mii,
		uint16_t addr,
		uint8_t d)
{
	mii_mem_access(mii, addr, &d, 1, false);
}
void
mii_write_word(
		mii_t *mii,
		uint16_t addr,
		uint16_t w)
{
	uint8_t d = w;
	mii_mem_access(mii, addr, &d, 1, false);
	d = w >> 8;
	mii_mem_access(mii, addr + 1, &d, 1, false);
}

void
mii_cpu_step(
		mii_t *mii,
		uint32_t count )
{
	if (mii->state != MII_STOPPED) {
		MII_DEBUG_PRINTF("mii: can't step/next, not stopped\n");
		return;
	}
	mii->trace.step_inst = count ? count : 1;
	__sync_synchronize();
	mii->state = MII_STEP;
}

void
mii_cpu_next(
		mii_t *mii)
{
	if (mii->state != MII_STOPPED) {
		MII_DEBUG_PRINTF("mii: can't step/next, not stopped\n");
		return;
	}
	// read current opcode, find how how many bytes it take,
	// then put a temporary breakpoint to the next PC.
	// all of that if this is not a relative branch of course, in
	// which case we use a normal 'step' behaviour
	uint8_t op = 0;
	mii_mem_access(mii, mii->cpu.PC, &op, false, false);
	MII_DEBUG_PRINTF("NEXT opcode %04x:%02x\n", mii->cpu.PC, op);
	if (op == 0x20) {	// JSR here?
		// set a temp breakpoint on reading 3 bytes from PC
		if (mii->debug.bp_map != (uint16_t)-1) {
			int i = ffsl(~mii->debug.bp_map) - 1;
			mii->debug.bp[i].addr = mii->cpu.PC + 3;
			mii->debug.bp[i].kind = MII_BP_R;
			mii->debug.bp[i].size = 1;
			mii->debug.bp[i].silent = 1;
			mii->debug.bp_map |= 1 << i;
			__sync_synchronize();
			mii->state = MII_RUNNING;
			return;
		}
		MII_DEBUG_PRINTF("%s no more breakpoints available\n", __func__);
	} else {
		mii_cpu_step(mii, 1);
	}
}

#if MII_RP2350
// Optimized run loop for RP2350 - inlines mii_run to reduce function call overhead
void
mii_run_cycles(
		mii_t *mii,
		uint32_t cycles)
{
	uint64_t target_cycle = mii->cpu.total_cycle + cycles;
	
	while (mii->cpu.total_cycle < target_cycle && mii->state == MII_RUNNING) {
		// Set instruction_run to approximate cycles we need (avg ~3 cycles/instruction)
		// This limits overshoot while still batching instructions
		uint64_t remaining = target_cycle - mii->cpu.total_cycle;
		mii->cpu.instruction_run = (remaining < 300) ? (remaining / 3) + 1 : 100;
		
		mii->cpu_state = mii_cpu_run(&mii->cpu, mii->cpu_state);
		
		if (unlikely(mii->cpu_state.trap)) {
			_mii_handle_trap(mii);
		}
		
		// Run timers periodically even without I/O access
		// This is needed for VBL timing when code does tight loops without I/O
		uint64_t total = mii->cpu.total_cycle + mii->cpu.cycle;
		uint64_t last = mii->timer.last_run;
		if (total > last + 1000) {  // Every ~1000 cycles
			mii_timer_run(mii, total - last);
			mii->timer.last_run = total;
		}
	}
}
#endif