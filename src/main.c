/*
 * main.c - Apple IIe Emulator for RP2350
 * Entry point with overclocking, PSRAM, and HDMI initialization
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pico.h>
#include <hardware/pwm.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#if PICO_RP2350
#include "hardware/structs/qmi.h"
#endif
#include "hardware/dma.h"  // Include DMA header early before mii_sw.h

#include "board_config.h"
#include "../drivers/psram_allocator.h"
#include "../drivers/HDMI.h"
#include "mii.h"
#include "mii_sw.h"
#include "mii_bank.h"
#include "mii_rom.h"
#include "mii_video.h"
#include "mii_65c02.h"
#include "mii_analog.h"
#include "mii_speaker.h"
#include "mii_audio_i2s.h"
#include "mii_slot.h"
#include "mii_disk2.h"
#include "disk_loader.h"
#include "mii_startscreen.h"
#include "disk_ui.h"
#include "debug_log.h"

#ifdef MII_RP2350
#include "mii_disk2_asm.h"
#endif

// Special key codes from keyboard driver
#define KEY_F11 0xFB

// Stubs for desktop-only functions we don't use on RP2350
// Note: mii_analog_access is now provided by mii_analog.c for paddle timing

void mii_speaker_click(mii_speaker_t *speaker) {
    (void)speaker;
#ifdef FEATURE_AUDIO_I2S
    // Forward speaker clicks to I2S audio driver
    extern mii_t g_mii;
    if (mii_audio_i2s_is_init()) {
        mii_audio_speaker_click(g_mii.cpu.total_cycle);
    }
#endif
#ifdef FEATURE_AUDIO_PWM
    static bool state = true;
    pwm_set_gpio_level(BEEPER_PIN, state ? ((1 << 12) - 1) : 0);
    state = !state;
#endif
}

int mii_cpu_disasm_one(char *buf, size_t buflen, mii_cpu_t *cpu,
                       uint8_t (*read_byte)(void*, uint16_t), void *param) {
    (void)buf; (void)buflen; (void)cpu; (void)read_byte; (void)param;
    return 0;  // No disassembly support
}

// External ROM data (from ROM files)
extern const uint8_t mii_rom_iiee[16384];
extern uint8_t mii_rom_iiee_video[4096];

// HDMI framebuffer dimensions (driver line-doubles to 640x480)
#undef HDMI_WIDTH
#undef HDMI_HEIGHT
#define HDMI_WIDTH 320
#define HDMI_HEIGHT 240

#if PSRAM_MAX_FREQ_MHZ
// PSRAM interface
extern void psram_init(uint cs_pin);
extern void psram_set_sram_mode(int enable);
#endif

// PS/2 keyboard interface
#ifndef ENABLE_PS2_KEYBOARD
#define ENABLE_PS2_KEYBOARD 1
#endif

#ifndef ENABLE_DEBUG_LOGS
#define ENABLE_DEBUG_LOGS 0
#endif
extern void ps2kbd_init(void);
extern void ps2kbd_tick(void);
extern int ps2kbd_get_key(int* pressed, unsigned char* key);
extern uint8_t ps2kbd_get_arrow_state(void);  // bits: 0=right, 1=left, 2=down, 3=up
extern uint8_t ps2kbd_get_modifiers(void);
extern bool ps2kbd_is_reset_combo(void);  // Ctrl+Alt+Delete pressed

// Keyboard modifier bits (from hid_codes.h)
#define KEYBOARD_MODIFIER_LEFTALT    (1 << 2)
#define KEYBOARD_MODIFIER_RIGHTALT   (1 << 6)
#define KEYBOARD_MODIFIER_LEFTCTRL   (1 << 0)
#define KEYBOARD_MODIFIER_RIGHTCTRL  (1 << 4)

// NES/SNES gamepad interface
#include "nespad/nespad.h"

// USB HID keyboard/gamepad interface
#include "usbhid/usbhid_wrapper.h"

#if PICO_RP2350
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
#endif

// Global emulator state (non-static for access from mii_speaker_click stub)
mii_t g_mii;

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
// Track currently held key for games that need key-hold detection
// Apple II keyboard repeat: ~500ms initial delay, then ~67ms repeat rate
static uint8_t currently_held_key = 0;
static uint32_t key_hold_frames = 0;  // Frames since key was first pressed
#define KEY_REPEAT_INITIAL_DELAY 30   // ~500ms at 60fps before repeat starts
#define KEY_REPEAT_RATE 4             // ~67ms between repeats

static void process_keyboard(void) {
    int pressed;
    unsigned char key;
    
    // Process PS/2 keyboard events
#if ENABLE_PS2_KEYBOARD
    while (ps2kbd_get_key(&pressed, &key)) {
        if (pressed) {
            // Check for F11 - disk selector toggle
            if (key == KEY_F11) {
                disk_ui_toggle();
                continue;
            }
            
            // If disk UI is visible, send keys to it
            if (disk_ui_is_visible()) {
                disk_ui_handle_key(key);
                currently_held_key = key;
                key_hold_frames = 0;
                continue;
            }
            
            // Normal key - send to emulator and track as held
            mii_keypress(&g_mii, key);
            currently_held_key = key;
            key_hold_frames = 0;  // Reset repeat timer
        } else {
            // Key released - clear if it was the held key
            if (key == currently_held_key) {
                currently_held_key = 0;
                key_hold_frames = 0;
            }
        }
    }
#endif
    
#ifdef USB_HID_ENABLED
    // Process USB HID keyboard events (same logic as PS/2)
    while (usbhid_wrapper_get_key(&pressed, &key)) {
        if (pressed) {
            // Check for F11 - disk selector toggle
            if (key == KEY_F11) {
                disk_ui_toggle();
                continue;
            }
            
            // If disk UI is visible, send keys to it
            if (disk_ui_is_visible()) {
                disk_ui_handle_key(key);
                currently_held_key = key;
                key_hold_frames = 0;
                continue;
            }
            
            // Normal key - send to emulator and track as held
            mii_keypress(&g_mii, key);
            currently_held_key = key;
            key_hold_frames = 0;
        } else {
            if (key == currently_held_key) {
                currently_held_key = 0;
                key_hold_frames = 0;
            }
        }
    }
#endif
    
    // Re-latch held key with proper repeat timing (like real Apple II keyboard)
    // Initial delay before repeat, then steady repeat rate
    if (currently_held_key != 0) {
        key_hold_frames++;
        
        // Only repeat after initial delay, and at the proper repeat rate
        if (key_hold_frames > KEY_REPEAT_INITIAL_DELAY) {
            uint32_t frames_since_delay = key_hold_frames - KEY_REPEAT_INITIAL_DELAY;
            if ((frames_since_delay % KEY_REPEAT_RATE) == 0) {
                // If disk UI is visible, repeat keys there
                if (disk_ui_is_visible()) {
                    disk_ui_handle_key(currently_held_key);
                } else {
                    mii_bank_t *sw = &g_mii.bank[MII_BANK_SW];
                    uint8_t strobe = mii_bank_peek(sw, 0xc010);
                    if (!(strobe & 0x80)) {
                        // Strobe is clear, game processed the key - re-latch
                        mii_keypress(&g_mii, currently_held_key);
                    }
                }
            }
        }
    }
}

// Call this to check if we should re-latch a held key
// Returns the currently held key, or 0 if none
uint8_t get_held_key(void) {
    return currently_held_key;
}

// Clear held key state (call after disk load or reset)
void clear_held_key(void) {
    currently_held_key = 0;
    key_hold_frames = 0;
}

// Flag to indicate emulator is ready
static volatile bool g_emulator_ready = false;

#if 0
// Core 1 - Video rendering loop
static void core1_main(void) {
    MII_DEBUG_PRINTF("Core 1: Waiting for emulator ready...\n");
    
    // Wait for Core 0 to finish initialization
    while (!g_emulator_ready) {
        sleep_ms(10);
    }
	__dmb();          // Data Memory Barrier
    
    MII_DEBUG_PRINTF("Core 1: Starting video rendering\n");
    
    uint32_t last_frame = hdmi_get_frame_count();
    
    while (1) {
        sleep_ms(16);
        if (!disk_ui_is_visible()) {
      //      mii_video_scale_to_hdmi(&g_mii.video, graphics_get_buffer());
        }

        // Wait until the swap has actually happened (vsync tick), then rotate buffers.
        // This avoids writing into the buffer currently being scanned out.
        uint32_t f;
        do {
            f = hdmi_get_frame_count();
            if (f != last_frame) break;
            sleep_ms(1);
        } while (1);
        last_frame = f;
    }
}
#endif

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
    // Register with the ROM system so mii_rom_get("iiee") works
    main_rom_struct.rom = rom;
    main_rom_struct.len = len;
    mii_rom_register(&main_rom_struct);
    MII_DEBUG_PRINTF("Loaded %zu bytes ROM at $%04X\n", len, addr);
}

// Load character ROM
static void load_char_rom(mii_t *mii, const uint8_t *rom, size_t len) {
    // Ensure we always end up with a descriptor whose .rom points at real bytes.
    // On RP2350 we don't run the desktop ROM loader, so any auto-registered
    // descriptor may exist but still have a NULL .rom.
    mii_rom_t *video_rom = mii_rom_get("iiee_video");
    if (video_rom && video_rom->rom) {
        mii->video.rom = video_rom;
        MII_DEBUG_PRINTF("Loaded %zu bytes character ROM (auto-registered)\n", len);
        return;
    }

    // Fall back to direct pointer (or patch the descriptor if it exists but is empty).
    if (video_rom) {
        video_rom->rom = rom;
        video_rom->len = len;
        mii->video.rom = video_rom;
        MII_DEBUG_PRINTF("Loaded %zu bytes character ROM (patched descriptor)\n", len);
    } else {
        char_rom_fallback.rom = rom;
        char_rom_fallback.len = len;
        mii->video.rom = &char_rom_fallback;
        MII_DEBUG_PRINTF("Loaded %zu bytes character ROM (fallback)\n", len);
    }
}

static void PWM_init_pin(uint8_t pinN, uint16_t max_lvl) {
    pwm_config config = pwm_get_default_config();
    gpio_set_function(pinN, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&config, 1.0);
    pwm_config_set_wrap(&config, max_lvl); // MAX PWM value
    pwm_init(pwm_gpio_to_slice_num(pinN), &config, true);
}

int main() {
    // Overclock support: For speeds > 252 MHz, increase voltage first
#if CPU_CLOCK_MHZ > 252
    vreg_disable_voltage_limit();
#if PICO_RP2040
    hw_set_bits(&vreg_and_chip_reset_hw->vreg,
                VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
#else
    vreg_set_voltage(CPU_VOLTAGE);
    set_flash_timings(CPU_CLOCK_MHZ);
#endif
    sleep_ms(100);
#endif
    
// Set system clock
#if PICO_RP2040
    set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, true);
#else
    if (!set_sys_clock_khz(CPU_CLOCK_MHZ * 1000, false)) {
        set_sys_clock_khz(252 * 1000, true);
    }
#endif

    // Initialize stdio (USB serial)
    stdio_init_all();
    
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }
#endif

    MII_DEBUG_PRINTF("\n\n");
    MII_DEBUG_PRINTF("=================================\n");
#if PICO_RP2040
    MII_DEBUG_PRINTF("  MurmApple - Apple IIe on RP2040\n");
#else
    MII_DEBUG_PRINTF("  MurmApple - Apple IIe on RP2350\n");
#endif
    MII_DEBUG_PRINTF("=================================\n");
    MII_DEBUG_PRINTF("System Clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);
    
#if PSRAM_MAX_FREQ_MHZ
    // Initialize PSRAM
    MII_DEBUG_PRINTF("Initializing PSRAM...\n");
    uint psram_pin = get_psram_pin();
    psram_init(psram_pin);
    psram_set_sram_mode(0);  // Use PSRAM mode (not SRAM simulation)
    MII_DEBUG_PRINTF("PSRAM initialized on CS pin %d\n", psram_pin);
    
    // Test PSRAM read/write
    volatile uint8_t *psram = (volatile uint8_t *)0x11000000;
    psram[0] = 0xAB;
    psram[1] = 0xCD;
    psram[2] = 0xEF;
    MII_DEBUG_PRINTF("PSRAM test: wrote AB CD EF, read %02X %02X %02X\n", 
           psram[0], psram[1], psram[2]);
    if (psram[0] != 0xAB || psram[1] != 0xCD || psram[2] != 0xEF) {
        MII_DEBUG_PRINTF("ERROR: PSRAM read/write failed!\n");
    }
#endif
    
    // IMPORTANT: Set buffer and resolution BEFORE graphics_init() 
    // because DMA/IRQs start immediately and will read from the buffer
    graphics_set_res(HDMI_WIDTH, HDMI_HEIGHT);
    
    // Initialize HDMI graphics (starts DMA/IRQs)
    MII_DEBUG_PRINTF("Initializing HDMI...\n");
    graphics_init(g_out_HDMI);
    
    // Initialize palette
    init_palette();
    graphics_restore_sync_colors();  // Restore HDMI sync colors after palette init
    
    // Verify palette entry 15 was set
    MII_DEBUG_PRINTF("Palette initialized, verifying...\n");
    extern uint32_t conv_color[];
    uint64_t *conv_color64 = (uint64_t *)conv_color;
    MII_DEBUG_PRINTF("conv_color[15] = 0x%016llx 0x%016llx\n", conv_color64[30], conv_color64[31]);
    
    // Initialize PS/2 keyboard
    MII_DEBUG_PRINTF("Initializing PS/2 keyboard...\n");
#if ENABLE_PS2_KEYBOARD
    ps2kbd_init();
    MII_DEBUG_PRINTF("PS/2 keyboard init complete\n");
#else
    MII_DEBUG_PRINTF("PS/2 keyboard disabled\n");
#endif
    
    // Initialize NES/SNES gamepad
    MII_DEBUG_PRINTF("Initializing NES gamepad...\n");
    if (nespad_begin(clock_get_hz(clk_sys) / 1000, NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH)) {
        MII_DEBUG_PRINTF("NES gamepad initialized (CLK=%d, DATA=%d, LATCH=%d)\n",
               NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH);
    } else {
        MII_DEBUG_PRINTF("NES gamepad init failed\n");
    }
    
    // Initialize USB HID keyboard/gamepad (if enabled)
#ifdef USB_HID_ENABLED
    MII_DEBUG_PRINTF("Initializing USB HID Host...\n");
    usbhid_wrapper_init();
    MII_DEBUG_PRINTF("USB HID Host initialized\n");
#endif
    
    // Initialize SD card and scan for disk images
    MII_DEBUG_PRINTF("Initializing SD card and disk images...\n");
    if (disk_loader_init() == 0) {
        MII_DEBUG_PRINTF("SD card ready, found %d disk images\n", g_disk_count);
    } else {
        MII_DEBUG_PRINTF("SD card not available (will run without disks)\n");
    }
    
    // Initialize the Apple IIe emulator
    MII_DEBUG_PRINTF("Initializing Apple IIe emulator...\n");
    mii_init(&g_mii);

    // RP2350 mii_init() skips mii_video_init(); seed video-related SW registers.
    // Default AN3 register to color mode (matches desktop init).
    mii_bank_poke(&g_mii.bank[MII_BANK_SW], SWAN3_REGISTER, 1);
    g_mii.video.an3_mode = 1;
    
    // Install Disk II controller in slot 6
    MII_DEBUG_PRINTF("Installing Disk II controller in slot 6...\n");
    int slot_res = mii_slot_drv_register(&g_mii, 6, "disk2");
    if (slot_res < 0) {
        MII_DEBUG_PRINTF("ERROR: Failed to install Disk II controller: %d\n", slot_res);
    } else {
        MII_DEBUG_PRINTF("Disk II controller installed in slot 6\n");
        
#ifdef MII_RP2350
        // Print struct offsets for assembly verification
        mii_disk2_print_offsets();
#endif
        
        // Debug: dump first few bytes of slot 6 ROM
        mii_bank_t *card_rom = &g_mii.bank[MII_BANK_CARD_ROM];
        MII_DEBUG_PRINTF("Card ROM bank: base=$%04X, mem=%p\n", card_rom->base, card_rom->mem);
        MII_DEBUG_PRINTF("Slot 6 ROM at $C600: ");
        for (int i = 0; i < 16; i++) {
            MII_DEBUG_PRINTF("%02X ", mii_bank_peek(card_rom, 0xC600 + i));
        }
        MII_DEBUG_PRINTF("\n");
        
        // Debug: Check empty slot 2 area (should be all zeros)
        MII_DEBUG_PRINTF("Slot 2 ROM at $C200: ");
        for (int i = 0; i < 16; i++) {
            MII_DEBUG_PRINTF("%02X ", mii_bank_peek(card_rom, 0xC200 + i));
        }
        MII_DEBUG_PRINTF("\n");
        MII_DEBUG_PRINTF("Slot 2 signature bytes: $C205=%02X, $C207=%02X\n",
               mii_bank_peek(card_rom, 0xC205), mii_bank_peek(card_rom, 0xC207));
    }
    
    // Initialize disk UI with emulator pointer (slot 6 is standard for Disk II)
    disk_ui_init_with_emulator(&g_mii, 6);
    
    // Load Apple IIe ROM (16K at $C000-$FFFF)
    MII_DEBUG_PRINTF("Loading Apple IIe ROM...\n");
    load_rom(&g_mii, mii_rom_iiee, 16384, 0xC000);
    
    // Debug: Check reset vector in ROM
    mii_bank_t *rom_bank = &g_mii.bank[MII_BANK_ROM];
    uint8_t rst_lo = mii_bank_peek(rom_bank, 0xFFFC);
    uint8_t rst_hi = mii_bank_peek(rom_bank, 0xFFFD);
    MII_DEBUG_PRINTF("ROM Reset vector at $FFFC-$FFFD: $%02X%02X\n", rst_hi, rst_lo);
    MII_DEBUG_PRINTF("ROM bank: base=$%04X, mem=%p\n", (unsigned)rom_bank->base, rom_bank->mem);
    
    // Also check raw ROM data
    MII_DEBUG_PRINTF("Raw ROM bytes at offset 0x3FFC-0x3FFD: %02X %02X\n", 
           mii_rom_iiee[0x3FFC], mii_rom_iiee[0x3FFD]);
    
    // Debug: Check slot 3 ROM area ($C3FC)
        MII_DEBUG_PRINTF("Slot 3 area: ROM @$C300-$C3FF:\n");
        MII_DEBUG_PRINTF("  Raw ROM offset 0x0300: %02X %02X %02X %02X\n",
           mii_rom_iiee[0x0300], mii_rom_iiee[0x0301], mii_rom_iiee[0x0302], mii_rom_iiee[0x0303]);
        MII_DEBUG_PRINTF("  Raw ROM offset 0x03FC: %02X %02X %02X %02X\n",
           mii_rom_iiee[0x03FC], mii_rom_iiee[0x03FD], mii_rom_iiee[0x03FE], mii_rom_iiee[0x03FF]);
        MII_DEBUG_PRINTF("  Bank peek $C3FC: %02X\n", mii_bank_peek(rom_bank, 0xC3FC));
        MII_DEBUG_PRINTF("  Direct mem[0x03FC]: %02X\n", rom_bank->mem[0x03FC]);
    
    // Load character ROM
    MII_DEBUG_PRINTF("Loading character ROM...\n");
    load_char_rom(&g_mii, mii_rom_iiee_video, 4096);
    if (g_mii.video.rom && g_mii.video.rom->rom) {
        const uint8_t *p = (const uint8_t *)g_mii.video.rom->rom;
        MII_DEBUG_PRINTF("Char ROM: desc=%p bytes=%p len=%u first=%02X %02X %02X %02X\n",
               (void *)g_mii.video.rom, (void *)p, (unsigned)g_mii.video.rom->len,
               p[0], p[1], p[2], p[3]);
    } else {
        MII_DEBUG_PRINTF("ERROR: Char ROM missing (desc=%p)\n", (void *)g_mii.video.rom);
        while(1);
    }
    
    // Reset the emulator - this sets reset flag and state to RUNNING
    MII_DEBUG_PRINTF("Resetting emulator...\n");
    mii_reset(&g_mii, true);
    MII_DEBUG_PRINTF("Reset complete, state=%d\n", g_mii.state);
    
    // Start HDMI output
    MII_DEBUG_PRINTF("Starting HDMI output...\n");
    startVIDEO(0);
    MII_DEBUG_PRINTF("HDMI started\n");
    
    // Display start screen with system information
    MII_DEBUG_PRINTF("Displaying start screen...\n");
    uint32_t board_num = 1;  // Default to M1
#ifdef BOARD_M2
    board_num = 2;
#endif

#ifndef PICO_RP2040 // for RP2350 only
    mii_startscreen_info_t screen_info = {
        .title = "MurmApple",
        .subtitle = "Apple IIe Emulator",
        .version = "v1.00",
        .cpu_mhz = CPU_CLOCK_MHZ,
#if PSRAM_MAX_FREQ_MHZ
        .psram_mhz = PSRAM_MAX_FREQ_MHZ,
#endif
        .board_variant = board_num,
    };
    mii_startscreen_show(&screen_info);
#endif

    // Let ROM boot naturally
    MII_DEBUG_PRINTF("Running ROM boot sequence (1M cycles)...\n");
    mii_run_cycles(&g_mii, 1000000);
    MII_DEBUG_PRINTF("ROM boot complete, PC=$%04X\n", g_mii.cpu.PC);
    
    // Debug: Check state after boot
    MII_DEBUG_PRINTF("Post-boot: Text page $0400: %02X %02X %02X %02X\n",
           mii_read_one(&g_mii, 0x400), mii_read_one(&g_mii, 0x401),
           mii_read_one(&g_mii, 0x402), mii_read_one(&g_mii, 0x403));

    // Signal that emulator is ready for Core 1 BEFORE launching it
    g_emulator_ready = true;
    
    // Launch video rendering on core 1
#if 0
    MII_DEBUG_PRINTF("Starting video rendering on core 1...\n");
    multicore_launch_core1(core1_main);
    MII_DEBUG_PRINTF("Core 1 launched\n");
#endif
    
#ifdef FEATURE_AUDIO_I2S
    // Initialize I2S audio
    MII_DEBUG_PRINTF("Initializing I2S audio...\n");
    if (mii_audio_i2s_init()) {
        MII_DEBUG_PRINTF("I2S audio initialized (DATA=%d, CLK=%d/%d, %d Hz)\n",
               I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, I2S_CLOCK_PIN_BASE + 1, MII_I2S_SAMPLE_RATE);
    } else {
        MII_DEBUG_PRINTF("I2S audio initialization failed\n");
    }
#endif
#ifdef FEATURE_AUDIO_PWM
    PWM_init_pin(BEEPER_PIN, (1 << 12) - 1);
#endif

    MII_DEBUG_PRINTF("Starting emulation on core 0...\n");
    MII_DEBUG_PRINTF("Initial PC: $%04X\n", g_mii.cpu.PC);
    MII_DEBUG_PRINTF("=================================\n\n");
    
    // Main emulation loop on core 0
    // Apple II runs at ~1.023 MHz = 1,023,000 cycles/second.
    // VBL timing is handled by mii_video_vbl_timer_cb at proper cycle counts.
    // Video timing: visible=12480 cycles, VBL=4550 cycles (total 17030/frame)
    const uint32_t a2_cycles_per_second = 1023000;
    const uint32_t cycles_per_frame = 17030;  // 12480 visible + 4550 vblank
    const uint32_t target_frame_us = (uint32_t)((1000000ULL * cycles_per_frame + (a2_cycles_per_second / 2)) / a2_cycles_per_second);
    
    uint32_t frame_count = 0;
    uint32_t total_emu_time = 0;
    uint16_t last_pc = 0;
    uint32_t boot_time_us = time_us_32();  // Track real wall-clock time

    // Performance metrics
    uint32_t total_cpu_time = 0;      // Time spent in CPU emulation
    uint32_t total_input_time = 0;    // Time spent polling input
    uint32_t total_cycles_run = 0;    // Actual cycles executed
    uint32_t metrics_start_us = time_us_32();

    // Debug state (printed from core0 to avoid disturbing HDMI DMA on core1)
    uint32_t last_mode_key = 0xffffffffu;
    uint32_t last_fb_hash = 0;
    int last_fb_nonzero = -1;
    
    while (1) {
        uint32_t frame_start = time_us_32();
        
        // Poll keyboard at start of frame
        uint32_t input_start = time_us_32();
    #if ENABLE_PS2_KEYBOARD
        ps2kbd_tick();
    #endif
        
#ifdef USB_HID_ENABLED
        // Poll USB HID devices
        usbhid_wrapper_poll();
#endif
        
        // Check for Ctrl+Alt+Delete reset combo
        static bool reset_combo_active = false;
        bool reset_combo = false;
    #if ENABLE_PS2_KEYBOARD
        reset_combo |= ps2kbd_is_reset_combo();
    #endif
    #ifdef USB_HID_ENABLED
        reset_combo |= usbhid_wrapper_is_reset_combo();
    #endif
        if (reset_combo) {
            if (!reset_combo_active) {
                reset_combo_active = true;
                MII_DEBUG_PRINTF("Reset combo detected (Ctrl+Alt+Delete)\n");
                mii_reset(&g_mii, true);
            }
        } else {
            reset_combo_active = false;
        }
        
        process_keyboard();
        
        // Poll NES gamepad and update Apple II buttons
        nespad_read();
        
        // Merge USB gamepad state with NES gamepad state
        uint32_t combined_gamepad_state = nespad_state;
#ifdef USB_HID_ENABLED
        combined_gamepad_state |= usbhid_wrapper_get_gamepad_state();
#endif
        
        {
            // Track previous gamepad state for edge detection
            static uint32_t prev_gamepad_state = 0;
            static uint32_t gamepad_hold_frames = 0;
            static uint32_t gamepad_held_button = 0;
            static bool gamepad_reset_combo_active = false;
            uint32_t gamepad_pressed = combined_gamepad_state & ~prev_gamepad_state;  // Just pressed this frame
            
            // Check for Start+A+B reset combo
            if ((combined_gamepad_state & (DPAD_START | DPAD_A | DPAD_B)) == (DPAD_START | DPAD_A | DPAD_B)) {
                if (!gamepad_reset_combo_active) {
                    gamepad_reset_combo_active = true;
                    MII_DEBUG_PRINTF("Reset combo detected (Start+A+B)\n");
                    mii_reset(&g_mii, true);
                }
                // Skip all gamepad processing while reset combo is held
                prev_gamepad_state = combined_gamepad_state;
                goto skip_gamepad_emulation;
            } else {
                gamepad_reset_combo_active = false;
            }
            
            // Gamepad repeat settings (same timing as keyboard)
            #define GAMEPAD_REPEAT_INITIAL 30  // ~500ms at 60fps
            #define GAMEPAD_REPEAT_RATE 4      // ~67ms between repeats
            
            // SELECT button toggles disk UI (like F11)
            if (gamepad_pressed & DPAD_SELECT) {
                disk_ui_toggle();
            }
            
            // If disk UI is visible, handle gamepad navigation
            if (disk_ui_is_visible()) {
                // Track which D-pad direction is held for repeat
                uint32_t dpad_mask = DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT;
                uint32_t dpad_held = combined_gamepad_state & dpad_mask;
                
                // Handle initial press
                if (gamepad_pressed & DPAD_UP) {
                    disk_ui_handle_key(0x0B);
                    gamepad_held_button = DPAD_UP;
                    gamepad_hold_frames = 0;
                }
                if (gamepad_pressed & DPAD_DOWN) {
                    disk_ui_handle_key(0x0A);
                    gamepad_held_button = DPAD_DOWN;
                    gamepad_hold_frames = 0;
                }
                if (gamepad_pressed & DPAD_LEFT) {
                    disk_ui_handle_key(0x08);
                    gamepad_held_button = DPAD_LEFT;
                    gamepad_hold_frames = 0;
                }
                if (gamepad_pressed & DPAD_RIGHT) {
                    disk_ui_handle_key(0x15);
                    gamepad_held_button = DPAD_RIGHT;
                    gamepad_hold_frames = 0;
                }
                
                // Handle key repeat for held D-pad
                if (dpad_held && (dpad_held & gamepad_held_button)) {
                    gamepad_hold_frames++;
                    if (gamepad_hold_frames > GAMEPAD_REPEAT_INITIAL) {
                        uint32_t since_delay = gamepad_hold_frames - GAMEPAD_REPEAT_INITIAL;
                        if ((since_delay % GAMEPAD_REPEAT_RATE) == 0) {
                            if (gamepad_held_button == DPAD_UP) disk_ui_handle_key(0x0B);
                            else if (gamepad_held_button == DPAD_DOWN) disk_ui_handle_key(0x0A);
                            else if (gamepad_held_button == DPAD_LEFT) disk_ui_handle_key(0x08);
                            else if (gamepad_held_button == DPAD_RIGHT) disk_ui_handle_key(0x15);
                        }
                    }
                } else {
                    gamepad_held_button = 0;
                    gamepad_hold_frames = 0;
                }
                
                // A button = Enter (select) - no repeat
                if (gamepad_pressed & DPAD_A) {
                    disk_ui_handle_key(0x0D);
                }
                // B button = Escape (cancel/back) - no repeat
                if (gamepad_pressed & DPAD_B) {
                    disk_ui_handle_key(0x1B);
                }
                
                // Don't update joystick/buttons while in UI
                prev_gamepad_state = combined_gamepad_state;
                goto skip_gamepad_emulation;
            }
            
            prev_gamepad_state = combined_gamepad_state;
            
            mii_bank_t *sw = &g_mii.bank[MII_BANK_SW];
            // Map NES buttons + keyboard modifiers to Apple II buttons:
            // NES A/B or Left Alt -> Open Apple (Button 0, $C061)
            // NES A/B or Right Alt -> Closed Apple (Button 1, $C062)
            // NES Start -> Button 2 ($C063)
            uint8_t mods = 0;
#if ENABLE_PS2_KEYBOARD
            mods |= ps2kbd_get_modifiers();
#endif
#ifdef USB_HID_ENABLED
            mods |= usbhid_wrapper_get_modifiers();
#endif
            uint8_t btn0 = (combined_gamepad_state & (DPAD_A | DPAD_B)) || (mods & KEYBOARD_MODIFIER_LEFTALT) ? 0x80 : 0x00;
            uint8_t btn1 = (combined_gamepad_state & (DPAD_A | DPAD_B)) || (mods & KEYBOARD_MODIFIER_RIGHTALT) ? 0x80 : 0x00;
            uint8_t btn2 = (combined_gamepad_state & DPAD_START) ? 0x80 : 0x00;
            mii_bank_poke(sw, 0xc061, btn0);
            mii_bank_poke(sw, 0xc062, btn1);
            mii_bank_poke(sw, 0xc063, btn2);
            
            // Map NES D-pad to Apple II joystick
            // For paddle/analog games like Arkanoid, we use gradual movement
            // instead of snapping to extremes, as these games rely on analog control.
            // Paddle 0 = X axis (left/right): 0=left, 127=center, 255=right
            // Paddle 1 = Y axis (up/down): 0=up, 255=down
            static uint8_t joy_x = 127;  // Persistent X position
            static uint8_t joy_y = 127;  // Persistent Y position
            
            // NES D-pad controls - gradual movement for paddle games
            // Speed: ~4 per frame = full range in ~32 frames (~0.5 sec)
            #define PADDLE_SPEED 4
            
            if (combined_gamepad_state & DPAD_LEFT) {
                if (joy_x > PADDLE_SPEED) joy_x -= PADDLE_SPEED;
                else joy_x = 0;
            }
            if (combined_gamepad_state & DPAD_RIGHT) {
                if (joy_x < 255 - PADDLE_SPEED) joy_x += PADDLE_SPEED;
                else joy_x = 255;
            }
            if (combined_gamepad_state & DPAD_UP) {
                if (joy_y > PADDLE_SPEED) joy_y -= PADDLE_SPEED;
                else joy_y = 0;
            }
            if (combined_gamepad_state & DPAD_DOWN) {
                if (joy_y < 255 - PADDLE_SPEED) joy_y += PADDLE_SPEED;
                else joy_y = 255;
            }
            
            g_mii.analog.v[0].value = joy_x;
            g_mii.analog.v[1].value = joy_y;
            
            skip_gamepad_emulation:;  // Label for skipping when UI is visible
        }
        uint32_t input_end = time_us_32();
        total_input_time += (input_end - input_start);
        
        // Track disk UI state changes for debugging
        static bool disk_ui_was_visible = false;
        static int debug_frames = 0;
        bool disk_ui_now = disk_ui_is_visible();
        
        if (disk_ui_was_visible && !disk_ui_now) {
            // UI just closed - debug first 60 frames (1 second)
            debug_frames = 60;
            MII_DEBUG_PRINTF("=== DISK UI CLOSED - MONITORING ===\n");
        }
        disk_ui_was_visible = disk_ui_now;
        
        // Run CPU for one frame worth of cycles.
        // VBL timing is now handled by mii_video_vbl_timer_cb which toggles
        // SWVBL at the proper cycle counts during execution. This allows
        // games that wait for VBL transitions to work correctly.
        //
        // IMPORTANT: Don't run emulator while disk UI is visible!
        // Games time their title screens using VBL counts. If we keep running
        // while user navigates the disk menu, the game's timer advances and
        // it will skip through timed sequences (like title screens).
        uint32_t cpu_start = time_us_32();
        uint64_t cycles_before = g_mii.cpu.total_cycle;
        if (!disk_ui_is_visible()) {
            // Debug: Check button/key state BEFORE running CPU
            if (debug_frames > 0) {
                mii_bank_t *sw = &g_mii.bank[MII_BANK_SW];
                uint8_t btn0 = mii_bank_peek(sw, 0xc061);
                uint8_t btn1 = mii_bank_peek(sw, 0xc062);
                uint8_t key = mii_bank_peek(sw, SWKBD);
                if (btn0 || btn1 || (key & 0x80)) {
                    MII_DEBUG_PRINTF("F%d: BTN0=%02X BTN1=%02X KEY=%02X\n", 
                           60 - debug_frames, btn0, btn1, key);
                }
                debug_frames--;
            }
            mii_run_cycles(&g_mii, cycles_per_frame);
            mii_video_scale_to_hdmi(&g_mii.video, graphics_get_buffer());
        } else {
            disk_ui_render(graphics_get_buffer(), HDMI_WIDTH, HDMI_HEIGHT);
        }
        uint64_t cycles_after = g_mii.cpu.total_cycle;
        uint32_t cpu_end = time_us_32();
        
        total_cpu_time += (cpu_end - cpu_start);
        total_cycles_run += (uint32_t)(cycles_after - cycles_before);

#ifdef FEATURE_AUDIO_I2S
        // Update audio output - fills I2S buffers
        mii_audio_update(cycles_after, a2_cycles_per_second);
#endif

        // frame_count is now incremented by the VBL timer callback
        
        uint32_t frame_end = time_us_32();
        total_emu_time += (frame_end - frame_start);

        // Throttle to real time so the emulator doesn't run too fast.
        uint32_t elapsed = frame_end - frame_start;
        if (elapsed < target_frame_us) {
            sleep_us((uint64_t)(target_frame_us - elapsed));
        }
        
        frame_count++;

        // Video mode change detection - only print when mode changes
        if ((frame_count % 60) == 0) {
            uint32_t sw = g_mii.sw_state;
            bool store80 = !!(sw & M_SW80STORE);
            bool text_mode = !!(sw & M_SWTEXT);
            bool mixed = !!(sw & M_SWMIXED);
            bool hires = !!(sw & M_SWHIRES);
            bool col80 = !!(sw & M_SW80COL);
            bool dhires = !!(sw & M_SWDHIRES);
            bool page2_raw = !!(sw & M_SWPAGE2);
            bool page2_eff = store80 ? 0 : page2_raw;
            uint8_t an3 = g_mii.video.an3_mode;

            uint32_t mode_key =
                (text_mode ? 1u : 0u) |
                (hires ? 2u : 0u) |
                (mixed ? 4u : 0u) |
                (page2_eff ? 8u : 0u) |
                (col80 ? 16u : 0u) |
                (dhires ? 32u : 0u) |
                (store80 ? 64u : 0u) |
                ((uint32_t)(an3 & 3u) << 8);

            // Track mode changes silently (remove debug spam)
            last_mode_key = mode_key;
        }
        
         // Print detailed performance metrics every 300 frames (5 seconds)
    #if 0
         if ((frame_count % 300) == 0) {
             uint32_t elapsed_us = time_us_32() - metrics_start_us;
             uint32_t avg_frame_us = total_emu_time / 300;
             uint32_t avg_cpu_us = total_cpu_time / 300;
             uint32_t avg_input_us = total_input_time / 300;
             uint32_t avg_other_us = avg_frame_us - avg_cpu_us - avg_input_us;

             // Calculate effective MHz
             // cycles_run in 5 seconds -> cycles/sec -> MHz
             uint32_t cycles_per_sec = (uint32_t)((uint64_t)total_cycles_run * 1000000ULL / elapsed_us);
             uint32_t effective_khz = cycles_per_sec / 1000;

             // Calculate cycles per microsecond of CPU time (efficiency)
             uint32_t cycles_per_cpu_us = total_cpu_time > 0 ? total_cycles_run / total_cpu_time : 0;

             // Target is 1.023 MHz = 1023 kHz
             uint32_t percent_speed = (effective_khz * 100) / 1023;

            MII_DEBUG_PRINTF("\n=== PERF Frame %lu (%.1fs) ===\n",
                frame_count, (float)elapsed_us / 1000000.0f);
            MII_DEBUG_PRINTF("Frame: %lu us (CPU: %lu, Input: %lu, Other: %lu)\n",
                avg_frame_us, avg_cpu_us, avg_input_us, avg_other_us);
            MII_DEBUG_PRINTF("Speed: %lu kHz (%lu%% of 1.023 MHz), %lu cyc/us\n",
                effective_khz, percent_speed, cycles_per_cpu_us);
            MII_DEBUG_PRINTF("PC: $%04X, Total cycles: %llu\n",
                g_mii.cpu.PC, g_mii.cpu.total_cycle);
            MII_DEBUG_PRINTF("=============================\n\n");

             // Reset counters
             total_emu_time = 0;
             total_cpu_time = 0;
             total_input_time = 0;
             total_cycles_run = 0;
             metrics_start_us = time_us_32();
         }
    #endif
    }
    return 0;
}
