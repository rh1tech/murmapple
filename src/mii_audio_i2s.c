/*
 * mii_audio_i2s.c
 * 
 * I2S audio output driver for Apple IIe emulator on RP2350
 * Uses pico-extras pico_audio_i2s library
 */
#include "mii_audio_i2s.h"
#include "board_config.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

// We need pico_audio_i2s from pico-extras
#define none pico_audio_enum_none
#include "pico/audio_i2s.h"
#undef none

//=============================================================================
// Configuration
//=============================================================================

// PIO and DMA configuration for I2S
#ifndef PICO_AUDIO_I2S_PIO
#define PICO_AUDIO_I2S_PIO 1
#endif

#ifndef PICO_AUDIO_I2S_DMA_CHANNEL
#define PICO_AUDIO_I2S_DMA_CHANNEL 6
#endif

#ifndef PICO_AUDIO_I2S_STATE_MACHINE
#define PICO_AUDIO_I2S_STATE_MACHINE 0
#endif

// Speaker volume (0-255, controls amplitude of 1-bit clicks)
#define SPEAKER_VOLUME          192

// Mockingboard volume (0-255)
#define MOCKINGBOARD_VOLUME     255

// Low-pass filter for speaker to soften clicks
#define SPEAKER_LOW_PASS        1

//=============================================================================
// State
//=============================================================================

static struct {
    bool initialized;
    
    // Audio buffer pool
    struct audio_buffer_pool *producer_pool;
    
    // Speaker state
    int16_t speaker_level;          // Current speaker output level
    int16_t speaker_target;         // Target level (toggled on click)
    uint64_t last_click_cycle;      // CPU cycle of last click
    uint64_t samples_since_click;   // Samples generated since last click
    
    // Mockingboard state
    bool mockingboard_enabled;
    int16_t mockingboard_left;
    int16_t mockingboard_right;
    
    // Timing
    uint64_t last_update_cycle;
    uint32_t cycles_per_sample;     // CPU cycles per audio sample
    
} audio_state;

// Audio format configuration
static struct audio_format audio_format = {
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .sample_freq = MII_I2S_SAMPLE_RATE,
    .channel_count = 2,
};

static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 4  // 2 channels * 2 bytes per sample
};

//=============================================================================
// Implementation
//=============================================================================

bool mii_audio_i2s_init(void)
{
    if (audio_state.initialized) {
        return true;
    }
    
    printf("mii_audio_i2s_init: initializing I2S audio\n");
    
    // Clear state
    memset(&audio_state, 0, sizeof(audio_state));
    
    // Create audio buffer pool
    audio_state.producer_pool = audio_new_producer_pool(
        &producer_format, 
        MII_I2S_BUFFER_COUNT, 
        MII_I2S_BUFFER_SAMPLES
    );
    
    if (!audio_state.producer_pool) {
        printf("mii_audio_i2s_init: failed to create producer pool\n");
        return false;
    }
    
    // Configure I2S pins
    struct audio_i2s_config config = {
        .data_pin = I2S_DATA_PIN,
        .clock_pin_base = I2S_CLOCK_PIN_BASE,
        .dma_channel = PICO_AUDIO_I2S_DMA_CHANNEL,
        .pio_sm = PICO_AUDIO_I2S_STATE_MACHINE,
    };
    
    printf("mii_audio_i2s_init: I2S pins DATA=%d CLK=%d/%d\n",
           I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, I2S_CLOCK_PIN_BASE + 1);
    
    // Setup I2S audio
    const struct audio_format *output_format = audio_i2s_setup(&audio_format, &config);
    if (!output_format) {
        printf("mii_audio_i2s_init: audio_i2s_setup failed\n");
        return false;
    }
    
    // Increase GPIO drive strength for cleaner signal
    gpio_set_drive_strength(I2S_DATA_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(I2S_CLOCK_PIN_BASE, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(I2S_CLOCK_PIN_BASE + 1, GPIO_DRIVE_STRENGTH_12MA);
    
    // Connect producer pool to I2S output
    bool ok = audio_i2s_connect_extra(audio_state.producer_pool, false, 0, 0, NULL);
    if (!ok) {
        printf("mii_audio_i2s_init: audio_i2s_connect_extra failed\n");
        return false;
    }
    
    // Enable I2S
    audio_i2s_set_enabled(true);
    
    // Initialize speaker to neutral position
    audio_state.speaker_level = 0;
    audio_state.speaker_target = SPEAKER_VOLUME * 128;  // Start high
    
    // Calculate cycles per sample (will be updated with actual CPU speed)
    // Default: 1.023 MHz Apple II clock / 44100 Hz = ~23.2 cycles per sample
    audio_state.cycles_per_sample = 1023000 / MII_I2S_SAMPLE_RATE;
    
    audio_state.initialized = true;
    printf("mii_audio_i2s_init: initialization complete\n");
    
    return true;
}

void mii_audio_i2s_shutdown(void)
{
    if (!audio_state.initialized) {
        return;
    }
    
    audio_i2s_set_enabled(false);
    audio_state.initialized = false;
    
    printf("mii_audio_i2s_shutdown: audio disabled\n");
}

bool mii_audio_i2s_is_init(void)
{
    return audio_state.initialized;
}

void mii_audio_speaker_click(uint64_t cycle)
{
    if (!audio_state.initialized) {
        return;
    }
    
    // Toggle speaker level
    audio_state.speaker_target = -audio_state.speaker_target;
    audio_state.last_click_cycle = cycle;
    audio_state.samples_since_click = 0;
}

void mii_audio_mockingboard_sample(int16_t left, int16_t right)
{
    audio_state.mockingboard_left = left;
    audio_state.mockingboard_right = right;
}

void mii_audio_mockingboard_enable(bool enable)
{
    audio_state.mockingboard_enabled = enable;
}

int16_t mii_audio_get_speaker_level(void)
{
    return audio_state.speaker_level;
}

// Simple low-pass filter for speaker to reduce harsh clicks
static inline int16_t low_pass_filter(int16_t current, int16_t target, int alpha256)
{
    int32_t result = ((256 - alpha256) * current + alpha256 * target) / 256;
    return (int16_t)result;
}

// Clamp to int16_t range
static inline int16_t clamp_s16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

int mii_audio_update(uint64_t current_cycle, uint64_t cycles_per_second)
{
    if (!audio_state.initialized) {
        return 0;
    }
    
    // Update cycles per sample based on actual CPU speed
    audio_state.cycles_per_sample = (uint32_t)(cycles_per_second / MII_I2S_SAMPLE_RATE);
    if (audio_state.cycles_per_sample == 0) {
        audio_state.cycles_per_sample = 1;
    }
    
    int total_samples = 0;
    audio_buffer_t *buffer;
    
    // Process all available buffers
    while ((buffer = take_audio_buffer(audio_state.producer_pool, false)) != NULL) {
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        int sample_count = buffer->max_sample_count;
        
        for (int i = 0; i < sample_count; i++) {
            // Update speaker with low-pass filter for smoother sound
#if SPEAKER_LOW_PASS
            // Faster attack, slower decay
            int alpha = (audio_state.speaker_target != audio_state.speaker_level) ? 64 : 8;
            audio_state.speaker_level = low_pass_filter(
                audio_state.speaker_level,
                audio_state.speaker_target,
                alpha
            );
#else
            audio_state.speaker_level = audio_state.speaker_target;
#endif
            
            // Mix speaker and mockingboard
            int32_t left = audio_state.speaker_level;
            int32_t right = audio_state.speaker_level;
            
            if (audio_state.mockingboard_enabled) {
                left += (audio_state.mockingboard_left * MOCKINGBOARD_VOLUME) / 256;
                right += (audio_state.mockingboard_right * MOCKINGBOARD_VOLUME) / 256;
            }
            
            // Output stereo samples
            samples[i * 2] = clamp_s16(left);
            samples[i * 2 + 1] = clamp_s16(right);
            
            audio_state.samples_since_click++;
        }
        
        buffer->sample_count = sample_count;
        give_audio_buffer(audio_state.producer_pool, buffer);
        total_samples += sample_count;
    }
    
    audio_state.last_update_cycle = current_cycle;
    return total_samples;
}
