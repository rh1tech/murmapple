#include "../src/board_config.h"
#include "HDMI.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdalign.h>
#include "debug_log.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "pico/platform.h"
#include "disk_ui.h"

// Flag to defer IRQ handler setup to Core 1
// When true, hdmi_init() will NOT set the IRQ handler - Core 1 must call
// graphics_init_irq_on_this_core() after starting.
static bool g_defer_irq_to_core1 = false;

// Globals expected by the driver
int graphics_buffer_width = 320;
int graphics_buffer_height = 240;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;

static volatile uint32_t graphics_frame_count = 0;

uint32_t hdmi_get_frame_count(void) {
    return graphics_frame_count;
}

uint32_t graphics_get_width(void) {
    return graphics_buffer_width;
}

uint32_t graphics_get_height(void) {
    return graphics_buffer_height;
}

void graphics_set_res(int w, int h) {
    graphics_buffer_width = w;
    graphics_buffer_height = h;
}

void graphics_set_shift(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

static struct video_mode_t video_mode[] __scratch_x("video_mode") = {
    { // 640x480 60Hz
        .h_total = 524,
        .h_width = 480,
        .freq = 60,
        .vgaPxClk = 25175000
    }
};

struct video_mode_t __scratch_x() graphics_get_video_mode(int mode) {
    return video_mode[0];
}

int __scratch_x() get_video_mode() {
    return 0;
}

void __scratch_x() vsync_handler() {
    // Called from DMA IRQ at frame boundary.
    graphics_frame_count++;
}

// --- New HDMI Driver Code ---

//PIO параметры
static uint offs_prg0 = 0;
static uint offs_prg1 = 0;

//SM
static int SM_video = -1;
static int SM_conv = -1;

//буфер  палитры 256 цветов в формате R8G8B8
static uint32_t palette[256] __scratch_x("palette");

// Color substitution map for HDMI reserved indices 240-243
static uint8_t color_substitute[4] __scratch_x("color_substitute") = {239, 239, 239, 239};


#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)

// #define HDMI_WIDTH 480 //480 Default
// #define HDMI_HEIGHT 644 //524 Default
// #define HDMI_HZ 52 //60 Default

//DMA каналы
//каналы работы с первичным графическим буфером
static int dma_chan_ctrl;
static int dma_chan;
//каналы работы с конвертацией палитры
static int dma_chan_pal_conv_ctrl;
static int dma_chan_pal_conv;

//DMA буферы
//основные строчные данные
static uint32_t* dma_lines[2] __scratch_x("dma_lines") = { NULL,NULL };
static uint32_t* DMA_BUF_ADDR[2];

//ДМА палитра для конвертации
//в хвосте этой памяти выделяется dma_data
alignas(4096) uint32_t conv_color[1224];

//индекс, проверяющий зависание
static uint32_t irq_inx = 0;
static uint32_t last_check_irq = 0;

#include "mii.h"
#include "mii_sw.h"
#include "mii_bank.h"
#include "mii_rom.h"
#include "mii_video.h"
extern mii_t g_mii;
volatile int lock_y = -1;

// Debug function to get DMA IRQ count
uint32_t hdmi_get_irq_count(void) {
    return irq_inx;
}

// Forward declaration
static inline bool hdmi_init(void);

// Check if HDMI DMA has stalled and restart if needed.
// Call this periodically during long operations to maintain HDMI signal.
bool hdmi_check_and_restart(void) {
    static uint32_t last_check_time = 0;
    uint32_t now = time_us_32();
    
    // Don't check more often than approx 2 frames (33ms) to avoid false positives
    // when called from tight loops (like SD card transfers).
    if (now - last_check_time < 33000) {
        return false;
    }

    uint32_t current = irq_inx;
    if (current == last_check_irq) {
        // IRQ count hasn't changed in >33ms - HDMI has likely stalled
        MII_DEBUG_PRINTF("HDMI: DMA stalled (irq_inx=%lu), restarting...\n", (unsigned long)current);
        hdmi_init();
        last_check_irq = irq_inx;
        last_check_time = now;
        return true;
    }
    
    last_check_irq = current;
    last_check_time = now;
    return false;
}

// Pause HDMI output - stops DMA but keeps configuration
void hdmi_pause(void) {
    // Disable IRQ
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    
    // Abort all DMA channels
    dma_hw->abort = (1 << dma_chan_ctrl) | (1 << dma_chan) | 
                    (1 << dma_chan_pal_conv) | (1 << dma_chan_pal_conv_ctrl);
    while (dma_hw->abort) tight_loop_contents();
    
    // Disable PIO state machines
    pio_sm_set_enabled(PIO_VIDEO, SM_video, false);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, false);
}

// Resume HDMI output - restarts everything
void hdmi_resume(void) {
    // Full reinit is the safest way to restart
    hdmi_init();
}

//функции и константы HDMI

#define BASE_HDMI_CTRL_INX (240)
//программа конвертации адреса

uint16_t pio_program_instructions_conv_HDMI[] = {
    //         //     .wrap_target
    0x80a0, //  0: pull   block
    0x40e8, //  1: in     osr, 8
    0x4034, //  2: in     x, 20
    0x8020, //  3: push   block
    //     .wrap
};


const struct pio_program pio_program_conv_addr_HDMI = {
    .instructions = pio_program_instructions_conv_HDMI,
    .length = 4,
    .origin = -1,
};

//программа видеовывода
static const uint16_t instructions_PIO_HDMI[] = {
    0x7006, //  0: out    pins, 6         side 2
    0x7006, //  1: out    pins, 6         side 2
    0x7006, //  2: out    pins, 6         side 2
    0x7006, //  3: out    pins, 6         side 2
    0x7006, //  4: out    pins, 6         side 2
    0x6806, //  5: out    pins, 6         side 1
    0x6806, //  6: out    pins, 6         side 1
    0x6806, //  7: out    pins, 6         side 1
    0x6806, //  8: out    pins, 6         side 1
    0x6806, //  9: out    pins, 6         side 1
};

static const struct pio_program program_PIO_HDMI = {
    .instructions = instructions_PIO_HDMI,
    .length = 10,
    .origin = -1,
};

static uint64_t get_ser_diff_data(const uint16_t dataR, const uint16_t dataG, const uint16_t dataB) {
    uint64_t out64 = 0;
    for (int i = 0; i < 10; i++) {
        out64 <<= 6;
        if (i == 5) out64 <<= 2;
#ifdef PICO_PC
        uint8_t bG = (dataR >> (9 - i)) & 1;
        uint8_t bR = (dataG >> (9 - i)) & 1;
#else
        uint8_t bR = (dataR >> (9 - i)) & 1;
        uint8_t bG = (dataG >> (9 - i)) & 1;
#endif
        uint8_t bB = (dataB >> (9 - i)) & 1;

        bR |= (bR ^ 1) << 1;
        bG |= (bG ^ 1) << 1;
        bB |= (bB ^ 1) << 1;

        if (HDMI_PIN_invert_diffpairs) {
            bR ^= 0b11;
            bG ^= 0b11;
            bB ^= 0b11;
        }
        uint8_t d6;
        if (HDMI_PIN_RGB_notBGR) {
            d6 = (bR << 4) | (bG << 2) | (bB << 0);
        }
        else {
            d6 = (bB << 4) | (bG << 2) | (bR << 0);
        }


        out64 |= d6;
    }
    return out64;
}

//конвертор TMDS
static uint tmds_encoder(const uint8_t d8) {
    int s1 = 0;
    for (int i = 0; i < 8; i++) s1 += (d8 & (1 << i)) ? 1 : 0;
    bool is_xnor = false;
    if ((s1 > 4) || ((s1 == 4) && ((d8 & 1) == 0))) is_xnor = true;
    uint16_t d_out = d8 & 1;
    uint16_t qi = d_out;
    for (int i = 1; i < 8; i++) {
        d_out |= ((qi << 1) ^ (d8 & (1 << i))) ^ (is_xnor << i);
        qi = d_out & (1 << i);
    }

    if (is_xnor) d_out |= 1 << 9;
    else d_out |= 1 << 8;

    return d_out;
}

static void pio_set_x(PIO pio, const int sm, uint32_t v) {
    uint instr_shift = pio_encode_in(pio_x, 4);
    uint instr_mov = pio_encode_mov(pio_x, pio_isr);
    for (int i = 0; i < 8; i++) {
        const uint32_t nibble = (v >> (i * 4)) & 0xf;
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, nibble));
        pio_sm_exec(pio, sm, instr_shift);
    }
    pio_sm_exec(pio, sm, instr_mov);
}

static void __scratch_x() dma_handler_HDMI() {
    static uint32_t inx_buf_dma;
    static uint line = 0;
    struct video_mode_t mode = video_mode[0];
    irq_inx++;

    dma_hw->ints0 = 1u << dma_chan_ctrl;
    dma_channel_set_read_addr(dma_chan_ctrl, &DMA_BUF_ADDR[inx_buf_dma & 1], false);

    if (line >= mode.h_total ) {
        line = 0;
        vsync_handler();
    } else {
        ++line;
    }

    if ((line & 1) == 0) return;
    inx_buf_dma++;

    uint8_t* activ_buf = (uint8_t *)dma_lines[inx_buf_dma & 1];

    if (line < mode.h_width ) {
        int y = line >> 1;
        register uint8_t* input_buffer = graphics_get_buffer() + y * (SCREEN_WIDTH / 2);
        register uint8_t* output_buffer = activ_buf + 72; //для выравнивания синхры;
        // Copy from framebuffer, substituting HDMI reserved colors
        lock_y = y;
        for (register int i = 0; i < SCREEN_WIDTH / 2; i++) {
            register uint8_t c = input_buffer[i];
            register int j = i << 1;
            output_buffer[j] = c & 0xF;
            output_buffer[j+1] = c >> 4;
        }
        lock_y = -1;
        
        //ССИ
        //для выравнивания синхры

        // --|_|---|_|---|_|----
        //---|___________|-----
        memset(activ_buf + 48,BASE_HDMI_CTRL_INX, 24);
        memset(activ_buf,BASE_HDMI_CTRL_INX + 1, 48);
        memset(activ_buf + 392,BASE_HDMI_CTRL_INX, 8);

        //без выравнивания
        // --|_|---|_|---|_|----
        //------|___________|----
        //   memset(activ_buf+320,BASE_HDMI_CTRL_INX,8);
        //   memset(activ_buf+328,BASE_HDMI_CTRL_INX+1,48);
        //   memset(activ_buf+376,BASE_HDMI_CTRL_INX,24);
    }
    else {
        if ((line >= 490) && (line < 492)) {
            //кадровый синхроимпульс
            //для выравнивания синхры
            // --|_|---|_|---|_|----
            //---|___________|-----
            memset(activ_buf + 48,BASE_HDMI_CTRL_INX + 2, 352);
            memset(activ_buf,BASE_HDMI_CTRL_INX + 3, 48);
            //без выравнивания
            // --|_|---|_|---|_|----
            //-------|___________|----

            // memset(activ_buf,BASE_HDMI_CTRL_INX+2,328);
            // memset(activ_buf+328,BASE_HDMI_CTRL_INX+3,48);
            // memset(activ_buf+376,BASE_HDMI_CTRL_INX+2,24);
        }
        else {
            //ССИ без изображения
            //для выравнивания синхры

            memset(activ_buf + 48,BASE_HDMI_CTRL_INX, 352);
            memset(activ_buf,BASE_HDMI_CTRL_INX + 1, 48);

            // memset(activ_buf,BASE_HDMI_CTRL_INX,328);
            // memset(activ_buf+328,BASE_HDMI_CTRL_INX+1,48);
            // memset(activ_buf+376,BASE_HDMI_CTRL_INX,24);
        };
    }


    // y=(y==524)?0:(y+1);
    // inx_buf_dma++;
}


static inline void irq_remove_handler_DMA_core1() {
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    irq_remove_handler(VIDEO_DMA_IRQ, irq_get_exclusive_handler(VIDEO_DMA_IRQ));
}

static inline void irq_set_exclusive_handler_DMA_core1() {
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
}

void graphics_set_palette_hdmi(const uint8_t i, const uint32_t color888);

//деинициализация - инициализация ресурсов
static inline bool hdmi_init() {
    //выключение прерывания DMA
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(dma_chan_ctrl, false);
    }
    else {
        dma_channel_set_irq1_enabled(dma_chan_ctrl, false);
    }

    irq_remove_handler_DMA_core1();


    //остановка всех каналов DMA
    dma_hw->abort = (1 << dma_chan_ctrl) | (1 << dma_chan) | (1 << dma_chan_pal_conv) | (
                        1 << dma_chan_pal_conv_ctrl);
    while (dma_hw->abort) tight_loop_contents();

    //выключение SM основной и конвертора

#if ZERO2
    pio_set_gpio_base(PIO_VIDEO, 16);
    pio_set_gpio_base(PIO_VIDEO_ADDR, 16);
#endif

    // pio_sm_restart(PIO_VIDEO, SM_video);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, false);

    //pio_sm_restart(PIO_VIDEO_ADDR, SM_conv);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, false);


    //удаление программ из соответствующих PIO
    pio_remove_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI, offs_prg1);
    pio_remove_program(PIO_VIDEO, &program_PIO_HDMI, offs_prg0);


    offs_prg1 = pio_add_program(PIO_VIDEO_ADDR, &pio_program_conv_addr_HDMI);
    offs_prg0 = pio_add_program(PIO_VIDEO, &program_PIO_HDMI);
    MII_DEBUG_PRINTF("HDMI: PIO programs loaded at offsets %u and %u\n", offs_prg0, offs_prg1);
    
    pio_set_x(PIO_VIDEO_ADDR, SM_conv, ((uint32_t)conv_color >> 12));
    MII_DEBUG_PRINTF("HDMI: conv_color table at %p\n", conv_color);

    //заполнение палитры (skip only sync control 240-243, but initialize 244-254)
    for (int ci = 0; ci < 240; ci++) graphics_set_palette_hdmi(ci, palette[ci]);
    // Initialize 244-254 to black initially (will be updated when Doom sets palette)
    for (int ci = 244; ci < 256; ci++) {
        if (palette[ci] == 0) palette[ci] = 0x000000; // Ensure initialized
        graphics_set_palette_hdmi(ci, palette[ci]);
    }

    //240-243 служебные данные(синхра) напрямую вносим в массив -конвертер
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = BASE_HDMI_CTRL_INX;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);

    //настройка PIO SM для конвертации

    pio_sm_config c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg1, offs_prg1 + (pio_program_conv_addr_HDMI.length - 1));
    sm_config_set_in_shift(&c_c, true, false, 32);

    pio_sm_init(PIO_VIDEO_ADDR, SM_conv, offs_prg1, &c_c);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, SM_conv, true);

    //настройка PIO SM для вывода данных
    c_c = pio_get_default_sm_config();
    sm_config_set_wrap(&c_c, offs_prg0, offs_prg0 + (program_PIO_HDMI.length - 1));

    //настройка side set
    sm_config_set_sideset_pins(&c_c,beginHDMI_PIN_clk);
    sm_config_set_sideset(&c_c, 2,false,false);
    for (int i = 0; i < 2; i++) {
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_clk + i);
        gpio_set_drive_strength(beginHDMI_PIN_clk + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_clk + i, GPIO_SLEW_RATE_FAST);
    }

#if ZERO2
    // Настройка направлений пинов для state machines
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, HDMI_BASE_PIN, 8, true);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO_ADDR, SM_conv, HDMI_BASE_PIN, 8, true);

    uint64_t mask64 = (uint64_t)(3u << beginHDMI_PIN_clk);
    pio_sm_set_pins_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
    pio_sm_set_pindirs_with_mask64(PIO_VIDEO, SM_video, mask64, mask64);
    // пины
#else
    pio_sm_set_pins_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
    pio_sm_set_pindirs_with_mask(PIO_VIDEO, SM_video, 3u << beginHDMI_PIN_clk, 3u << beginHDMI_PIN_clk);
    // пины
#endif

    for (int i = 0; i < 6; i++) {
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(PIO_VIDEO, beginHDMI_PIN_data + i);
        gpio_set_drive_strength(beginHDMI_PIN_data + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(beginHDMI_PIN_data + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, SM_video, beginHDMI_PIN_data, 6, true);
    //конфигурация пинов на выход
    sm_config_set_out_pins(&c_c, beginHDMI_PIN_data, 6);

    //
    sm_config_set_out_shift(&c_c, true, true, 30);
    sm_config_set_fifo_join(&c_c, PIO_FIFO_JOIN_TX);

    int hdmi_hz = graphics_get_video_mode(get_video_mode()).freq;
    sm_config_set_clkdiv(&c_c, (clock_get_hz(clk_sys) / 252000000.0f) * (60 / hdmi_hz));
    pio_sm_init(PIO_VIDEO, SM_video, offs_prg0, &c_c);
    pio_sm_set_enabled(PIO_VIDEO, SM_video, true);

    //настройки DMA
    dma_lines[0] = &conv_color[1024];
    dma_lines[1] = &conv_color[1124];

    //основной рабочий канал
    dma_channel_config cfg_dma = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_8);
    channel_config_set_chain_to(&cfg_dma, dma_chan_ctrl); // chain to other channel
    channel_config_set_high_priority(&cfg_dma, true);  // High priority for HDMI

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);


    uint dreq = DREQ_PIO1_TX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_TX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan,
        &cfg_dma,
        &PIO_VIDEO_ADDR->txf[SM_conv], // Write address
        &dma_lines[0][0], // read address
        400, //
        false // Don't start yet
    );

    //контрольный канал для основного
    cfg_dma = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan); // chain to other channel
    channel_config_set_high_priority(&cfg_dma, true);  // High priority for HDMI

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    DMA_BUF_ADDR[0] = &dma_lines[0][0];
    DMA_BUF_ADDR[1] = &dma_lines[1][0];

    dma_channel_configure(
        dma_chan_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        &DMA_BUF_ADDR[0], // read address
        1, //
        false // Don't start yet
    );

    //канал - конвертер палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv_ctrl); // chain to other channel
    channel_config_set_high_priority(&cfg_dma, true);  // High priority for HDMI

    channel_config_set_read_increment(&cfg_dma, true);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_TX0 + SM_video;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + SM_video;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv,
        &cfg_dma,
        &PIO_VIDEO->txf[SM_video], // Write address
        &conv_color[0], // read address
        4, //
        false // Don't start yet
    );

    //канал управления конвертером палитры

    cfg_dma = dma_channel_get_default_config(dma_chan_pal_conv_ctrl);
    channel_config_set_transfer_data_size(&cfg_dma, DMA_SIZE_32);
    channel_config_set_chain_to(&cfg_dma, dma_chan_pal_conv); // chain to other channel
    channel_config_set_high_priority(&cfg_dma, true);  // High priority for HDMI

    channel_config_set_read_increment(&cfg_dma, false);
    channel_config_set_write_increment(&cfg_dma, false);

    dreq = DREQ_PIO1_RX0 + SM_conv;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_RX0 + SM_conv;

    channel_config_set_dreq(&cfg_dma, dreq);

    dma_channel_configure(
        dma_chan_pal_conv_ctrl,
        &cfg_dma,
        &dma_hw->ch[dma_chan_pal_conv].read_addr, // Write address
        &PIO_VIDEO_ADDR->rxf[SM_conv], // read address
        1, //
        true // start yet
    );

    //стартуем прерывание и канал
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_acknowledge_irq0(dma_chan_ctrl);
        dma_channel_set_irq0_enabled(dma_chan_ctrl, true);
    }
    else {
        dma_channel_acknowledge_irq1(dma_chan_ctrl);
        dma_channel_set_irq1_enabled(dma_chan_ctrl, true);
    }

    irq_set_exclusive_handler_DMA_core1();

    dma_start_channel_mask((1u << dma_chan_ctrl));

    return true;
};

void graphics_set_palette_hdmi(uint8_t i, uint32_t color888) {
    palette[i] = color888 & 0x00ffffff;

    // For HDMI sync control indices (240-243), find nearest color in range 0-239
    if (i >= 240 && i <= 243) {
        uint8_t r = (color888 >> 16) & 0xff;
        uint8_t g = (color888 >> 8) & 0xff;
        uint8_t b = color888 & 0xff;
        
        int best_match = 239;
        int best_distance = 999999;
        
        // Find closest color in palette 0-239
        for (int j = 0; j < 240; j++) {
            uint8_t pr = (palette[j] >> 16) & 0xff;
            uint8_t pg = (palette[j] >> 8) & 0xff;
            uint8_t pb = palette[j] & 0xff;
            
            int dr = r - pr;
            int dg = g - pg;
            int db = b - pb;
            int distance = dr*dr + dg*dg + db*db;
            
            if (distance < best_distance) {
                best_distance = distance;
                best_match = j;
            }
        }
        
        color_substitute[i - 240] = best_match;
        return; // Don't set hardware palette for these indices
    }

    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint8_t R = (color888 >> 16) & 0xff;
    const uint8_t G = (color888 >> 8) & 0xff;
    const uint8_t B = (color888 >> 0) & 0xff;
    conv_color64[i * 2] = get_ser_diff_data(tmds_encoder(R), tmds_encoder(G), tmds_encoder(B));
    conv_color64[i * 2 + 1] = conv_color64[i * 2] ^ 0x0003ffffffffffffl;
};

#define RGB888(r, g, b) ((r<<16) | (g << 8 ) | b )

void graphics_init_hdmi() {
    MII_DEBUG_PRINTF("HDMI: Claiming PIO resources...\n");
    
    // Initialize default palette with basic colors before hdmi_init
    // This ensures the conv_color table has valid TMDS data
    static const uint32_t default_colors[16] = {
        0x000000, 0xDD0033, 0x000099, 0xDD22DD,  // Black, Magenta, DkBlue, Purple
        0x007722, 0x555555, 0x2222FF, 0x66AAFF,  // DkGreen, Grey1, MedBlue, LtBlue
        0x885500, 0xFF6600, 0xAAAAAA, 0xFF9988,  // Brown, Orange, Grey2, Pink
        0x11DD00, 0xFFFF00, 0x44FF99, 0xFFFFFF,  // LtGreen, Yellow, Aqua, White
    };
    for (int i = 0; i < 16; i++) {
        palette[i] = default_colors[i];
    }
    // Fill rest with grayscale
    for (int i = 16; i < 240; i++) {
        uint8_t gray = (i * 255) / 239;
        palette[i] = (gray << 16) | (gray << 8) | gray;
    }
    
    // PIO и DMA
    SM_video = pio_claim_unused_sm(PIO_VIDEO, true);
    SM_conv = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    dma_chan_pal_conv_ctrl = dma_claim_unused_channel(true);
    dma_chan_pal_conv = dma_claim_unused_channel(true);

        MII_DEBUG_PRINTF("HDMI: SM_video=%u, SM_conv=%u\n", SM_video, SM_conv);
        MII_DEBUG_PRINTF("HDMI: DMA channels: ctrl=%u, data=%u, pal_ctrl=%u, pal=%u\n",
            dma_chan_ctrl, dma_chan, dma_chan_pal_conv_ctrl, dma_chan_pal_conv);
        MII_DEBUG_PRINTF("HDMI: Base pin=%d, CLK pins=%d-%d, DATA pins=%d-%d\n",
            HDMI_BASE_PIN, beginHDMI_PIN_clk, beginHDMI_PIN_clk+1,
            beginHDMI_PIN_data, beginHDMI_PIN_data+5);
        MII_DEBUG_PRINTF("HDMI: Calling hdmi_init()...\n");
    
    hdmi_init();
    
    MII_DEBUG_PRINTF("HDMI: Init complete\n");
}

void graphics_set_bgcolor_hdmi(uint32_t color888) //определяем зарезервированный цвет в палитре
{
    graphics_set_palette_hdmi(255, color888);
};

void graphics_restore_sync_colors(void) {
    // Restore HDMI sync control colors after palette updates
    uint64_t* conv_color64 = (uint64_t *)conv_color;
    const uint16_t b0 = 0b1101010100;
    const uint16_t b1 = 0b0010101011;
    const uint16_t b2 = 0b0101010100;
    const uint16_t b3 = 0b1010101011;
    const int base_inx = BASE_HDMI_CTRL_INX;

    conv_color64[2 * base_inx + 0] = get_ser_diff_data(b0, b0, b3);
    conv_color64[2 * base_inx + 1] = get_ser_diff_data(b0, b0, b3);

    conv_color64[2 * (base_inx + 1) + 0] = get_ser_diff_data(b0, b0, b2);
    conv_color64[2 * (base_inx + 1) + 1] = get_ser_diff_data(b0, b0, b2);

    conv_color64[2 * (base_inx + 2) + 0] = get_ser_diff_data(b0, b0, b1);
    conv_color64[2 * (base_inx + 2) + 1] = get_ser_diff_data(b0, b0, b1);

    conv_color64[2 * (base_inx + 3) + 0] = get_ser_diff_data(b0, b0, b0);
    conv_color64[2 * (base_inx + 3) + 1] = get_ser_diff_data(b0, b0, b0);
}

static uint8_t graphics_buffer[SCREEN_WIDTH * SCREEN_HEIGHT / 2] __aligned(4096) = { 0 };

uint8_t* graphics_get_buffer() {
    return graphics_buffer;
}

// Wrappers for existing API
void graphics_init(g_out g_out) {
    graphics_init_hdmi();
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    graphics_set_palette_hdmi(i, color888);
}

void graphics_set_bgcolor(uint32_t color888) {
    graphics_set_bgcolor_hdmi(color888);
}

void startVIDEO(uint8_t vol) {
    // Stub
}

void graphics_set_defer_irq_to_core1(bool defer) {
    g_defer_irq_to_core1 = defer;
}

void graphics_init_irq_on_this_core(void) {
    // Initialize the HDMI DMA IRQ handler on the current core.
    // Call this from Core 1 after graphics_init() was called with defer mode.
    // This ensures HDMI keeps running even when Core 0 is busy with SD card I/O.
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
    MII_DEBUG_PRINTF("HDMI: IRQ handler set on core %u\n", get_core_num());
}

void graphics_rebind_irq_to_current_core(void) {
    // Rebind the HDMI DMA IRQ handler to the current core.
    // This allows the HDMI signal to keep running even when the other core
    // is busy (e.g., doing SD card I/O).
    //
    // We need to be careful here - the DMA is running and firing interrupts.
    // We disable interrupts globally during the switch to avoid race conditions.
    
    uint32_t save = save_and_disable_interrupts();
    
    // Disable the DMA IRQ on the old core
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    
    // Clear any pending IRQ
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_acknowledge_irq0(dma_chan_ctrl);
    } else {
        dma_channel_acknowledge_irq1(dma_chan_ctrl);
    }
    
    // Set the handler on this core (Core 1)
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, dma_handler_HDMI);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
    
    restore_interrupts(save);

    MII_DEBUG_PRINTF("HDMI: IRQ handler rebound to core %u\n", get_core_num());
}

void set_palette(uint8_t n) {
    // Stub
}
