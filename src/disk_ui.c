/*
 * disk_ui.c
 * 
 * Simple text-based disk selector UI for murmapple
 * Renders a modal dialog over the Apple II display
 */

#include <stdio.h>
#include <string.h>
#include "disk_ui.h"
#include "disk_loader.h"
#include "mii.h"
#include "mii_sw.h"
#include "mii_bank.h"

// External function to clear held key state (from main.c)
extern void clear_held_key(void);

// Emulator reference (for mounting disks)
static mii_t *g_mii = NULL;
static int g_disk2_slot = 6;  // Default slot for Disk II

// UI state - volatile to prevent race conditions between cores
static volatile disk_ui_state_t ui_state = DISK_UI_HIDDEN;
static volatile int selected_drive = 0;      // 0 or 1
static volatile disk_action_t selected_action = DISK_ACTION_BOOT;  // Boot or Insert
static volatile int selected_index = 0;      // Currently highlighted disk
static volatile int scroll_offset = 0;       // For scrolling long lists
static volatile bool ui_dirty = false;       // True when UI needs redraw
static volatile bool ui_rendered = false;    // True when UI has been rendered at least once

// With double-buffering, the render target alternates each frame.
// Track the last framebuffer pointer so we can force a redraw when it changes.
static uint8_t *g_last_framebuffer = NULL;

// UI dimensions
#define UI_X        40      // Left edge in 320px mode
#define UI_Y        40      // Top edge in 240px mode  
#define UI_WIDTH    240     // Dialog width
#define UI_HEIGHT   160     // Dialog height
#define UI_PADDING  8       // Padding inside dialog
#define CHAR_WIDTH  8       // Character width in pixels
#define CHAR_HEIGHT 8       // Character height in pixels
#define MAX_VISIBLE 12      // Max visible items (reduced for better fit)

// Colors (palette indices)
#define COLOR_BG        0   // Black
#define COLOR_BORDER    15  // White
#define COLOR_TEXT      15  // White
#define COLOR_HIGHLIGHT 4   // Green
#define COLOR_TITLE     9   // Orange

// 8x8 bitmap font - ASCII 32-127
// Each character is 8 bytes, each byte is a row (MSB = left pixel)
static const uint8_t font_8x8[][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 Space
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 !
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // 34 "
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // 35 #
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // 36 $
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // 37 %
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // 38 &
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 39 '
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // 40 (
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // 41 )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 *
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 43 +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // 44 ,
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 45 -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 46 .
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // 47 /
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // 48 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 49 1
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, // 50 2
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // 51 3
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // 52 4
    {0xFE,0xC0,0xC0,0xFC,0x06,0xC6,0x7C,0x00}, // 53 5
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // 54 6
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // 55 7
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // 56 8
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // 57 9
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // 58 :
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // 59 ;
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 60 <
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, // 61 =
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // 62 >
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // 63 ?
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, // 64 @
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 65 A
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 66 B
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 67 C
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 68 D
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 69 E
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 70 F
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3A,0x00}, // 71 G
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 72 H
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 73 I
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 74 J
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 75 K
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 76 L
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, // 77 M
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 78 N
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 79 O
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 80 P
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, // 81 Q
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 82 R
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 83 S
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00}, // 84 T
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 85 U
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 86 V
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 87 W
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 88 X
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, // 89 Y
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, // 90 Z
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 91 [
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // 92 backslash
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 93 ]
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // 94 ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 95 _
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // 96 `
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, // 97 a
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, // 98 b
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 99 c
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, // 100 d
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 101 e
    {0x38,0x6C,0x60,0xF8,0x60,0x60,0xF0,0x00}, // 102 f
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, // 103 g
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, // 104 h
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 105 i
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, // 106 j
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, // 107 k
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 108 l
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, // 109 m
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, // 110 n
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 111 o
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, // 112 p
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, // 113 q
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, // 114 r
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, // 115 s
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, // 116 t
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, // 117 u
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 118 v
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00}, // 119 w
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 120 x
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, // 121 y
    {0x00,0x00,0x7E,0x4C,0x18,0x32,0x7E,0x00}, // 122 z
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // 123 {
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 124 |
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // 125 }
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // 126 ~
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

// Draw a character using the 8x8 bitmap font
static void draw_char(uint8_t *fb, int fb_width, int x, int y, char c, uint8_t color) {
    // Map character to font index (ASCII 32-126)
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx > 94) return;  // Out of range
    
    const uint8_t *glyph = font_8x8[idx];
    
    for (int row = 0; row < 8; row++) {
        if (y + row < 0 || y + row >= 240) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
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
        x += 8;  // Move to next character position
        str++;
    }
}

void disk_ui_init(void) {
    ui_state = DISK_UI_HIDDEN;
    selected_drive = 0;
    selected_index = 0;
    scroll_offset = 0;
}

void disk_ui_init_with_emulator(mii_t *mii, int disk2_slot) {
    disk_ui_init();
    g_mii = mii;
    g_disk2_slot = disk2_slot;
    printf("Disk UI initialized with mii=%p, slot=%d\n", mii, disk2_slot);
}

void disk_ui_show(void) {
    if (ui_state == DISK_UI_HIDDEN) {
        ui_state = DISK_UI_SELECT_DRIVE;
        selected_drive = 0;
        ui_dirty = true;
        ui_rendered = false;
        printf("Disk UI: showing drive selection\n");
    }
}

void disk_ui_hide(void) {
    ui_state = DISK_UI_HIDDEN;
    ui_rendered = false;
    ui_dirty = false;
    g_last_framebuffer = NULL;
    printf("Disk UI: hidden\n");
}

void disk_ui_toggle(void) {
    if (ui_state == DISK_UI_HIDDEN) {
        disk_ui_show();
    } else {
        disk_ui_hide();
    }
}

bool disk_ui_is_visible(void) {
    return ui_state != DISK_UI_HIDDEN;
}

// Check if UI needs redraw (called from render loop)
bool disk_ui_needs_redraw(void) {
    return ui_dirty || !ui_rendered;
}

int disk_ui_get_selected_drive(void) {
    return selected_drive;
}

bool disk_ui_handle_key(uint8_t key) {
    if (ui_state == DISK_UI_HIDDEN) {
        return false;
    }
    
    printf("Disk UI key: 0x%02X in state %d\n", key, ui_state);
    
    bool handled = false;
    
    switch (key) {
        case 0x1B:  // Escape
            if (ui_state == DISK_UI_SELECT_DISK) {
                ui_state = DISK_UI_SELECT_ACTION;
                ui_dirty = true;
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                ui_state = DISK_UI_SELECT_DRIVE;
                ui_dirty = true;
            } else {
                disk_ui_hide();
            }
            handled = true;
            break;
            
        case 0x0D:  // Enter
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                ui_state = DISK_UI_SELECT_ACTION;
                selected_action = DISK_ACTION_BOOT;  // Default to boot
                ui_dirty = true;
                printf("Disk UI: selecting action for drive %d\n", selected_drive + 1);
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                ui_state = DISK_UI_SELECT_DISK;
                selected_index = 0;
                scroll_offset = 0;
                ui_dirty = true;
                printf("Disk UI: selecting disk for drive %d (%s mode)\n", 
                       selected_drive + 1, 
                       selected_action == DISK_ACTION_BOOT ? "BOOT" : "INSERT");
            } else if (ui_state == DISK_UI_SELECT_DISK) {
                // Load the selected disk to PSRAM
                printf("Disk UI: loading disk %d to drive %d (%s)\n", 
                       selected_index, selected_drive + 1,
                       selected_action == DISK_ACTION_BOOT ? "BOOT" : "INSERT");
                if (disk_load_image(selected_drive, selected_index) == 0) {
                    // Mount the disk to the emulator
                    // For INSERT mode, preserve drive state (motor, head position)
                    int preserve_state = (selected_action == DISK_ACTION_INSERT) ? 1 : 0;
                    if (g_mii) {
                        if (disk_mount_to_emulator(selected_drive, g_mii, g_disk2_slot, preserve_state) == 0) {
                            printf("Disk UI: disk mounted successfully\n");
                            
                            if (selected_action == DISK_ACTION_BOOT) {
                                // BOOT mode: Reset the CPU so it can boot from the new disk
                                printf("Disk UI: resetting CPU for disk boot\n");
                                mii_reset(g_mii, true);
                                
                                // Clear keyboard state so no spurious key is pending
                                mii_bank_t *sw_bank = &g_mii->bank[MII_BANK_SW];
                                mii_bank_poke(sw_bank, SWKBD, 0);
                                mii_bank_poke(sw_bank, SWAKD, 0);
                                
                                // Clear held key tracking to prevent re-latching
                                clear_held_key();
                                
                                // Make sure slot ROMs are visible for PR#6 / disk boot.
                                uint8_t sw_byte = 0;
                                mii_mem_access(g_mii, SWINTCXROMOFF, &sw_byte, true, true);
                                
                                printf("Disk UI: CPU reset complete\n");
                            } else {
                                // INSERT mode: Just swap the disk, don't reset
                                printf("Disk UI: disk inserted (no reset)\n");
                                
                                // Clear keyboard state so old keypresses don't affect the game
                                mii_bank_t *sw_bank = &g_mii->bank[MII_BANK_SW];
                                mii_bank_poke(sw_bank, SWKBD, 0);
                                mii_bank_poke(sw_bank, SWAKD, 0);
                                
                                // Clear JOYSTICK BUTTONS too - these can skip title screens!
                                mii_bank_poke(sw_bank, 0xc061, 0);  // Button 0
                                mii_bank_poke(sw_bank, 0xc062, 0);  // Button 1
                                mii_bank_poke(sw_bank, 0xc063, 0);  // Button 2
                                
                                // Clear held key tracking
                                clear_held_key();
                            }
                        } else {
                            printf("Disk UI: failed to mount disk to emulator\n");
                        }
                    } else {
                        printf("Disk UI: warning - no emulator reference, disk not mounted\n");
                    }
                    disk_ui_hide();
                }
            }
            handled = true;
            break;
            
        case 0x08:  // Left arrow / backspace
        case 0x0B:  // Up arrow
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                if (selected_drive != 0) {
                    selected_drive = 0;
                    ui_dirty = true;
                }
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                if (selected_action != DISK_ACTION_BOOT) {
                    selected_action = DISK_ACTION_BOOT;
                    ui_dirty = true;
                }
            } else if (ui_state == DISK_UI_SELECT_DISK) {
                if (selected_index > 0) {
                    selected_index--;
                    if (selected_index < scroll_offset) {
                        scroll_offset = selected_index;
                    }
                    ui_dirty = true;
                }
            }
            handled = true;
            break;
            
        case 0x15:  // Right arrow
        case 0x0A:  // Down arrow
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                if (selected_drive != 1) {
                    selected_drive = 1;
                    ui_dirty = true;
                }
            } else if (ui_state == DISK_UI_SELECT_ACTION) {
                if (selected_action != DISK_ACTION_INSERT) {
                    selected_action = DISK_ACTION_INSERT;
                    ui_dirty = true;
                }
            } else if (ui_state == DISK_UI_SELECT_DISK) {
                if (selected_index < g_disk_count - 1) {
                    selected_index++;
                    if (selected_index >= scroll_offset + MAX_VISIBLE) {
                        scroll_offset = selected_index - MAX_VISIBLE + 1;
                    }
                    ui_dirty = true;
                }
            }
            handled = true;
            break;
            
        case '1':
            if (selected_drive != 0) {
                selected_drive = 0;
                ui_dirty = true;
            }
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                ui_state = DISK_UI_SELECT_DISK;
                selected_index = 0;
                scroll_offset = 0;
                ui_dirty = true;
            }
            handled = true;
            break;
            
        case '2':
            if (selected_drive != 1) {
                selected_drive = 1;
                ui_dirty = true;
            }
            if (ui_state == DISK_UI_SELECT_DRIVE) {
                ui_state = DISK_UI_SELECT_DISK;
                selected_index = 0;
                scroll_offset = 0;
                ui_dirty = true;
            }
            handled = true;
            break;
    }
    
    return handled;
}

void disk_ui_render(uint8_t *framebuffer, int width, int height) {
    // Read volatile state once to avoid race conditions
    disk_ui_state_t state = ui_state;
    
    if (state == DISK_UI_HIDDEN) {
        return;
    }

    // If we're drawing into a different buffer than last time (double-buffering),
    // force a full redraw so the UI persists across buffer flips.
    if (framebuffer != g_last_framebuffer) {
        ui_dirty = true;
        ui_rendered = false;
        g_last_framebuffer = framebuffer;
    }
    
    // Only redraw if dirty or not yet rendered
    if (!ui_dirty && ui_rendered) {
        return;
    }
    
    int drive = selected_drive;
    int sel_idx = selected_index;
    int scroll = scroll_offset;
    
    (void)height;  // Unused
    
    // Track if this is first render (need full draw) or update (partial redraw)
    bool full_redraw = !ui_rendered;
    
    int text_x = UI_X + UI_PADDING;
    int text_y = UI_Y + UI_PADDING;
    
    if (full_redraw) {
        // Draw dialog background
        draw_rect(framebuffer, width, UI_X, UI_Y, UI_WIDTH, UI_HEIGHT, COLOR_BG);
        
        // Draw border
        draw_rect(framebuffer, width, UI_X, UI_Y, UI_WIDTH, 2, COLOR_BORDER);
        draw_rect(framebuffer, width, UI_X, UI_Y + UI_HEIGHT - 2, UI_WIDTH, 2, COLOR_BORDER);
        draw_rect(framebuffer, width, UI_X, UI_Y, 2, UI_HEIGHT, COLOR_BORDER);
        draw_rect(framebuffer, width, UI_X + UI_WIDTH - 2, UI_Y, 2, UI_HEIGHT, COLOR_BORDER);
    }
    
    if (state == DISK_UI_SELECT_DRIVE) {
        if (full_redraw) {
            // Title
            draw_string(framebuffer, width, text_x, text_y, "SELECT DRIVE", COLOR_TITLE);
        }
        text_y += 16;
        
        // Drive 1 - always redraw both drives for selection changes
        int item_width = UI_WIDTH - UI_PADDING * 2;
        draw_rect(framebuffer, width, text_x - 4, text_y, item_width, 12, COLOR_BG);
        uint8_t color1 = (drive == 0) ? COLOR_HIGHLIGHT : COLOR_TEXT;
        draw_string(framebuffer, width, text_x, text_y, "1 - DRIVE 1", color1);
        if (g_loaded_disks[0].loaded) {
            draw_string(framebuffer, width, text_x + 96, text_y, g_loaded_disks[0].filename, color1);
        }
        text_y += 14;
        
        // Drive 2
        draw_rect(framebuffer, width, text_x - 4, text_y, item_width, 12, COLOR_BG);
        uint8_t color2 = (drive == 1) ? COLOR_HIGHLIGHT : COLOR_TEXT;
        draw_string(framebuffer, width, text_x, text_y, "2 - DRIVE 2", color2);
        if (g_loaded_disks[1].loaded) {
            draw_string(framebuffer, width, text_x + 96, text_y, g_loaded_disks[1].filename, color2);
        }
        
        if (full_redraw) {
            // Instructions
            draw_string(framebuffer, width, text_x, UI_Y + UI_HEIGHT - 14, "ENTER=OK  ESC=CANCEL", COLOR_TEXT);
        }
        
    } else if (state == DISK_UI_SELECT_ACTION) {
        if (full_redraw) {
            // Title
            char title[32];
            snprintf(title, sizeof(title), "DRIVE %d - SELECT ACTION", drive + 1);
            draw_string(framebuffer, width, text_x, text_y, title, COLOR_TITLE);
        }
        text_y += 20;
        
        // Action options
        int item_width = UI_WIDTH - UI_PADDING * 2;
        disk_action_t action = selected_action;
        
        // BOOT option
        draw_rect(framebuffer, width, text_x - 4, text_y, item_width, 12, COLOR_BG);
        uint8_t color_boot = (action == DISK_ACTION_BOOT) ? COLOR_HIGHLIGHT : COLOR_TEXT;
        draw_string(framebuffer, width, text_x, text_y, "BOOT   - Insert disk & reboot", color_boot);
        text_y += 16;
        
        // INSERT option
        draw_rect(framebuffer, width, text_x - 4, text_y, item_width, 12, COLOR_BG);
        uint8_t color_insert = (action == DISK_ACTION_INSERT) ? COLOR_HIGHLIGHT : COLOR_TEXT;
        draw_string(framebuffer, width, text_x, text_y, "INSERT - Swap disk (no reboot)", color_insert);
        
        if (full_redraw) {
            // Instructions
            draw_string(framebuffer, width, text_x, UI_Y + UI_HEIGHT - 14, "ENTER=OK  ESC=BACK", COLOR_TEXT);
        }
        
    } else if (state == DISK_UI_SELECT_DISK) {
        if (full_redraw) {
            // Title
            char title[32];
            snprintf(title, sizeof(title), "DRIVE %d - SELECT DISK", drive + 1);
            draw_string(framebuffer, width, text_x, text_y, title, COLOR_TITLE);
        }
        text_y += 14;
        
        // Disk list - redraw all visible items with their backgrounds
        if (g_disk_count == 0) {
            if (full_redraw) {
                draw_string(framebuffer, width, text_x, text_y, "NO DISKS FOUND", COLOR_TEXT);
            }
        } else {
            int visible = (g_disk_count < MAX_VISIBLE) ? g_disk_count : MAX_VISIBLE;
            int item_width = UI_WIDTH - UI_PADDING * 2;
            
            for (int i = 0; i < visible; i++) {
                int idx = scroll + i;
                if (idx >= g_disk_count) break;
                
                bool is_selected = (idx == sel_idx);
                
                // Draw item background (clears previous state)
                uint8_t bg_color = is_selected ? COLOR_HIGHLIGHT : COLOR_BG;
                draw_rect(framebuffer, width, text_x - 4, text_y, item_width, 10, bg_color);
                
                // Draw text
                uint8_t text_color = is_selected ? COLOR_BG : COLOR_TEXT;
                
                // Truncate filename if too long (28 chars max)
                char name[32];
                strncpy(name, g_disk_list[idx].filename, 28);
                name[28] = '\0';
                
                draw_string(framebuffer, width, text_x, text_y + 1, name, text_color);
                text_y += 11;
            }
        }
        
        if (full_redraw) {
            // Instructions
            draw_string(framebuffer, width, text_x, UI_Y + UI_HEIGHT - 14, "UP/DN  ENTER=LOAD  ESC=BACK", COLOR_TEXT);
        }
    }
    
    // Mark as rendered
    ui_dirty = false;
    ui_rendered = true;
}
