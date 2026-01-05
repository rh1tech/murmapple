/*
 * mii_vcd_stub.c
 *
 * Stub VCD (Value Change Dump) functions for RP2350
 * VCD is used for signal tracing/debugging, not needed on embedded Pico.
 *
 * Based on mii_vcd.c Copyright (C) 2023 Michel Pollet
 * SPDX-License-Identifier: MIT
 */

#include "mii.h"
#include "mii_vcd.h"

// Stub implementations - VCD debugging disabled on Pico

int
mii_vcd_init(
		mii_t *mii,
		const char *filename,
		mii_vcd_t *vcd,
		uint32_t cycle_tick_ns)
{
	(void)mii; (void)filename; (void)vcd; (void)cycle_tick_ns;
	// VCD disabled on Pico
	return -1;
}

int
mii_vcd_init_input(
		mii_t *mii,
		const char *filename,
		mii_vcd_t *vcd)
{
	(void)mii; (void)filename; (void)vcd;
	return -1;
}

void
mii_vcd_close(
		mii_vcd_t *vcd)
{
	(void)vcd;
	// VCD disabled on Pico
}

mii_signal_t *
mii_alloc_signal(
		mii_signal_pool_t *pool,
		uint32_t base,
		uint32_t count,
		const char **names)
{
	(void)pool; (void)base; (void)count; (void)names;
	// Return NULL - signals not allocated on Pico
	return NULL;
}

void
mii_free_signal(
		mii_signal_t *sig,
		uint32_t count)
{
	(void)sig; (void)count;
}

void
mii_init_signal(
		mii_signal_pool_t *pool,
		mii_signal_t *sig,
		uint32_t base,
		uint32_t count,
		const char **names)
{
	(void)pool; (void)sig; (void)base; (void)count; (void)names;
}

uint8_t
mii_signal_get_flags(
		mii_signal_t *sig)
{
	(void)sig;
	return 0;
}

void
mii_signal_set_flags(
		mii_signal_t *sig,
		uint8_t flags)
{
	(void)sig; (void)flags;
}

// mii_raise_signal and mii_raise_signal_float are now macros in mii_vcd.h
// No function implementations needed

void
mii_connect_signal(
		mii_signal_t *src,
		mii_signal_t *dst)
{
	(void)src; (void)dst;
}

void
mii_unconnect_signal(
		mii_signal_t *src,
		mii_signal_t *dst)
{
	(void)src; (void)dst;
}

int
mii_vcd_add_signal(
		mii_vcd_t *vcd,
		mii_signal_t *sig,
		uint width,
		const char *name)
{
	(void)vcd; (void)sig; (void)width; (void)name;
	return -1;  // VCD disabled
}
int
mii_vcd_start(
		mii_vcd_t *vcd)
{
	(void)vcd;
	return -1;  // VCD disabled
}

int
mii_vcd_stop(
		mii_vcd_t *vcd)
{
	(void)vcd;
	return 0;
}