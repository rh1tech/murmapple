/*
 * USB HID Wrapper for Apple IIe Emulator
 * Maps USB HID keyboard to Apple IIe key codes
 * Maps USB HID gamepad to NES-style button bits
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "usbhid.h"
#include "usbhid_wrapper.h"
#include <stdint.h>
#include <stdbool.h>
#include "nespad/nespad.h"

#ifdef USB_HID_ENABLED

// Track Delete key state for reset combo
static bool delete_key_pressed = false;
static uint8_t current_modifiers = 0;

//--------------------------------------------------------------------
// HID Keycode to Apple II ASCII Mapping
// Returns the Apple II ASCII character for a given HID keycode
// Special return values:
//   0xFB = F11 key (disk selector toggle)
//   0    = Ignored key
//--------------------------------------------------------------------

static unsigned char hid_to_apple2(uint8_t hid_keycode, uint8_t modifiers) {
    bool shift = (modifiers & 0x22) != 0;  // L/R Shift
    bool ctrl = (modifiers & 0x11) != 0;   // L/R Ctrl
    
    // Track Delete key state for reset combo
    if (hid_keycode == 0x4C) {
        return 0;  // Delete key doesn't produce a character
    }
    
    // Function keys - F11 toggles disk selector
    if (hid_keycode >= 0x3A && hid_keycode <= 0x45) {
        return 0xF1 + (hid_keycode - 0x3A);  // F1=0xF1, ... F11=0xFB, F12=0xFC
    }
    
    // Letters A-Z - Apple II Monitor expects uppercase
    if (hid_keycode >= 0x04 && hid_keycode <= 0x1D) {
        char c = 'A' + (hid_keycode - 0x04);
        if (ctrl) {
            return c - 'A' + 1;  // Control characters
        }
        return c;  // Always uppercase for Apple II compatibility
    }
    
    // Numbers 1-9, 0
    if (hid_keycode >= 0x1E && hid_keycode <= 0x27) {
        static const char num_chars[] = "1234567890";
        static const char shift_chars[] = "!@#$%^&*()";
        int idx = (hid_keycode == 0x27) ? 9 : (hid_keycode - 0x1E);
        return shift ? shift_chars[idx] : num_chars[idx];
    }
    
    // Special keys
    switch (hid_keycode) {
        case 0x28: return 0x0D;  // Enter
        case 0x29: return 0x1B;  // Escape
        case 0x2A: return 0x08;  // Backspace (left delete on Apple II)
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
        
        default: return 0;  // Unknown key
    }
}

//--------------------------------------------------------------------
// USB HID Wrapper API
//--------------------------------------------------------------------

void usbhid_wrapper_init(void) {
    usbhid_init();
    current_modifiers = 0;
    delete_key_pressed = false;
}

void usbhid_wrapper_poll(void) {
    usbhid_task();
    
    // Update current modifier and Delete key state from keyboard
    usbhid_keyboard_state_t kbd_state;
    usbhid_get_keyboard_state(&kbd_state);
    current_modifiers = kbd_state.modifier;
    
    // Check if Delete key (0x4C) is currently pressed
    delete_key_pressed = false;
    for (int i = 0; i < 6; i++) {
        if (kbd_state.keycode[i] == 0x4C) {
            delete_key_pressed = true;
            break;
        }
    }
}

int usbhid_wrapper_keyboard_connected(void) {
    return usbhid_keyboard_connected();
}

int usbhid_wrapper_gamepad_connected(void) {
    return usbhid_gamepad_connected();
}

int usbhid_wrapper_get_key(int *pressed, unsigned char *key) {
    uint8_t hid_keycode;
    int down;
    
    while (usbhid_get_key_action(&hid_keycode, &down)) {
        // Get current modifier state
        usbhid_keyboard_state_t kbd_state;
        usbhid_get_keyboard_state(&kbd_state);
        
        unsigned char apple2_key = hid_to_apple2(hid_keycode, kbd_state.modifier);
        if (apple2_key != 0) {
            *pressed = down;
            *key = apple2_key;
            return 1;
        }
    }
    
    return 0;
}

uint8_t usbhid_wrapper_get_modifiers(void) {
    return current_modifiers;
}

bool usbhid_wrapper_is_reset_combo(void) {
    // Check for Ctrl+Alt+Delete
    bool ctrl = (current_modifiers & 0x11) != 0;  // L/R Ctrl
    bool alt = (current_modifiers & 0x44) != 0;   // L/R Alt
    return ctrl && alt && delete_key_pressed;
}

uint32_t usbhid_wrapper_get_gamepad_state(void) {
    usbhid_gamepad_state_t gp;
    usbhid_get_gamepad_state(&gp);
    
    if (!gp.connected) {
        return 0;
    }
    
    uint32_t buttons = 0;
    
    // Map D-pad
    if (gp.dpad & 0x01) buttons |= DPAD_UP;
    if (gp.dpad & 0x02) buttons |= DPAD_DOWN;
    if (gp.dpad & 0x04) buttons |= DPAD_LEFT;
    if (gp.dpad & 0x08) buttons |= DPAD_RIGHT;
    
    // Map face buttons to NES-style
    // USB gamepad: A(0x01), B(0x02), X(0x04), Y(0x08), Start(0x40), Select(0x80)
    if (gp.buttons & 0x01) buttons |= DPAD_A;      // A -> A
    if (gp.buttons & 0x02) buttons |= DPAD_B;      // B -> B
    if (gp.buttons & 0x40) buttons |= DPAD_START;  // Start -> Start
    if (gp.buttons & 0x80) buttons |= DPAD_SELECT; // Select/Mode -> Select
    
    return buttons;
}

#endif // USB_HID_ENABLED
