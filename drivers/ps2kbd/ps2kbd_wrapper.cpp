/*
 * ps2kbd_wrapper.cpp - PS/2 Keyboard wrapper for Apple IIe emulator
 * Simplified version without DOOM dependencies
 */

#include "../../src/board_config.h"
#include "ps2kbd_wrapper.h"
#include "ps2kbd_mrmltr.h"
#include <queue>

struct KeyEvent {
    int pressed;
    unsigned char key;
};

static std::queue<KeyEvent> event_queue;

static bool turbo_latched = false;   // Scroll Lock
static bool turbo_momentary = false; // F12 held
static bool show_speed = false; // F12 held

bool __not_in_flash() ps2kbd_is_turbo(void) {
    return turbo_latched || turbo_momentary;
}

bool __not_in_flash() ps2kbd_is_show_speed(void) {
    return show_speed;
}

// HID to Apple II ASCII mapping
// Returns the Apple II ASCII character for a given HID keycode
// Special return values:
//   0xF1 = F1 key (reserved)
//   0xFB = F11 key (disk selector)
//   0xFC = F12 key (reserved)
//   0xFD - PgUp
//   0xFE - PgDown
static unsigned char hid_to_apple2(uint8_t code, uint8_t modifiers) {
    bool shift = (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    bool ctrl = (modifiers & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
    
    // Function keys - return special codes
    if (code >= 0x3A && code <= 0x45) {  // F1-F12
        return 0xF1 + (code - 0x3A);  // F1=0xF1, F2=0xF2, ... F11=0xFB, F12=0xFC
    }
    
    // Letters - Apple II Monitor expects uppercase
    if (code >= 0x04 && code <= 0x1D) {
        char c = 'A' + (code - 0x04);
        if (ctrl) {
            return c - 'A' + 1;  // Control characters
        }
        // Always return uppercase for Monitor compatibility
        return c;
    }
    
    // Numbers and their shift symbols
    if (code >= 0x1E && code <= 0x27) {
        static const char num_chars[] = "1234567890";
        static const char shift_chars[] = "!@#$%^&*()";
        int idx = (code == 0x27) ? 9 : (code - 0x1E);
        return shift ? shift_chars[idx] : num_chars[idx];
    }
    
    // Special keys
    switch (code) {
        case 0x28: return 0x0D;  // Enter
        case 0x29: return 0x1B;  // Escape
        case 0x2A: return 0x08;  // Backspace (left arrow/delete on Apple II)
        case 0x2B: return 0x09;  // Tab
        case 0x2C: return ' ';   // Space
        
        // Punctuation
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        
        // Arrow keys (Apple II control codes)
        case 0x4F: return 0x15;  // Right arrow (CTRL+U)
        case 0x50: return 0x08;  // Left arrow (Backspace)
        case 0x51: return 0x0A;  // Down arrow (CTRL+J, line feed)
        case 0x52: return 0x0B;  // Up arrow (CTRL+K)
        case 0x4B: return 0xFD;  // Page Up
        case 0x4E: return 0xFE;  // Page Down

        default: return 0;
    }
}

static uint8_t current_modifiers = 0;

// Track raw HID arrow key state for joystick emulation
// Bits: 0=right, 1=left, 2=down, 3=up
static uint8_t arrow_key_state = 0;

// Track if Delete key is currently pressed (for Ctrl+Alt+Delete combo)
static bool delete_key_pressed = false;

static void key_handler(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev) {
    // Store current modifiers for use in key mapping
    current_modifiers = curr->modifier;
    
    // Update arrow key state and Delete key from current report
    arrow_key_state = 0;
    delete_key_pressed = false;
    for (int i = 0; i < 6; i++) {
        if (curr->keycode[i] == 0x4F) arrow_key_state |= 0x01;  // Right
        if (curr->keycode[i] == 0x50) arrow_key_state |= 0x02;  // Left
        if (curr->keycode[i] == 0x51) arrow_key_state |= 0x04;  // Down
        if (curr->keycode[i] == 0x52) arrow_key_state |= 0x08;  // Up
        if (curr->keycode[i] == 0x4C) delete_key_pressed = true;  // Delete
    }
    
    // Check for key presses (in curr but not in prev)
    for (int i = 0; i < 6; i++) {
        if (curr->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (prev->keycode[j] == curr->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Key pressed
                uint8_t kc = curr->keycode[i];
                // F9 — toggle show CPU speed
                if (kc == 0x42) {
                    show_speed = !show_speed;
                    continue;
                }
                // F12 — momentary turbo
                if (kc == 0x45) {
                    turbo_momentary = true;
                    continue;
                }
                // Scroll Lock — toggle turbo
                if (kc == 0x47) {
                    turbo_latched = !turbo_latched;
                    continue;
                }
                unsigned char k = hid_to_apple2(kc, curr->modifier);
                if (k) {
                    event_queue.push({1, k});
                }
            }
        }
    }
    
    // Check for key releases (in prev but not in curr)
    for (int i = 0; i < 6; i++) {
        if (prev->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (curr->keycode[j] == prev->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Key released
                uint8_t kc = prev->keycode[i];
                // F12 release — disable momentary turbo
                if (kc == 0x45) {
                    turbo_momentary = false;
                    continue;
                }                
                unsigned char k = hid_to_apple2(kc, prev->modifier);
                if (k) {
                    event_queue.push({0, k});
                }
            }
        }
    }
    
    // Handle Open Apple (Left Alt) and Solid Apple (Right Alt) as button events
    // These are handled separately by the emulator via modifier tracking
}

static Ps2Kbd_Mrmltr* kbd = nullptr;

void ps2kbd_init(void) {
    // Ps2Kbd_Mrmltr constructor takes (pio, gpio, keyHandler)
    static Ps2Kbd_Mrmltr kbd_instance(pio0, PS2_PIN_CLK, key_handler);
    kbd = &kbd_instance;
    kbd->init_gpio();
}

void ps2kbd_tick(void) {
    if (kbd) {
        kbd->tick();
    }
}

int ps2kbd_get_key(int* pressed, unsigned char* key) {
    if (event_queue.empty()) {
        return 0;
    }
    KeyEvent ev = event_queue.front();
    event_queue.pop();
    *pressed = ev.pressed;
    *key = ev.key;
    return 1;
}

// Get current modifier state (for Open Apple / Solid Apple buttons)
uint8_t ps2kbd_get_modifiers(void) {
    return current_modifiers;
}

// Get current arrow key state for joystick emulation
// Returns: bits 0=right, 1=left, 2=down, 3=up
uint8_t ps2kbd_get_arrow_state(void) {
    return arrow_key_state;
}

// Check if Ctrl+Alt+Delete is pressed (for system reset)
bool ps2kbd_is_reset_combo(void) {
    bool ctrl = (current_modifiers & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
    bool alt = (current_modifiers & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) != 0;
    return ctrl && alt && delete_key_pressed;
}
