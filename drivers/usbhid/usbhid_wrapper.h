/*
 * USB HID Wrapper for Apple IIe Emulator
 * Provides interface to USB keyboard and gamepad
 * 
 * When USB_HID_ENABLED is not defined, provides empty stub functions
 * so the code compiles but USB HID is disabled.
 * 
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef USBHID_WRAPPER_H
#define USBHID_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USB_HID_ENABLED

/**
 * Initialize USB HID wrapper
 * Call during system initialization
 */
void usbhid_wrapper_init(void);

/**
 * Poll USB HID devices
 * Call every frame to process USB events
 */
void usbhid_wrapper_poll(void);

/**
 * Check if USB keyboard is connected
 * @return Non-zero if a USB keyboard is connected
 */
int usbhid_wrapper_keyboard_connected(void);

/**
 * Check if USB gamepad is connected
 * @return Non-zero if a USB gamepad is connected
 */
int usbhid_wrapper_gamepad_connected(void);

/**
 * Get the next key event from the USB keyboard queue
 * Works like ps2kbd_get_key() - returns Apple IIe key codes
 * @param pressed Output: 1 if key pressed, 0 if released
 * @param key Output: Apple IIe ASCII key code (or special function key code)
 * @return Non-zero if a key event was available
 */
int usbhid_wrapper_get_key(int *pressed, unsigned char *key);

/**
 * Get current USB keyboard modifier state
 * @return HID modifier bits (same as ps2kbd_get_modifiers)
 */
uint8_t usbhid_wrapper_get_modifiers(void);

/**
 * Check if Ctrl+Alt+Delete is pressed on USB keyboard
 * @return true if reset combo is active
 */
bool usbhid_wrapper_is_reset_combo(void);

/**
 * Get USB gamepad button state in NES-style format
 * Compatible with nespad_state format for easy merging
 * @return Button state bits (DPAD_UP, DPAD_DOWN, etc.)
 */
uint32_t usbhid_wrapper_get_gamepad_state(void);

#else // !USB_HID_ENABLED

// Stub functions when USB HID is disabled
static inline void usbhid_wrapper_init(void) {}
static inline void usbhid_wrapper_poll(void) {}
static inline int usbhid_wrapper_keyboard_connected(void) { return 0; }
static inline int usbhid_wrapper_gamepad_connected(void) { return 0; }
static inline int usbhid_wrapper_get_key(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }
static inline uint8_t usbhid_wrapper_get_modifiers(void) { return 0; }
static inline bool usbhid_wrapper_is_reset_combo(void) { return false; }
static inline uint32_t usbhid_wrapper_get_gamepad_state(void) { return 0; }

#endif // USB_HID_ENABLED

#ifdef __cplusplus
}
#endif

#endif /* USBHID_WRAPPER_H */
