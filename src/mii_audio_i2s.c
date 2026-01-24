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
#include "debug_log.h"

#include <pico/stdlib.h>
#include <pico/stdlib.h>
#include <hardware/pwm.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/clocks.h>

#include "mii_audio.h"

// We need pico_audio_i2s from pico-extras
#if defined(FEATURE_AUDIO_I2S)
#define none pico_audio_enum_none
#include "pico/audio_i2s.h"
#undef none
#endif

//=============================================================================
// Configuration
//=============================================================================

// PIO and DMA configuration for I2S
// NOTE: HDMI uses PIO1, PS/2 uses PIO0 SM0
// So I2S uses PIO0 SM2
#ifndef PICO_AUDIO_I2S_PIO
#define PICO_AUDIO_I2S_PIO 0
#endif

#ifndef PICO_AUDIO_I2S_DMA_CHANNEL
#define PICO_AUDIO_I2S_DMA_CHANNEL 10
#endif

// NOTE: PS/2 keyboard uses PIO0 SM0, so I2S uses SM2 on PIO1
#ifndef PICO_AUDIO_I2S_STATE_MACHINE
#define PICO_AUDIO_I2S_STATE_MACHINE 2
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

// Sample buffer for apple2ts-style audio generation
// Algorithm from Kent Dickey: accumulate fractional contributions per sample
#define SAMPLE_BUFFER_SIZE 8192  // ~0.74 seconds at 11025 Hz (larger buffer)
#define SAMPLE_BUFFER_OFFSET 512 // Playback lags behind writes by this many samples (~46ms)

static struct {
    // Circular buffer of sample contributions
    // Each entry is fixed-point 8.8: positive = HIGH, negative = LOW
    // Range: -256 (all LOW) to +256 (all HIGH)
    int16_t samples[SAMPLE_BUFFER_SIZE];
    
    // Write position (where next click contribution goes)
    uint32_t write_index;
    
    // Read position (where playback reads from)
    uint32_t read_index;
    
    // Current sample position (fractional, in 16.16 fixed point)
    // MUST be uint64_t to handle large cycle counts!
    uint64_t curr_sample_frac;
    
    // Speaker value: +256 for HIGH, -256 for LOW
    int speaker_value;
    
    // Sampling rate ratio: samples per CPU cycle (16.16 fixed point)
    // 44100 / 1020484 ≈ 0.0432 ?
    uint32_t samples_per_cycle_frac;
    
} sample_buffer;

static struct {
    bool initialized;
    
    // Audio buffer pool
#if defined(FEATURE_AUDIO_I2S)
    struct audio_buffer_pool *producer_pool;
#endif    
    // For visualization
    int16_t speaker_sample;
    
    // Mockingboard state
    bool mockingboard_enabled;
    int16_t mockingboard_left;
    int16_t mockingboard_right;
    
    // Timing
    uint32_t cycles_per_sample;         // CPU cycles per audio sample (~23 at 44100Hz) ?
    
} audio_state;

// Audio format configuration
#if defined(FEATURE_AUDIO_I2S)
static struct audio_format audio_format = {
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .sample_freq = MII_I2S_SAMPLE_RATE,
    .channel_count = 2,
};

static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 4  // 2 channels * 2 bytes per sample
};
#endif
#if defined(FEATURE_AUDIO_PWM)
static int g_pwm_dma_chan = -1;
static uint g_pwm_slice = 0;

// DMA буфер: по одному 32-бит слову на сэмпл (L в low16, R в high16)
static uint32_t *g_pwm_dma_buf = NULL;
static uint32_t g_pwm_dma_count = 0;
static bool g_pwm_dma_active = false;
#endif

//=============================================================================
// Implementation
//=============================================================================

bool mii_audio_i2s_init(void)
{
    if (audio_state.initialized) {
        return true;
    }
    
    // Clear state
    memset(&audio_state, 0, sizeof(audio_state));
    
#if defined(FEATURE_AUDIO_PWM)
    // PWM pins must be adjacent for single-slice stereo via CC
    gpio_set_function(PWM_RIGHT_PIN, GPIO_FUNC_PWM);
    gpio_set_function(PWM_LEFT_PIN,  GPIO_FUNC_PWM);

    // Both pins (10/11) share the same slice
    g_pwm_slice = pwm_gpio_to_slice_num(PWM_RIGHT_PIN);
    pwm_config pcfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&pcfg, 1.0f);
    pwm_config_set_wrap(&pcfg, PWM_WRAP);
    pwm_init(g_pwm_slice, &pcfg, true);

    // init duty to mid
    pwm_set_gpio_level(PWM_RIGHT_PIN, PWM_WRAP >> 1);
    pwm_set_gpio_level(PWM_LEFT_PIN,  PWM_WRAP >> 1);

    // Allocate DMA buffer sized to ONE audio buffer worth of frames
    static uint32_t dma_buf[PWM_DMA_SAMPLES];
    g_pwm_dma_count = PWM_DMA_SAMPLES;
    g_pwm_dma_buf = dma_buf;

    g_pwm_dma_chan = dma_claim_unused_channel(true);

    dma_channel_config dcfg = dma_channel_get_default_config(g_pwm_dma_chan);
    channel_config_set_transfer_data_size(&dcfg, DMA_SIZE_32);
    channel_config_set_read_increment(&dcfg, true);
    channel_config_set_write_increment(&dcfg, false);
    channel_config_set_dreq(&dcfg, pwm_get_dreq(g_pwm_slice));

    // Destination = PWM CC register (writes both A/B in one 32-bit word)
    dma_channel_configure(
        g_pwm_dma_chan,
        &dcfg,
        &pwm_hw->slice[g_pwm_slice].cc,
        g_pwm_dma_buf,
        g_pwm_dma_count,
        false
    );
#endif
#if defined(FEATURE_AUDIO_I2S)
    // Create audio buffer pool
    audio_state.producer_pool = audio_new_producer_pool(
        &producer_format, 
        MII_I2S_BUFFER_COUNT, 
        MII_I2S_BUFFER_SAMPLES
    );
    
    if (!audio_state.producer_pool) {
        MII_DEBUG_PRINTF("mii_audio_i2s_init: failed to create producer pool\n");
        return false;
    }

    // Configure I2S pins using PICO_AUDIO_I2S_* defines
    struct audio_i2s_config config = {
        .data_pin = PICO_AUDIO_I2S_DATA_PIN,
        .clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE,
        .dma_channel = PICO_AUDIO_I2S_DMA_CHANNEL,
        .pio_sm = PICO_AUDIO_I2S_STATE_MACHINE,
    };
    
    // Setup I2S audio
    const struct audio_format *output_format = audio_i2s_setup(&audio_format, &config);
    if (!output_format) {
        return false;
    }
    
    // Increase GPIO drive strength for cleaner signal
    gpio_set_drive_strength(PICO_AUDIO_I2S_DATA_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PICO_AUDIO_I2S_CLOCK_PIN_BASE, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(PICO_AUDIO_I2S_CLOCK_PIN_BASE + 1, GPIO_DRIVE_STRENGTH_12MA);
    
    // Connect producer pool to I2S output
    bool ok = audio_i2s_connect_extra(audio_state.producer_pool, false, 0, 0, NULL);
    if (!ok) {
        MII_DEBUG_PRINTF("mii_audio_i2s_init: audio_i2s_connect_extra failed\n");
        return false;
    }
    
    // Enable I2S
    audio_i2s_set_enabled(true);
#endif
    
    // Initialize sample buffer (apple2ts style)
    memset(&sample_buffer, 0, sizeof(sample_buffer));
    sample_buffer.write_index = SAMPLE_BUFFER_OFFSET;
    sample_buffer.read_index = 0;
    sample_buffer.curr_sample_frac = 0;
    sample_buffer.speaker_value = 256;  // Start HIGH (256 = +1.0 in 8.8 fixed point)
    
    // Calculate samples per CPU cycle as 16.16 fixed point
    // 11025 / 1020484 ≈ 0.0432, in 16.16 fixed point = 2831/2 ?
    sample_buffer.samples_per_cycle_frac = (uint32_t)(((uint64_t)MII_I2S_SAMPLE_RATE << 16) / 1020484ULL);
    
    // Initialize speaker state
    audio_state.speaker_sample = 0;
    
    // Calculate cycles per sample (will be updated with actual CPU speed)
    // Default: 1.023 MHz Apple II clock / 22050 Hz = ~23.2 cycles per sample
    audio_state.cycles_per_sample = 1023000 / MII_I2S_SAMPLE_RATE;
    
    audio_state.initialized = true;
    
    return true;
}

void mii_audio_i2s_shutdown(void)
{
    if (!audio_state.initialized) {
        return;
    }
    
#if defined(FEATURE_AUDIO_I2S)
    audio_i2s_set_enabled(false);
#endif
    audio_state.initialized = false;
}

bool mii_audio_i2s_is_init(void)
{
    return audio_state.initialized;
}

// Apple2ts-style speaker click handling
// Process clicks immediately, filling the sample buffer with contributions
void mii_audio_speaker_click(uint64_t cycle)
{
    if (!audio_state.initialized) {
        return;
    }
    
    // Convert CPU cycle to sample number
    // sampling = 44100 / 1020484 ≈ 0.0432 ?
    // In 16.16 fixed point: 44100 * 65536 / 1020484 ≈ 2831 ?
    uint64_t new_sample_frac = cycle * sample_buffer.samples_per_cycle_frac;
    uint64_t new_sample = new_sample_frac >> 16;
    
    // Calculate delta from last click position
    uint64_t curr_sample = sample_buffer.curr_sample_frac >> 16;
    int64_t delta = (int64_t)(new_sample - curr_sample);
    
    // Sanity check: if delta is negative or zero, just toggle speaker
    if (delta <= 0) {
        sample_buffer.speaker_value = -sample_buffer.speaker_value;
        return;
    }
    
    // If delta is very large (> SAMPLE_BUFFER_OFFSET), this is a new sound after silence
    // Reset positions relative to read position
    if (delta > SAMPLE_BUFFER_OFFSET) {
        // Set write position just ahead of current read position
        sample_buffer.write_index = (sample_buffer.read_index + SAMPLE_BUFFER_OFFSET) % SAMPLE_BUFFER_SIZE;
        // Update curr_sample_frac so next click has correct delta
        sample_buffer.curr_sample_frac = new_sample_frac;
        sample_buffer.speaker_value = -sample_buffer.speaker_value;
        return;
    }
    
    // Fill the sample buffer from current write position to new position
    int value = sample_buffer.speaker_value;
    uint32_t fill_count = (uint32_t)delta;
    uint32_t idx = sample_buffer.write_index;
    
    // Fill samples with current speaker value
    for (uint32_t i = 0; i < fill_count && i < SAMPLE_BUFFER_SIZE; i++) {
        sample_buffer.samples[idx] = value;
        idx = (idx + 1) % SAMPLE_BUFFER_SIZE;
        
        // Safety: don't overrun read position
        if (idx == sample_buffer.read_index) {
            // About to overwrite unread data - adjust read position
            sample_buffer.read_index = (sample_buffer.read_index + 1) % SAMPLE_BUFFER_SIZE;
        }
    }
    
    // Update state
    sample_buffer.write_index = idx;
    sample_buffer.curr_sample_frac = new_sample_frac;
    
    // Toggle speaker for next segment
    sample_buffer.speaker_value = -sample_buffer.speaker_value;
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
    return audio_state.speaker_sample;
}

void mii_audio_sync_cycle(uint64_t cpu_cycle)
{
    if (!audio_state.initialized) {
        return;
    }
    
    // Reset sample buffer state
    memset(sample_buffer.samples, 0, sizeof(sample_buffer.samples));
    sample_buffer.write_index = SAMPLE_BUFFER_OFFSET;
    sample_buffer.read_index = 0;
    sample_buffer.curr_sample_frac = (cpu_cycle * sample_buffer.samples_per_cycle_frac);
    sample_buffer.speaker_value = 256;  // Start HIGH
}

// Clamp to int16_t range
static inline int16_t clamp_s16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

#if defined(FEATURE_AUDIO_PWM)
static inline uint16_t pcm16_to_pwm_u16(int16_t s) {
    uint32_t u = (uint16_t)(s + 32768);
    return (uint16_t)((u * ((uint32_t)PWM_WRAP + 1)) >> 16);
}

static void pwm_submit_one_sample(int16_t l, int16_t r)
{
    if (g_pwm_dma_active && dma_channel_is_busy(g_pwm_dma_chan))
        return;

    uint16_t pl = pcm16_to_pwm_u16(l);
    uint16_t pr = pcm16_to_pwm_u16(r);
    uint32_t v = ((uint32_t)pr << 16) | pl;

    for (uint32_t i = 0; i < PWM_OSR; i++) {
        g_pwm_dma_buf[i] = v;
    }

    dma_channel_transfer_from_buffer_now(
        g_pwm_dma_chan,
        g_pwm_dma_buf,
        PWM_OSR
    );

    g_pwm_dma_active = true;
}
#endif

int mii_audio_update(uint64_t current_cycle, uint64_t cycles_per_second)
{
    if (!audio_state.initialized) {
        return 0;
    }
    
    (void)current_cycle;  // Not used - we read from sample buffer directly
    (void)cycles_per_second;
    
    int total_samples = 0;
#if defined(FEATURE_AUDIO_I2S)
    audio_buffer_t *buffer;
    
    // Speaker volume scaling (sample_buffer values are -256 to +256)
    // Scale to 16-bit audio range
    const int32_t volume_scale = (SPEAKER_VOLUME * 128) / 256;  // ~96
    
    // Track last non-zero sample for smoothing during underruns
    static int16_t last_sample = 0;
    
    // Process all available buffers
    while ((buffer = take_audio_buffer(audio_state.producer_pool, false)) != NULL) {
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        int sample_count = buffer->max_sample_count;
        
        for (int i = 0; i < sample_count; i++) {
            // Check if we're about to underrun (read catching up to write)
            int32_t pending = (int32_t)(sample_buffer.write_index - sample_buffer.read_index);
            if (pending < 0) pending += SAMPLE_BUFFER_SIZE;
            
            int16_t contribution;
            if (pending > 0) {
                // Read from sample buffer and clear
                contribution = sample_buffer.samples[sample_buffer.read_index];
                sample_buffer.samples[sample_buffer.read_index] = 0;
                sample_buffer.read_index = (sample_buffer.read_index + 1) % SAMPLE_BUFFER_SIZE;
                last_sample = contribution;
            } else {
                // Underrun - hold last value to avoid pops
                contribution = last_sample;
            }
            
            // Scale contribution to audio sample
            int32_t sample_value = contribution * volume_scale;
            audio_state.speaker_sample = clamp_s16(sample_value);
            
            // Mix speaker and mockingboard
            int32_t left = sample_value;
            int32_t right = sample_value;
            
            if (audio_state.mockingboard_enabled) {
                left += (audio_state.mockingboard_left * MOCKINGBOARD_VOLUME) / 256;
                right += (audio_state.mockingboard_right * MOCKINGBOARD_VOLUME) / 256;
            }
            
            // Output stereo samples
            samples[i * 2] = clamp_s16(left);
            samples[i * 2 + 1] = clamp_s16(right);
        }
        
        buffer->sample_count = sample_count;
        give_audio_buffer(audio_state.producer_pool, buffer);
        total_samples += sample_count;
    }
#endif
#if defined(FEATURE_AUDIO_PWM)
    int sample_count = 1;

    // Speaker volume scaling (тот же, что и в I2S)
    const int32_t volume_scale = (SPEAKER_VOLUME * 128) / 256;
    static int16_t last_sample = 0;

    for (int i = 0; i < sample_count; i++) {
        int32_t pending = (int32_t)(sample_buffer.write_index - sample_buffer.read_index);
        if (pending < 0) pending += SAMPLE_BUFFER_SIZE;

        int16_t contribution;
        if (pending > 0) {
            contribution = sample_buffer.samples[sample_buffer.read_index];
            sample_buffer.samples[sample_buffer.read_index] = 0;
            sample_buffer.read_index = (sample_buffer.read_index + 1) % SAMPLE_BUFFER_SIZE;
            last_sample = contribution;
        } else {
            contribution = last_sample;
        }

        int32_t sample_value = contribution * volume_scale;
        audio_state.speaker_sample = clamp_s16(sample_value);

        int32_t left  = sample_value;
        int32_t right = sample_value;

        if (audio_state.mockingboard_enabled) {
            left  += (audio_state.mockingboard_left  * MOCKINGBOARD_VOLUME) / 256;
            right += (audio_state.mockingboard_right * MOCKINGBOARD_VOLUME) / 256;
        }

        pwm_submit_one_sample(clamp_s16(left), clamp_s16(right));
    }
#endif
    
    return total_samples;
}

// Test beep generator - plays a square wave beep
#if defined(FEATURE_AUDIO_I2S)
void mii_audio_test_beep(int frequency_hz, int duration_ms)
{
    if (!audio_state.initialized) {
        return;
    }
    
    int total_samples = (MII_I2S_SAMPLE_RATE * duration_ms) / 1000;
    int phase = 0;
    int phase_inc = (frequency_hz * 65536) / MII_I2S_SAMPLE_RATE;
    int samples_played = 0;
    
    while (samples_played < total_samples) {
        audio_buffer_t *buffer = take_audio_buffer(audio_state.producer_pool, true);
        if (!buffer) {
            break;
        }
        
        int16_t *samples = (int16_t *)buffer->buffer->bytes;
        int count = buffer->max_sample_count;
        if (count > (total_samples - samples_played)) {
            count = total_samples - samples_played;
        }
        
        for (int i = 0; i < count; i++) {
            int16_t value = (phase & 0x8000) ? 16000 : -16000;
            samples[i * 2] = value;
            samples[i * 2 + 1] = value;
            phase += phase_inc;
        }
        
        #if defined(FEATURE_AUDIO_PWM)
            pwm_submit_stereo_s16(samples, (uint32_t)count);
            buffer->sample_count = count;
        #endif
        #if defined(FEATURE_AUDIO_I2S)
            buffer->sample_count = count;
            give_audio_buffer(audio_state.producer_pool, buffer);
        #endif
        samples_played += count;
    }
}
#endif
