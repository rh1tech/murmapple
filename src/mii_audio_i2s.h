/*
 * mii_audio_i2s.h
 * 
 * I2S audio output driver for Apple IIe emulator on RP2350
 * Handles speaker clicks and Mockingboard audio via I2S DAC
 */
#ifndef MII_AUDIO_I2S_H
#define MII_AUDIO_I2S_H

#include <stdint.h>
#include <stdbool.h>

// Audio sample rate (CD quality)
#define MII_I2S_SAMPLE_RATE     22050

// Audio buffer size in samples (per channel)
// At 22050 Hz, 512 samples = ~23ms latency
#define MII_I2S_BUFFER_SAMPLES  512

// Number of audio buffers for double/triple buffering
#define MII_I2S_BUFFER_COUNT    3

// Initialize I2S audio subsystem
// Returns true on success
bool mii_audio_i2s_init(void);

// Shutdown I2S audio
void mii_audio_i2s_shutdown(void);

// Check if audio is initialized
bool mii_audio_i2s_is_init(void);

// Speaker click - toggles the 1-bit speaker output
// Called when $C030 is accessed
void mii_audio_speaker_click(uint64_t cycle);

// Set Mockingboard audio sample for mixing
// left/right are -32768 to 32767 (signed 16-bit)
void mii_audio_mockingboard_sample(int16_t left, int16_t right);

// Enable/disable Mockingboard audio mixing
void mii_audio_mockingboard_enable(bool enable);

// Update audio - called periodically to push samples to I2S
// Returns number of samples written
int mii_audio_update(uint64_t current_cycle, uint64_t cycles_per_second);

// Sync audio cycle counter with CPU cycle (call after reset or disk load)
void mii_audio_sync_cycle(uint64_t cpu_cycle);

// Get speaker output level (for visualization)
int16_t mii_audio_get_speaker_level(void);

// Test beep - plays a simple tone to verify I2S output
void mii_audio_test_beep(int frequency_hz, int duration_ms);

#endif // MII_AUDIO_I2S_H
