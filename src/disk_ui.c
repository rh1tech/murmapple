/*
 * disk_ui.c
 * 
 * Macintosh-style disk selector UI for murmapple
 * Features inverted title bar, compact 6x8 font, proper selection highlighting
 */

#include <pico.h>
#include <pico/stdlib.h>
#include <hardware/sync.h>
#include <stdio.h>
#include <string.h>
#include "disk_ui.h"
#include "disk_loader.h"
#include "mii.h"
#include "mii_sw.h"
#include "mii_bank.h"
#include "debug_log.h"

// External function to clear held key state (from main.c)
extern void clear_held_key(void);

// Emulator reference (for mounting disks)
static mii_t *g_mii = NULL;
int g_disk2_slot = 6;  // Default slot for Disk II

// UI state - volatile to prevent race conditions between cores
static volatile disk_ui_state_t ui_state = DISK_UI_HIDDEN;
static volatile int selected_drive = 0;      // 0 or 1
static volatile int selected_file = 0;       // Currently highlighted file
static volatile int selected_action = 0;     // 0=Boot, 1=Insert, 2=Cancel
static volatile int scroll_offset = 0;       // For scrolling long lists
static volatile bool ui_dirty = false;       // True when UI needs redraw
static volatile bool ui_rendered = false;    // True when UI has been rendered at least once

// With double-buffering, the render target alternates each frame.
static uint8_t *g_last_framebuffer = NULL;

// UI dimensions - larger window with compact font
#define UI_X            24      // Left edge in 320px mode
#define UI_Y            20      // Top edge in 240px mode  
#define UI_WIDTH        272     // Dialog width
#define UI_HEIGHT       200     // Dialog height
#define UI_PADDING      6       // Padding inside dialog
#define CHAR_WIDTH      6       // Character width in pixels (compact font)
#define CHAR_HEIGHT     8       // Character height in pixels
#define HEADER_HEIGHT   12      // Title bar height
#define LINE_HEIGHT     10      // Line spacing
#define MAX_VISIBLE     16      // Max visible items
#define MAX_DISPLAY_LEN 40      // Max characters for filename display

// Colors (palette indices)
#define COLOR_BG        0   // Black
#define COLOR_BORDER    15  // White
#define COLOR_TEXT      15  // White
#define COLOR_HEADER_BG 15  // White (for inverted header)
#define COLOR_HEADER_FG 0   // Black (for inverted header)

// Compact 6x8 bitmap font (similar to Apple/Mac system font)
// Each character is 8 bytes (rows), only 6 pixels wide per row
static const uint8_t font_6x8[][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 Space
    {0x20,0x20,0x20,0x20,0x20,0x00,0x20,0x00}, // 33 !
    {0x50,0x50,0x50,0x00,0x00,0x00,0x00,0x00}, // 34 "
    {0x50,0x50,0xF8,0x50,0xF8,0x50,0x50,0x00}, // 35 #
    {0x20,0x78,0xA0,0x70,0x28,0xF0,0x20,0x00}, // 36 $
    {0xC0,0xC8,0x10,0x20,0x40,0x98,0x18,0x00}, // 37 %
    {0x40,0xA0,0xA0,0x40,0xA8,0x90,0x68,0x00}, // 38 &
    {0x20,0x20,0x40,0x00,0x00,0x00,0x00,0x00}, // 39 '
    {0x10,0x20,0x40,0x40,0x40,0x20,0x10,0x00}, // 40 (
    {0x40,0x20,0x10,0x10,0x10,0x20,0x40,0x00}, // 41 )
    {0x00,0x20,0xA8,0x70,0xA8,0x20,0x00,0x00}, // 42 *
    {0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00}, // 43 +
    {0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x40}, // 44 ,
    {0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00}, // 45 -
    {0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x00}, // 46 .
    {0x00,0x08,0x10,0x20,0x40,0x80,0x00,0x00}, // 47 /
    {0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00}, // 48 0
    {0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00}, // 49 1
    {0x70,0x88,0x08,0x30,0x40,0x80,0xF8,0x00}, // 50 2
    {0x70,0x88,0x08,0x30,0x08,0x88,0x70,0x00}, // 51 3
    {0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00}, // 52 4
    {0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0x00}, // 53 5
    {0x30,0x40,0x80,0xF0,0x88,0x88,0x70,0x00}, // 54 6
    {0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0x00}, // 55 7
    {0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00}, // 56 8
    {0x70,0x88,0x88,0x78,0x08,0x10,0x60,0x00}, // 57 9
    {0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00}, // 58 :
    {0x00,0x00,0x20,0x00,0x00,0x20,0x20,0x40}, // 59 ;
    {0x08,0x10,0x20,0x40,0x20,0x10,0x08,0x00}, // 60 <
    {0x00,0x00,0xF8,0x00,0xF8,0x00,0x00,0x00}, // 61 =
    {0x40,0x20,0x10,0x08,0x10,0x20,0x40,0x00}, // 62 >
    {0x70,0x88,0x10,0x20,0x20,0x00,0x20,0x00}, // 63 ?
    {0x70,0x88,0xB8,0xA8,0xB8,0x80,0x70,0x00}, // 64 @
    {0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0x00}, // 65 A
    {0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00}, // 66 B
    {0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00}, // 67 C
    {0xE0,0x90,0x88,0x88,0x88,0x90,0xE0,0x00}, // 68 D
    {0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00}, // 69 E
    {0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00}, // 70 F
    {0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0x00}, // 71 G
    {0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00}, // 72 H
    {0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00}, // 73 I
    {0x38,0x10,0x10,0x10,0x90,0x90,0x60,0x00}, // 74 J
    {0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00}, // 75 K
    {0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00}, // 76 L
    {0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0x00}, // 77 M
    {0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00}, // 78 N
    {0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00}, // 79 O
    {0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00}, // 80 P
    {0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00}, // 81 Q
    {0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00}, // 82 R
    {0x70,0x88,0x80,0x70,0x08,0x88,0x70,0x00}, // 83 S
    {0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00}, // 84 T
    {0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00}, // 85 U
    {0x88,0x88,0x88,0x88,0x50,0x50,0x20,0x00}, // 86 V
    {0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88,0x00}, // 87 W
    {0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00}, // 88 X
    {0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00}, // 89 Y
    {0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00}, // 90 Z
    {0x70,0x40,0x40,0x40,0x40,0x40,0x70,0x00}, // 91 [
    {0x00,0x80,0x40,0x20,0x10,0x08,0x00,0x00}, // 92 backslash
    {0x70,0x10,0x10,0x10,0x10,0x10,0x70,0x00}, // 93 ]
    {0x20,0x50,0x88,0x00,0x00,0x00,0x00,0x00}, // 94 ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF8}, // 95 _
    {0x40,0x20,0x10,0x00,0x00,0x00,0x00,0x00}, // 96 `
    {0x00,0x00,0x70,0x08,0x78,0x88,0x78,0x00}, // 97 a
    {0x80,0x80,0xB0,0xC8,0x88,0xC8,0xB0,0x00}, // 98 b
    {0x00,0x00,0x70,0x80,0x80,0x88,0x70,0x00}, // 99 c
    {0x08,0x08,0x68,0x98,0x88,0x98,0x68,0x00}, // 100 d
    {0x00,0x00,0x70,0x88,0xF8,0x80,0x70,0x00}, // 101 e
    {0x30,0x48,0x40,0xE0,0x40,0x40,0x40,0x00}, // 102 f
    {0x00,0x00,0x68,0x98,0x98,0x68,0x08,0x70}, // 103 g
    {0x80,0x80,0xB0,0xC8,0x88,0x88,0x88,0x00}, // 104 h
    {0x20,0x00,0x60,0x20,0x20,0x20,0x70,0x00}, // 105 i
    {0x10,0x00,0x30,0x10,0x10,0x90,0x60,0x00}, // 106 j
    {0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90,0x00}, // 107 k
    {0x60,0x20,0x20,0x20,0x20,0x20,0x70,0x00}, // 108 l
    {0x00,0x00,0xD0,0xA8,0xA8,0xA8,0xA8,0x00}, // 109 m
    {0x00,0x00,0xB0,0xC8,0x88,0x88,0x88,0x00}, // 110 n
    {0x00,0x00,0x70,0x88,0x88,0x88,0x70,0x00}, // 111 o
    {0x00,0x00,0xB0,0xC8,0xC8,0xB0,0x80,0x80}, // 112 p
    {0x00,0x00,0x68,0x98,0x98,0x68,0x08,0x08}, // 113 q
    {0x00,0x00,0xB0,0xC8,0x80,0x80,0x80,0x00}, // 114 r
    {0x00,0x00,0x78,0x80,0x70,0x08,0xF0,0x00}, // 115 s
    {0x40,0x40,0xE0,0x40,0x40,0x48,0x30,0x00}, // 116 t
    {0x00,0x00,0x88,0x88,0x88,0x98,0x68,0x00}, // 117 u
    {0x00,0x00,0x88,0x88,0x88,0x50,0x20,0x00}, // 118 v
    {0x00,0x00,0x88,0xA8,0xA8,0xA8,0x50,0x00}, // 119 w
    {0x00,0x00,0x88,0x50,0x20,0x50,0x88,0x00}, // 120 x
    {0x00,0x00,0x88,0x88,0x98,0x68,0x08,0x70}, // 121 y
    {0x00,0x00,0xF8,0x10,0x20,0x40,0xF8,0x00}, // 122 z
    {0x10,0x20,0x20,0x40,0x20,0x20,0x10,0x00}, // 123 {
    {0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00}, // 124 |
    {0x40,0x20,0x20,0x10,0x20,0x20,0x40,0x00}, // 125 }
    {0x00,0x00,0x40,0xA8,0x10,0x00,0x00,0x00}, // 126 ~
};

// Draw a filled rectangle
static void draw_rect(uint8_t *fb, int width, int x, int y, int w, int h, uint8_t color) {
    for (int dy = 0; dy < h; dy++) {
        if (y + dy < 0 || y + dy >= 240) continue;
        for (int dx = 0; dx < w; dx++) {
            if (x + dx < 0 || x + dx >= width) continue;
            fb[(y + dy) * width + (x + dx)] = color;
        }
    }
}

// Draw a character using the 6x8 bitmap font
static void draw_char(uint8_t *fb, int fb_width, int x, int y, char c, uint8_t color) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx > 94) return;
    
    const uint8_t *glyph = font_6x8[idx];
    
    for (int row = 0; row < 8; row++) {
        if (y + row < 0 || y + row >= 240) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 6; col++) {
            if (x + col < 0 || x + col >= fb_width) continue;
            if (bits & (0x80 >> col)) {
                fb[(y + row) * fb_width + (x + col)] = color;
            }
        }
    }
}

// Draw a string
static void draw_string(uint8_t *fb, int fb_width, int x, int y, const char *str, uint8_t color) {
    while (*str) {
        draw_char(fb, fb_width, x, y, *str, color);
        x += CHAR_WIDTH;
        str++;
    }
}

// Draw a string with truncation and ellipsis
static void draw_string_truncated(uint8_t *fb, int fb_width, int x, int y, const char *str, int max_chars, uint8_t color) {
    int len = strlen(str);
    if (len <= max_chars) {
        draw_string(fb, fb_width, x, y, str, color);
    } else {
        // Draw truncated with "..." at end
        for (int i = 0; i < max_chars - 3; i++) {
            draw_char(fb, fb_width, x + i * CHAR_WIDTH, y, str[i], color);
        }
        draw_string(fb, fb_width, x + (max_chars - 3) * CHAR_WIDTH, y, "...", color);
    }
}

// Draw inverted header bar (Mac-style)
static void draw_header(uint8_t *fb, int fb_width, int x, int y, int w, const char *title) {
    // White background
    draw_rect(fb, fb_width, x, y, w, HEADER_HEIGHT, COLOR_HEADER_BG);
    
    // Center the title
    int title_len = strlen(title);
    int title_x = x + (w - title_len * CHAR_WIDTH) / 2;
    int title_y = y + (HEADER_HEIGHT - CHAR_HEIGHT) / 2;
    
    // Black text on white background
    draw_string(fb, fb_width, title_x, title_y, title, COLOR_HEADER_FG);
}

// Draw a menu item (with optional inversion for selection)
static void draw_menu_item(uint8_t *fb, int fb_width, int x, int y, int w, const char *text, int max_chars, bool selected) {
    if (selected) {
        // Inverted: white background, black text
        draw_rect(fb, fb_width, x, y, w, LINE_HEIGHT, COLOR_HEADER_BG);
        draw_string_truncated(fb, fb_width, x + 2, y + 1, text, max_chars, COLOR_HEADER_FG);
    } else {
        // Normal: black background, white text
        draw_rect(fb, fb_width, x, y, w, LINE_HEIGHT, COLOR_BG);
        draw_string_truncated(fb, fb_width, x + 2, y + 1, text, max_chars, COLOR_TEXT);
    }
}

// Draw a border frame
static void draw_border(uint8_t *fb, int fb_width, int x, int y, int w, int h) {
    // Top and bottom
    draw_rect(fb, fb_width, x, y, w, 1, COLOR_BORDER);
    draw_rect(fb, fb_width, x, y + h - 1, w, 1, COLOR_BORDER);
    // Left and right
    draw_rect(fb, fb_width, x, y, 1, h, COLOR_BORDER);
    draw_rect(fb, fb_width, x + w - 1, y, 1, h, COLOR_BORDER);
}

// Draw a scrollbar on the right side
// x, y: position of scrollbar area
// h: height of scrollbar area
// total_items: total number of items
// visible_items: number of visible items
// scroll_pos: current scroll position (first visible item index)
static void draw_scrollbar(uint8_t *fb, int fb_width, int x, int y, int h, int total_items, int visible_items, int scroll_pos) {
    if (total_items <= visible_items) {
        return;  // No scrollbar needed
    }
    
    // Draw scrollbar track (dim)
    draw_rect(fb, fb_width, x, y, 4, h, COLOR_BG);
    draw_rect(fb, fb_width, x, y, 1, h, 8);  // Dim gray track
    
    // Calculate thumb position and size
    int thumb_h = (h * visible_items) / total_items;
    if (thumb_h < 8) thumb_h = 8;  // Minimum thumb size
    
    int max_scroll = total_items - visible_items;
    int thumb_y = y + ((h - thumb_h) * scroll_pos) / max_scroll;
    
    // Draw thumb (bright)
    draw_rect(fb, fb_width, x, thumb_y, 4, thumb_h, COLOR_BORDER);
}

void disk_ui_init(void) {
    ui_state = DISK_UI_HIDDEN;
    selected_drive = 0;
    selected_file = 0;
    selected_action = 0;
    scroll_offset = 0;
}

void disk_ui_init_with_emulator(mii_t *mii, int disk2_slot) {
    disk_ui_init();
    g_mii = mii;
    g_disk2_slot = disk2_slot;
    MII_DEBUG_PRINTF("Disk UI initialized with mii=%p, slot=%d\n", mii, disk2_slot);
}

extern uint8_t vram[2 * RAM_PAGES_PER_POOL * RAM_PAGE_SIZE];
extern FIL fp;

void disk_ui_show(void) {
    if (ui_state == DISK_UI_HIDDEN) {
        { // TODO: error handling
            gpio_put(PICO_DEFAULT_LED_PIN, true);
            f_open(&fp, "/tmp/apple.snap", FA_CREATE_ALWAYS | FA_WRITE);
            UINT wb;
            f_write(&fp, vram, sizeof(vram), &wb); // TODO: save only pages, requerid to be saved
            f_close(&fp);
            gpio_put(PICO_DEFAULT_LED_PIN, false);
        }
        memset(vram, 0, sizeof(vram));

        gpio_put(PICO_DEFAULT_LED_PIN, true);
        // Scan for disk images
        int count = disk_scan_directory();
        printf("Found %d disk images\n", count);
        gpio_put(PICO_DEFAULT_LED_PIN, false);

        ui_state = DISK_UI_SELECT_DRIVE;
        selected_drive = 0;
        ui_dirty = true;
        ui_rendered = false;
        MII_DEBUG_PRINTF("Disk UI: showing drive selection\n");
    }
}

void disk_ui_hide(void) {
    { // TODO: error handling
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        if (FR_OK == f_open(&fp, "/tmp/apple.snap", FA_READ)) {
            UINT rb;
            f_read(&fp, vram, sizeof(vram), &rb);
            f_close(&fp);
        }
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    ui_state = DISK_UI_HIDDEN;
    ui_rendered = false;
    ui_dirty = false;
    g_last_framebuffer = NULL;
    MII_DEBUG_PRINTF("Disk UI: hidden\n");
}

void disk_ui_toggle(void) {
    if (ui_state == DISK_UI_HIDDEN) {
        disk_ui_show();
    } else {
        disk_ui_hide();
    }
}

bool __scratch_x() disk_ui_is_visible(void) {
    __dmb();
    return ui_state != DISK_UI_HIDDEN;
}

bool disk_ui_needs_redraw(void) {
    return ui_dirty || !ui_rendered;
}

int disk_ui_get_selected_drive(void) {
    return selected_drive;
}

// Show the loading screen
void disk_ui_show_loading(void) {
    ui_state = DISK_UI_LOADING;
    ui_dirty = true;
    ui_rendered = false;
}

// Handle loading complete - mount disk and perform action
static void handle_disk_loaded(void) {
    disk_ui_hide();
    if (g_mii) {
        int preserve_state = (selected_action == 1) ? 1 : 0;  // INSERT preserves state
        if (disk_mount_to_emulator(selected_drive, g_mii, g_disk2_slot, preserve_state) == 0) {
            printf("Disk UI: disk mounted successfully\n");
            
            if (selected_action == 0) {  // BOOT
                printf("Disk UI: resetting CPU for disk boot\n");
                mii_reset(g_mii, true);
                
                mii_bank_t *sw_bank = &g_mii->bank[MII_BANK_SW];
                mii_bank_poke(sw_bank, SWKBD, 0);
                mii_bank_poke(sw_bank, SWAKD, 0);
                clear_held_key();
                
                uint8_t sw_byte = 0;
                mii_mem_access(g_mii, SWINTCXROMOFF, &sw_byte, true, true);
                printf("Disk UI: CPU reset complete\n");
            } else {  // INSERT
                printf("Disk UI: disk inserted (no reset)\n");
                
                mii_bank_t *sw_bank = &g_mii->bank[MII_BANK_SW];
                mii_bank_poke(sw_bank, SWKBD, 0);
                mii_bank_poke(sw_bank, SWAKD, 0);
                mii_bank_poke(sw_bank, 0xc061, 0);
                mii_bank_poke(sw_bank, 0xc062, 0);
                mii_bank_poke(sw_bank, 0xc063, 0);
                clear_held_key();
            }
        } else {
            MII_DEBUG_PRINTF("Disk UI: failed to mount disk to emulator\n");
        }
    } else {
        MII_DEBUG_PRINTF("Disk UI: warning - no emulator reference, disk not mounted\n");
    }
}

bool disk_ui_handle_key(uint8_t key) {
    if (ui_state == DISK_UI_HIDDEN || ui_state == DISK_UI_LOADING) {
        return false;
    }
    
    MII_DEBUG_PRINTF("Disk UI key: 0x%02X in state %d\n", key, ui_state);
    
    bool handled = false;
    
    switch (key) {
        case 0x1B:  // Escape
            if (ui_state == DISK_UI_SELECT_FILE) {
                ui_state = DISK_UI_SELECT_DRIVE;
                ui_dirty = true;
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                ui_state = DISK_UI_SELECT_FILE;
                ui_dirty = true;
            } else {
                disk_ui_hide();
            }
            handled = true;
            break;
            
        case 0x0D:  // Enter
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                // Proceed to file selection
                ui_state = DISK_UI_SELECT_FILE;
                selected_file = 0;
                scroll_offset = 0;
                ui_dirty = true;
                MII_DEBUG_PRINTF("Disk UI: selecting file for drive %d\n", selected_drive + 1);
            } else if (ui_state == DISK_UI_SELECT_FILE) {
                // Proceed to action selection
                ui_state = DISK_UI_SELECT_ACTION;
                selected_action = 0;  // Default to Boot
                ui_dirty = true;
                MII_DEBUG_PRINTF("Disk UI: selecting action for file %d\n", selected_file);
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                if (selected_action == 2) {  // Cancel
                    ui_state = DISK_UI_SELECT_FILE;
                    ui_dirty = true;
                } else {
                    // Boot or Insert - show loading screen and load disk
                    MII_DEBUG_PRINTF("Disk UI: loading disk %d to drive %d (%s)\n", 
                           selected_file, selected_drive + 1,
                           selected_action == 0 ? "BOOT" : "INSERT");
                    
                    disk_ui_show_loading();
                    
                    if (disk_load_image(selected_drive, selected_file) == 0) {
                        handle_disk_loaded();
                    } else {
                        // Failed to load - go back to file selection
                        ui_state = DISK_UI_SELECT_FILE;
                        ui_dirty = true;
                    }
                }
            }
            handled = true;
            break;
            
        case 0x08:  // Left arrow / backspace
        case 0x0B:  // Up arrow
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                // Wrap around: 0 -> 1, 1 -> 0
                selected_drive = 1 - selected_drive;
                ui_dirty = true;
            } else if (ui_state == DISK_UI_SELECT_FILE) {
                if (g_disk_count > 0) {
                    if (selected_file > 0) {
                        selected_file--;
                    } else {
                        // Wrap to last item
                        selected_file = g_disk_count - 1;
                        scroll_offset = (g_disk_count > MAX_VISIBLE) ? g_disk_count - MAX_VISIBLE : 0;
                    }
                    if (selected_file < scroll_offset) {
                        scroll_offset = selected_file;
                    }
                    ui_dirty = true;
                }
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                if (selected_action > 0) {
                    selected_action--;
                } else {
                    selected_action = 2;  // Wrap to Cancel
                }
                ui_dirty = true;
            }
            handled = true;
            break;
            
        case 0x15:  // Right arrow
        case 0x0A:  // Down arrow
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                // Wrap around: 0 -> 1, 1 -> 0
                selected_drive = 1 - selected_drive;
                ui_dirty = true;
            } else if (ui_state == DISK_UI_SELECT_FILE) {
                if (g_disk_count > 0) {
                    if (selected_file < g_disk_count - 1) {
                        selected_file++;
                    } else {
                        // Wrap to first item
                        selected_file = 0;
                        scroll_offset = 0;
                    }
                    if (selected_file >= scroll_offset + MAX_VISIBLE) {
                        scroll_offset = selected_file - MAX_VISIBLE + 1;
                    }
                    ui_dirty = true;
                }
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                if (selected_action < 2) {
                    selected_action++;
                } else {
                    selected_action = 0;  // Wrap to Boot
                }
                ui_dirty = true;
            }
            handled = true;
            break;
            
        case '1':
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                selected_drive = 0;
                ui_state = DISK_UI_SELECT_FILE;
                selected_file = 0;
                scroll_offset = 0;
                ui_dirty = true;
            }
            handled = true;
            break;
            
        case '2':
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                selected_drive = 1;
                ui_state = DISK_UI_SELECT_FILE;
                selected_file = 0;
                scroll_offset = 0;
                ui_dirty = true;
            }
            handled = true;
            break;
    }
    
    return handled;
}

void disk_ui_render(uint8_t *framebuffer, int width, int height) {
    disk_ui_state_t state = ui_state;
    
    if (state == DISK_UI_HIDDEN) {
        return;
    }

    if (framebuffer != g_last_framebuffer) {
        ui_dirty = true;
        ui_rendered = false;
        g_last_framebuffer = framebuffer;
    }
    
    if (!ui_dirty && ui_rendered) {
        return;
    }
    
    (void)height;
    
    int drive = selected_drive;
    int sel_file = selected_file;
    int sel_action = selected_action;
    int scroll = scroll_offset;
    
    int content_x = UI_X + UI_PADDING;
    int content_y = UI_Y + HEADER_HEIGHT + UI_PADDING;
    int content_width = UI_WIDTH - UI_PADDING * 2;
    int max_chars = (content_width - 4) / CHAR_WIDTH;
    
    // Always do full redraw for simplicity
    // Draw dialog background
    draw_rect(framebuffer, width, UI_X, UI_Y, UI_WIDTH, UI_HEIGHT, COLOR_BG);
    
    // Draw border
    draw_border(framebuffer, width, UI_X, UI_Y, UI_WIDTH, UI_HEIGHT);
    
    if (state == DISK_UI_LOADING) {
        // Loading screen
        draw_header(framebuffer, width, UI_X, UI_Y, UI_WIDTH, " Loading... ");
        
        int msg_y = UI_Y + UI_HEIGHT / 2 - CHAR_HEIGHT / 2;
        draw_string(framebuffer, width, content_x + 80, msg_y, "Please wait...", COLOR_TEXT);
        
    } else if (state == DISK_UI_SELECT_DRIVE) {
        // Drive selection
        draw_header(framebuffer, width, UI_X, UI_Y, UI_WIDTH, " Select Drive ");
        
        int y = content_y + 8;
        
        // Drive 1
        char drive1_text[64];
        if (g_loaded_disks[0].loaded) {
            snprintf(drive1_text, sizeof(drive1_text), "Drive 1: %.32s", g_loaded_disks[0].filename);
        } else {
            strcpy(drive1_text, "Drive 1: (empty)");
        }
        draw_menu_item(framebuffer, width, content_x, y, content_width, drive1_text, max_chars, drive == 0);
        y += LINE_HEIGHT + 2;
        
        // Drive 2
        char drive2_text[64];
        if (g_loaded_disks[1].loaded) {
            snprintf(drive2_text, sizeof(drive2_text), "Drive 2: %.32s", g_loaded_disks[1].filename);
        } else {
            strcpy(drive2_text, "Drive 2: (empty)");
        }
        draw_menu_item(framebuffer, width, content_x, y, content_width, drive2_text, max_chars, drive == 1);
        
        // Instructions below dialog border - clear area first
        int footer_y = UI_Y + UI_HEIGHT + 4;
        draw_rect(framebuffer, width, UI_X, footer_y, UI_WIDTH, LINE_HEIGHT, COLOR_BG);
        draw_string(framebuffer, width, content_x, footer_y, "[1/2] Select  [Enter] OK  [Esc] Cancel", COLOR_TEXT);
        
    } else if (state == DISK_UI_SELECT_FILE) {
        // File selection
        char title[32];
        snprintf(title, sizeof(title), " Drive %d - Select Disk ", drive + 1);
        draw_header(framebuffer, width, UI_X, UI_Y, UI_WIDTH, title);
        
        int y = content_y;
        
        if (g_disk_count == 0) {
            draw_string(framebuffer, width, content_x, y, "No disk images found", COLOR_TEXT);
            draw_string(framebuffer, width, content_x, y + LINE_HEIGHT, "Place .dsk/.woz/.nib files in /apple", COLOR_TEXT);
        } else {
            // Calculate visible range
            int visible = (g_disk_count < MAX_VISIBLE) ? g_disk_count : MAX_VISIBLE;
            int list_height = visible * LINE_HEIGHT;
            
            for (int i = 0; i < visible; i++) {
                int idx = scroll + i;
                if (idx >= g_disk_count) break;
                
                bool is_selected = (idx == sel_file);
                // Leave room for scrollbar (6 pixels)
                draw_menu_item(framebuffer, width, content_x, y, content_width - 8, 
                              g_disk_list[idx].filename, max_chars - 2, is_selected);
                y += LINE_HEIGHT;
            }
            
            // Draw scrollbar if needed
            if (g_disk_count > MAX_VISIBLE) {
                int scrollbar_x = UI_X + UI_WIDTH - UI_PADDING - 4;
                draw_scrollbar(framebuffer, width, scrollbar_x, content_y, list_height, 
                              g_disk_count, visible, scroll);
            }
        }
        
        // Instructions below dialog border - clear area first
        int footer_y = UI_Y + UI_HEIGHT + 4;
        draw_rect(framebuffer, width, UI_X, footer_y, UI_WIDTH, LINE_HEIGHT, COLOR_BG);
        draw_string(framebuffer, width, content_x, footer_y, "[Up/Dn] Select  [Enter] OK  [Esc] Back", COLOR_TEXT);
        
    } else if (state == DISK_UI_SELECT_ACTION) {
        // Action selection
        char title[48];
        snprintf(title, sizeof(title), " Drive %d ", drive + 1);
        draw_header(framebuffer, width, UI_X, UI_Y, UI_WIDTH, title);
        
        int y = content_y + 4;
        
        // Show selected file
        char file_label[64];
        snprintf(file_label, sizeof(file_label), "File: %.40s", g_disk_list[sel_file].filename);
        draw_string_truncated(framebuffer, width, content_x, y, file_label, max_chars, COLOR_TEXT);
        y += LINE_HEIGHT + 8;
        
        // Action options
        draw_string(framebuffer, width, content_x, y, "Select action:", COLOR_TEXT);
        y += LINE_HEIGHT + 4;
        
        draw_menu_item(framebuffer, width, content_x + 10, y, content_width - 20, 
                      "Boot   - Insert and reboot", max_chars - 4, sel_action == 0);
        y += LINE_HEIGHT + 2;
        
        draw_menu_item(framebuffer, width, content_x + 10, y, content_width - 20,
                      "Insert - Swap disk (no reboot)", max_chars - 4, sel_action == 1);
        y += LINE_HEIGHT + 2;
        
        draw_menu_item(framebuffer, width, content_x + 10, y, content_width - 20,
                      "Cancel", max_chars - 4, sel_action == 2);
        
        // Instructions below dialog border - clear area first
        int footer_y = UI_Y + UI_HEIGHT + 4;
        draw_rect(framebuffer, width, UI_X, footer_y, UI_WIDTH, LINE_HEIGHT, COLOR_BG);
        draw_string(framebuffer, width, content_x, footer_y, "[Up/Dn] Select  [Enter] OK  [Esc] Back", COLOR_TEXT);
    }
    
    ui_dirty = false;
    ui_rendered = true;
}
