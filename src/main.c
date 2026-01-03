/*
 * main.c - Apple IIe Emulator for RP2350
 * Entry point with overclocking, PSRAM, and HDMI initialization
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/dma.h"  // Include DMA header early before mii_sw.h

#include "board_config.h"
#include "../drivers/psram_allocator.h"
#include "../drivers/HDMI.h"
#include "mii.h"
#include "mii_sw.h"
#include "mii_rom.h"
#include "mii_video.h"
#include "mii_65c02.h"
#include "mii_analog.h"
#include "mii_speaker.h"

// Stubs for desktop-only functions we don't use on RP2350
void mii_analog_access(struct mii_t *mii, mii_analog_t *analog,
                       uint16_t addr, uint8_t *byte, bool write) {
    (void)mii; (void)analog; (void)addr; (void)byte; (void)write;
    // No analog (joystick) support on RP2350 yet
}

void mii_speaker_click(mii_speaker_t *speaker) {
    (void)speaker;
    // No speaker support on RP2350 yet
}

int mii_cpu_disasm_one(char *buf, size_t buflen, mii_cpu_t *cpu,
                       uint8_t (*read_byte)(void*, uint16_t), void *param) {
    (void)buf; (void)buflen; (void)cpu; (void)read_byte; (void)param;
    return 0;  // No disassembly support
}

// External ROM data (from ROM files)
extern const uint8_t mii_rom_iiee[];
extern const uint8_t mii_rom_iiee_video[];

// HDMI framebuffer dimensions (driver line-doubles to 640x480)
#undef HDMI_WIDTH
#undef HDMI_HEIGHT
#define HDMI_WIDTH 320
#define HDMI_HEIGHT 240

// PSRAM interface
extern void psram_init(uint cs_pin);
extern void psram_set_sram_mode(int enable);

// PS/2 keyboard interface
extern void ps2kbd_init(void);
extern void ps2kbd_tick(void);
extern int ps2kbd_get_key(int* pressed, unsigned char* key);

// Flash timing configuration for overclocking
#define FLASH_MAX_FREQ_MHZ 88

static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz) {
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = FLASH_MAX_FREQ_MHZ * 1000000;
    
    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000) {
        divisor = 2;
    }
    
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000) {
        rxdelay += 1;
    }
    
    qmi_hw->m[0].timing = 0x60007000 |
                        rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                        divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

// Global emulator state
static mii_t g_mii;

// HDMI framebuffer
static uint8_t *g_hdmi_buffer = NULL;

// Apple II color palette (RGB888)
static const uint32_t apple2_rgb888[16] = {
    0x000000, // Black
    0xDD0033, // Magenta
    0x000099, // Dark Blue
    0xDD22DD, // Purple
    0x007722, // Dark Green
    0x555555, // Grey1
    0x2222FF, // Medium Blue
    0x66AAFF, // Light Blue
    0x885500, // Brown
    0xFF6600, // Orange
    0xAAAAAA, // Grey2
    0xFF9988, // Pink
    0x11DD00, // Light Green
    0xFFFF00, // Yellow
    0x44FF99, // Aqua
    0xFFFFFF, // White
};

// Initialize the HDMI palette
static void init_palette(void) {
    for (int i = 0; i < 16; i++) {
        graphics_set_palette(i, apple2_rgb888[i]);
    }
    // Fill remaining entries with grayscale
    for (int i = 16; i < 256; i++) {
        uint32_t gray = ((i & 0xE0) << 16) | ((i & 0x1C) << 11) | ((i & 0x03) << 6);
        graphics_set_palette(i, gray);
    }
}

// Process PS/2 keyboard input
static uint32_t last_key_time = 0;
static void process_keyboard(void) {
    int pressed;
    unsigned char key;
    
    while (ps2kbd_get_key(&pressed, &key)) {
        if (pressed) {
            uint32_t now = time_us_32();
            uint32_t delta = now - last_key_time;
            printf("KEY: 0x%02X delta=%lu us\n", key, delta);
            last_key_time = now;
            mii_keypress(&g_mii, key);
        }
    }
}

// Flag to indicate emulator is ready
static volatile bool g_emulator_ready = false;

// Core 1 - Video rendering loop
static void core1_main(void) {
    printf("Core 1: Waiting for emulator ready...\n");
    
    // Wait for Core 0 to finish initialization
    while (!g_emulator_ready) {
        sleep_ms(10);
    }
    
    printf("Core 1: Starting video rendering\n");
    
    while (1) {
        sleep_ms(16);
        
        // Render Apple II video
        mii_video_render(&g_mii);
        
        // Scale and copy to HDMI framebuffer
        mii_video_scale_to_hdmi(&g_mii.video, g_hdmi_buffer);
    }
}

// Static ROM structure for character ROM (in case auto-registration fails)
static mii_rom_t char_rom_fallback = {
    .name = "iiee_video",
    .class = "video",
    .description = "Apple IIe Video ROM",
    .rom = NULL, // Will be set in load_char_rom
    .len = 4096,
};

// Static ROM structure for main Apple IIe ROM
static mii_rom_t main_rom_struct = {
    .name = "iiee",
    .class = "main",
    .description = "Apple IIe Enhanced ROM",
    .rom = NULL, // Will be set in load_rom
    .len = 16384,
};

// Load ROM into memory bank and register with ROM system
static void load_rom(mii_t *mii, const uint8_t *rom, size_t len, uint16_t addr) {
    mii_bank_t *rom_bank = &mii->bank[MII_BANK_ROM];
    for (size_t i = 0; i < len; i++) {
        rom_bank->mem[rom_bank->mem_offset + addr - rom_bank->base + i] = rom[i];
    }
    
    // Register with the ROM system so mii_rom_get("iiee") works
    main_rom_struct.rom = rom;
    main_rom_struct.len = len;
    mii_rom_register(&main_rom_struct);
    
    printf("Loaded %zu bytes ROM at $%04X\n", len, addr);
}

// Load character ROM
static void load_char_rom(mii_t *mii, const uint8_t *rom, size_t len) {
    // First try the auto-registered ROM
    mii_rom_t *video_rom = mii_rom_get("iiee_video");
    if (video_rom) {
        mii->video.rom = video_rom;
        printf("Loaded %zu bytes character ROM (auto-registered)\n", len);
    } else {
        // Fall back to direct pointer
        char_rom_fallback.rom = rom;
        mii->video.rom = &char_rom_fallback;
        printf("Loaded %zu bytes character ROM (fallback)\n", len);
    }
}

int main() {
    // Overclock support: For speeds > 252 MHz, increase voltage first
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);
    sleep_ms(100);
#endif
    
    // Set system clock
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }
    
    // Initialize stdio (USB serial)
    stdio_init_all();
    
    // Wait for USB serial connection (4 seconds as requested)
    printf("\n\n");
    for (int i = 4; i > 0; i--) {
        printf("MurmApple - Starting in %d...\n", i);
        sleep_ms(1000);
    }
    
    printf("=================================\n");
    printf("  MurmApple - Apple IIe on RP2350\n");
    printf("=================================\n");
    printf("System Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    
    // Initialize PSRAM
    printf("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    psram_init(psram_pin);
    psram_set_sram_mode(0);  // Use PSRAM mode (not SRAM simulation)
    printf("PSRAM initialized on CS pin %d\n", psram_pin);
    
    // Test PSRAM read/write
    volatile uint8_t *psram = (volatile uint8_t *)0x11000000;
    psram[0] = 0xAB;
    psram[1] = 0xCD;
    psram[2] = 0xEF;
    printf("PSRAM test: wrote AB CD EF, read %02X %02X %02X\n", 
           psram[0], psram[1], psram[2]);
    if (psram[0] != 0xAB || psram[1] != 0xCD || psram[2] != 0xEF) {
        printf("ERROR: PSRAM read/write failed!\n");
    }
    
    // Allocate HDMI framebuffer in SRAM (not PSRAM!) - DMA needs fast access
    printf("Allocating HDMI framebuffer in SRAM...\n");
    g_hdmi_buffer = (uint8_t *)malloc(HDMI_WIDTH * HDMI_HEIGHT);
    if (!g_hdmi_buffer) {
        printf("ERROR: Failed to allocate HDMI framebuffer\n");
        while (1) tight_loop_contents();
    }
    printf("HDMI buffer allocated at %p (%d bytes)\n", g_hdmi_buffer, HDMI_WIDTH * HDMI_HEIGHT);
    memset(g_hdmi_buffer, 0, HDMI_WIDTH * HDMI_HEIGHT);
    
    // IMPORTANT: Set buffer and resolution BEFORE graphics_init() 
    // because DMA/IRQs start immediately and will read from the buffer
    graphics_set_buffer(g_hdmi_buffer);
    graphics_set_res(HDMI_WIDTH, HDMI_HEIGHT);
    
    // Initialize HDMI graphics (starts DMA/IRQs)
    printf("Initializing HDMI...\n");
    graphics_init(g_out_HDMI);
    printf("HDMI buffer at %p (%lux%lu)\n", 
           g_hdmi_buffer, graphics_get_width(), graphics_get_height());
    
    // Initialize palette
    init_palette();
    graphics_restore_sync_colors();  // Restore HDMI sync colors after palette init
    
    // Verify palette entry 15 was set
    printf("Palette initialized, verifying...\n");
    extern uint32_t conv_color[];
    uint64_t *conv_color64 = (uint64_t *)conv_color;
    printf("conv_color[15] = 0x%016llx 0x%016llx\n", conv_color64[30], conv_color64[31]);
    
    // Clear framebuffer to black
    memset(g_hdmi_buffer, 0, sizeof(g_hdmi_buffer));
    
    // Initialize PS/2 keyboard
    printf("Initializing PS/2 keyboard...\n");
    ps2kbd_init();
    
    // Initialize the Apple IIe emulator
    printf("Initializing Apple IIe emulator...\n");
    mii_init(&g_mii);
    
    // Load Apple IIe ROM (16K at $C000-$FFFF)
    printf("Loading Apple IIe ROM...\n");
    load_rom(&g_mii, mii_rom_iiee, 16384, 0xC000);
    
    // Debug: Check reset vector in ROM
    mii_bank_t *rom_bank = &g_mii.bank[MII_BANK_ROM];
    uint8_t rst_lo = mii_bank_peek(rom_bank, 0xFFFC);
    uint8_t rst_hi = mii_bank_peek(rom_bank, 0xFFFD);
    printf("ROM Reset vector at $FFFC-$FFFD: $%02X%02X\n", rst_hi, rst_lo);
    printf("ROM bank: base=$%04X, mem=%p\n", (unsigned)rom_bank->base, rom_bank->mem);
    
    // Also check raw ROM data
    printf("Raw ROM bytes at offset 0x3FFC-0x3FFD: %02X %02X\n", 
           mii_rom_iiee[0x3FFC], mii_rom_iiee[0x3FFD]);
    
    // Debug: Check slot 3 ROM area ($C3FC)
    printf("Slot 3 area: ROM @$C300-$C3FF:\n");
    printf("  Raw ROM offset 0x0300: %02X %02X %02X %02X\n",
           mii_rom_iiee[0x0300], mii_rom_iiee[0x0301], mii_rom_iiee[0x0302], mii_rom_iiee[0x0303]);
    printf("  Raw ROM offset 0x03FC: %02X %02X %02X %02X\n",
           mii_rom_iiee[0x03FC], mii_rom_iiee[0x03FD], mii_rom_iiee[0x03FE], mii_rom_iiee[0x03FF]);
    printf("  Bank peek $C3FC: %02X\n", mii_bank_peek(rom_bank, 0xC3FC));
    printf("  Direct mem[0x03FC]: %02X\n", rom_bank->mem[0x03FC]);
    
    // Load character ROM
    printf("Loading character ROM...\n");
    load_char_rom(&g_mii, mii_rom_iiee_video, 4096);
    
    // Reset the emulator - this sets reset flag and state to RUNNING
    printf("Resetting emulator...\n");
    mii_reset(&g_mii, true);
    printf("Reset complete, state=%d\n", g_mii.state);
    
    // Start HDMI output
    printf("Starting HDMI output...\n");
    startVIDEO(0);
    printf("HDMI started\n");
    
    // Let ROM boot naturally
    printf("Running ROM boot sequence (1M cycles)...\n");
    mii_run_cycles(&g_mii, 1000000);
    printf("ROM boot complete, PC=$%04X\n", g_mii.cpu.PC);
    
    // Debug: Check state after boot
    printf("Post-boot: Text page $0400: %02X %02X %02X %02X\n",
           mii_read_one(&g_mii, 0x400), mii_read_one(&g_mii, 0x401),
           mii_read_one(&g_mii, 0x402), mii_read_one(&g_mii, 0x403));

    // Signal that emulator is ready for Core 1 BEFORE launching it
    g_emulator_ready = true;
    
    // Launch video rendering on core 1
    printf("Starting video rendering on core 1...\n");
    multicore_launch_core1(core1_main);
    printf("Core 1 launched\n");
    
    printf("Starting emulation on core 0...\n");
    printf("Initial PC: $%04X\n", g_mii.cpu.PC);
    printf("=================================\n\n");
    
    // Main emulation loop on core 0
    const uint32_t cycles_per_frame = 17066; // 1023000 / 60
    
    uint32_t frame_count = 0;
    uint32_t total_emu_time = 0;
    uint32_t total_kbd_time = 0;
    
    while (1) {
        // Poll keyboard at start of frame
        uint32_t kbd_start = time_us_32();
        ps2kbd_tick();
        process_keyboard();
        uint32_t kbd_end = time_us_32();
        total_kbd_time += (kbd_end - kbd_start);
        
        // Run CPU for one frame
        uint32_t emu_start = time_us_32();
        mii_run_cycles(&g_mii, cycles_per_frame);
        uint32_t emu_end = time_us_32();
        total_emu_time += (emu_end - emu_start);
        
        frame_count++;
        
        // Print stats every 60 frames 
        if ((frame_count % 60) == 0) {
            uint32_t avg_emu_us = total_emu_time / 60;
            uint32_t avg_kbd_us = total_kbd_time / 60;
            printf("Frame %lu: emu=%lu us/frame, kbd=%lu us/frame (target: 16667), PC: $%04X\n", 
                   frame_count, avg_emu_us, avg_kbd_us, g_mii.cpu.PC);
            total_emu_time = 0;
            total_kbd_time = 0;
        }
    }
    
    return 0;
}
