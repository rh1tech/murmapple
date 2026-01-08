/*
 * USB HID Host Application Callbacks
 * Implements TinyUSB Host callbacks for keyboard and mouse
 * 
 * Based on TinyUSB HID host example
 * SPDX-License-Identifier: MIT
 */

#include "tusb.h"
#include "usbhid.h"
#include <stdio.h>
#include <string.h>
#include "debug_log.h"

#define HID_DEBUG_PRINTF(...) MII_DEBUG_PRINTF(__VA_ARGS__)

// Only compile if USB Host is enabled
#if CFG_TUH_ENABLED

//--------------------------------------------------------------------
// Internal state
//--------------------------------------------------------------------

#define MAX_REPORT 4

// Per-device, per-instance HID info for generic report parsing
// Index by dev_addr * CFG_TUH_HID + instance (simplified indexing)
static struct {
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

// Previous keyboard report for detecting key changes
static hid_keyboard_report_t prev_kbd_report = { 0, 0, {0} };

// Previous mouse report for detecting button changes  
static hid_mouse_report_t prev_mouse_report = { 0 };

// Accumulated mouse movement
static volatile int16_t cumulative_dx = 0;
static volatile int16_t cumulative_dy = 0;
static volatile int8_t cumulative_wheel = 0;
static volatile uint8_t current_buttons = 0;
static volatile int mouse_has_motion = 0;

// Gamepad state
static volatile int8_t gamepad_axis_x = 0;
static volatile int8_t gamepad_axis_y = 0;
static volatile uint8_t gamepad_dpad = 0;
static volatile uint16_t gamepad_buttons = 0;
static volatile int gamepad_connected = 0;

// Device connection state
static volatile int keyboard_connected = 0;
static volatile int mouse_connected = 0;

// Key action queue (for detecting press/release)
#define KEY_ACTION_QUEUE_SIZE 32
typedef struct {
    uint8_t keycode;
    int down;
} key_action_t;

static key_action_t key_action_queue[KEY_ACTION_QUEUE_SIZE];
static volatile int key_action_head = 0;
static volatile int key_action_tail = 0;

//--------------------------------------------------------------------
// Internal functions
//--------------------------------------------------------------------

static void queue_key_action(uint8_t keycode, int down) {
    int next_head = (key_action_head + 1) % KEY_ACTION_QUEUE_SIZE;
    if (next_head != key_action_tail) {
        key_action_queue[key_action_head].keycode = keycode;
        key_action_queue[key_action_head].down = down;
        key_action_head = next_head;
    }
}

static int find_keycode_in_report(hid_keyboard_report_t const *report, uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) return 1;
    }
    return 0;
}

// Forward declarations
static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report);
static void process_mouse_report(hid_mouse_report_t const *report);
static void process_gamepad_report(uint8_t const *report, uint16_t len);
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);

//--------------------------------------------------------------------
// Process keyboard report
//--------------------------------------------------------------------

static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
    // Handle modifier changes
    uint8_t released_mods = prev_report->modifier & ~(report->modifier);
    uint8_t pressed_mods = report->modifier & ~(prev_report->modifier);
    
    // Map modifier bits to pseudo-keycodes (using high byte to distinguish)
    // These will be translated in the wrapper
    if (released_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) {
        queue_key_action(0xE1, 0); // SHIFT released (HID Left Shift)
    }
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) {
        queue_key_action(0xE1, 1); // SHIFT pressed
    }
    if (released_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
        queue_key_action(0xE0, 0); // CTRL released
    }
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
        queue_key_action(0xE0, 1); // CTRL pressed
    }
    if (released_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) {
        queue_key_action(0xE2, 0); // ALT released
    }
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) {
        queue_key_action(0xE2, 1); // ALT pressed
    }
    
    // Check for released keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = prev_report->keycode[i];
        if (keycode && !find_keycode_in_report(report, keycode)) {
            queue_key_action(keycode, 0); // Key released
        }
    }
    
    // Check for pressed keys
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_keycode_in_report(prev_report, keycode)) {
            queue_key_action(keycode, 1); // Key pressed
        }
    }
}

//--------------------------------------------------------------------
// Process mouse report
//--------------------------------------------------------------------

static void process_mouse_report(hid_mouse_report_t const *report) {
    // Standard boot protocol mouse report
    // Note: Y axis inverted for DOOM (positive Y = forward in game)
    cumulative_dx += report->x;
    cumulative_dy += -report->y;  // Invert Y for correct forward/back
    cumulative_wheel += report->wheel;
    current_buttons = report->buttons & 0x07;
    
    if (report->x != 0 || report->y != 0) {
        mouse_has_motion = 1;
    }
    
    prev_mouse_report = *report;
}

//--------------------------------------------------------------------
// Process gamepad report
// Handles common USB gamepad formats (Xbox-style, generic HID)
//--------------------------------------------------------------------

static void process_gamepad_report(uint8_t const *report, uint16_t len) {
    // Safety check
    if (report == NULL || len < 7) return;
    
    // Debug: log raw D-pad bytes continuously when not centered
#if ENABLE_DEBUG_LOGS
    static uint8_t prev_dpad_x = 0x7F, prev_dpad_y = 0x7F;
    if (report[3] != prev_dpad_x || report[4] != prev_dpad_y) {
        HID_DEBUG_PRINTF("DPAD: X=%02X Y=%02X\n", report[3], report[4]);
        prev_dpad_x = report[3];
        prev_dpad_y = report[4];
    }
#endif
    
    // Format discovered:
    // Byte 3: D-pad X (0x00=Left, 0x7F=center, 0xFF=Right)
    // Byte 4: D-pad Y (0x00=Up, 0x7F=center, 0xFF=Down)
    // Byte 5: A(0x20), B(0x40), X(0x10), Y(0x80)
    // Byte 6: L-shift(0x01), R-shift(0x02), Select(0x10), Start(0x20)
    
    // D-pad from bytes 3-4 (with thresholds around center 0x7F/0x80)
    // Some gamepads send 0x00/0xFF for extremes, others send values closer to center
    gamepad_dpad = 0;
    if (report[3] < 0x60) gamepad_dpad |= 0x04; // Left (0x00-0x5F)
    if (report[3] > 0x9F) gamepad_dpad |= 0x08; // Right (0xA0-0xFF)
    if (report[4] < 0x60) gamepad_dpad |= 0x01; // Up (0x00-0x5F)
    if (report[4] > 0x80) gamepad_dpad |= 0x02; // Down (0x81-0xFF) - lowered threshold
    
    // Buttons
    gamepad_buttons = 0;
    if (report[5] & 0x20) gamepad_buttons |= 0x01; // A → Genesis A
    if (report[5] & 0x40) gamepad_buttons |= 0x02; // B → Genesis B
    if (report[5] & 0x80) gamepad_buttons |= 0x04; // Y → Genesis C
    if (report[5] & 0x10) gamepad_buttons |= 0x08; // X → Genesis X
    if (report[6] & 0x01) gamepad_buttons |= 0x10; // L-shift → Genesis Y
    if (report[6] & 0x02) gamepad_buttons |= 0x20; // R-shift → Genesis Z
    if (report[6] & 0x20) gamepad_buttons |= 0x40; // Start → Start
    if (report[6] & 0x10) gamepad_buttons |= 0x80; // Select → Mode
}

//--------------------------------------------------------------------
// Process generic HID report
//--------------------------------------------------------------------

static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)dev_addr;
    
    // Bounds check
    if (instance >= CFG_TUH_HID || report == NULL || len == 0) {
        return;
    }
    
    uint8_t const rpt_count = hid_info[instance].report_count;
    if (rpt_count == 0 || rpt_count > MAX_REPORT) {
        return;
    }
    
    tuh_hid_report_info_t *rpt_info_arr = hid_info[instance].report_info;
    tuh_hid_report_info_t *rpt_info = NULL;
    
    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
        // Simple report without report ID
        rpt_info = &rpt_info_arr[0];
    } else {
        // Composite report, first byte is report ID
        uint8_t const rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count && i < MAX_REPORT; i++) {
            if (rpt_id == rpt_info_arr[i].report_id) {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }
    
    if (!rpt_info) {
        return;
    }
    
    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        switch (rpt_info->usage) {
            case HID_USAGE_DESKTOP_KEYBOARD:
                process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
                break;
            case HID_USAGE_DESKTOP_MOUSE:
                process_mouse_report((hid_mouse_report_t const *)report);
                break;
            case HID_USAGE_DESKTOP_GAMEPAD:
            case HID_USAGE_DESKTOP_JOYSTICK:
                process_gamepad_report(report, len);
                break;
            default:
                break;
        }
    }
}

//--------------------------------------------------------------------
// TinyUSB Callbacks
//--------------------------------------------------------------------

// Invoked when HID device is mounted
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    
    HID_DEBUG_PRINTF("USB HID mount: dev=%d inst=%d proto=%d desc_len=%d\n", 
                     dev_addr, instance, itf_protocol, desc_len);
    
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 1;
        HID_DEBUG_PRINTF("  -> Keyboard connected\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 1;
        HID_DEBUG_PRINTF("  -> Mouse connected\n");
    }
    
    // Parse generic report descriptor
    if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        HID_DEBUG_PRINTF("  -> Generic HID device\n");
        // Bounds check instance
        if (instance >= CFG_TUH_HID) {
            // Instance out of range, skip parsing
            if (!tuh_hid_receive_report(dev_addr, instance)) {
                // Failed to request report
            }
            return;
        }
        
        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
        
        // Check if it's a gamepad/joystick
        for (uint8_t i = 0; i < hid_info[instance].report_count && i < MAX_REPORT; i++) {
            if (hid_info[instance].report_info[i].usage_page == HID_USAGE_PAGE_DESKTOP) {
                uint8_t usage = hid_info[instance].report_info[i].usage;
                if (usage == HID_USAGE_DESKTOP_GAMEPAD || usage == HID_USAGE_DESKTOP_JOYSTICK) {
                    gamepad_connected = 1;
                }
            }
        }
    }
    
    // Request to receive reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        // Failed to request report
    }
}

// Invoked when HID device is unmounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 0;
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 0;
    } else if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        // Could be a gamepad
        gamepad_connected = 0;
        gamepad_buttons = 0;
        gamepad_dpad = 0;
        gamepad_axis_x = 128;
        gamepad_axis_y = 128;
    }
}

// Invoked when report is received
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    
    // Debug: log all HID reports
    static uint32_t report_counter = 0;
#if ENABLE_DEBUG_LOGS
    if ((report_counter++ % 60) == 0) {
        HID_DEBUG_PRINTF("HID[%d] proto=%d len=%d: ", instance, itf_protocol, len);
        for (int i = 0; i < len && i < 12; i++) {
            HID_DEBUG_PRINTF("%02X ", report[i]);
        }
        HID_DEBUG_PRINTF("\n");
    }
#else
    (void)report_counter;
#endif
    
    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            if (report && len >= sizeof(hid_keyboard_report_t)) {
                process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
            }
            break;
        
        case HID_ITF_PROTOCOL_MOUSE:
            if (report && len >= sizeof(hid_mouse_report_t)) {
                process_mouse_report((hid_mouse_report_t const *)report);
            }
            break;
        
        default:
            // Generic device (gamepad/joystick) - simple direct parsing
            // Skip complex report descriptor parsing, just read raw data
            if (report && len >= 2) {
                gamepad_connected = 1;
                process_gamepad_report(report, len);
            }
            break;
    }
    
    // Continue receiving reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        // Failed to request next report
    }
}

//--------------------------------------------------------------------
// Public API (defined in usbhid.h)
//--------------------------------------------------------------------

void usbhid_init(void) {
    HID_DEBUG_PRINTF("USB HID: Initializing TinyUSB Host...\n");
    // Initialize TinyUSB Host
    tuh_init(BOARD_TUH_RHPORT);
    HID_DEBUG_PRINTF("USB HID: TinyUSB Host initialized\n");
    
    // Clear state
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    memset(&prev_mouse_report, 0, sizeof(prev_mouse_report));
    cumulative_dx = 0;
    cumulative_dy = 0;
    cumulative_wheel = 0;
    current_buttons = 0;
    mouse_has_motion = 0;
    key_action_head = 0;
    key_action_tail = 0;
}

void usbhid_task(void) {
    // Process USB events
    tuh_task();
}

int usbhid_keyboard_connected(void) {
    return keyboard_connected;
}

int usbhid_mouse_connected(void) {
    return mouse_connected;
}

void usbhid_get_keyboard_state(usbhid_keyboard_state_t *state) {
    if (state) {
        memcpy(state->keycode, prev_kbd_report.keycode, 6);
        state->modifier = prev_kbd_report.modifier;
        state->has_key = (key_action_head != key_action_tail);
    }
}

void usbhid_get_mouse_state(usbhid_mouse_state_t *state) {
    if (state) {
        state->dx = cumulative_dx;
        state->dy = cumulative_dy;
        state->wheel = cumulative_wheel;
        state->buttons = current_buttons;
        state->has_motion = mouse_has_motion;
        
        // Reset accumulated values
        cumulative_dx = 0;
        cumulative_dy = 0;
        cumulative_wheel = 0;
        mouse_has_motion = 0;
    }
}

int usbhid_get_key_action(uint8_t *keycode, int *down) {
    if (key_action_head == key_action_tail) {
        return 0; // No actions queued
    }
    
    *keycode = key_action_queue[key_action_tail].keycode;
    *down = key_action_queue[key_action_tail].down;
    key_action_tail = (key_action_tail + 1) % KEY_ACTION_QUEUE_SIZE;
    return 1;
}

// Gamepad API functions
int usbhid_gamepad_connected(void) {
    return gamepad_connected;
}

void usbhid_get_gamepad_state(usbhid_gamepad_state_t *state) {
    if (state) {
        state->axis_x = gamepad_axis_x;
        state->axis_y = gamepad_axis_y;
        state->dpad = gamepad_dpad;
        state->buttons = gamepad_buttons;
        state->connected = gamepad_connected;
    }
}

#endif // CFG_TUH_ENABLED
