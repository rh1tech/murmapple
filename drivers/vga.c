#include "vga.h"
#include "hardware/clocks.h"
#include "stdbool.h"
#include "hardware/structs/pll.h"
#include "hardware/structs/systick.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdio.h>
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "stdlib.h"

volatile int lock_y = -1;

uint16_t pio_program_VGA_instructions[] = {
    //     .wrap_target
    0x6008, //  0: out    pins, 8
    //     .wrap
};

const struct pio_program pio_program_VGA = {
    .instructions = pio_program_VGA_instructions,
    .length = 1,
    .origin = -1,
};


static uint32_t* lines_pattern[4];
static uint32_t* lines_pattern_data = NULL;
static int _SM_VGA = -1;


static int N_lines_total = 525;
static int N_lines_visible = 480;
static int line_VS_begin = 490;
static int line_VS_end = 491;
static int shift_picture = 0;

static int visible_line_size = 320;


static int dma_chan_ctrl;
static int dma_chan;

static uint graphics_buffer_width = 0;
static uint graphics_buffer_height = 0;
static int graphics_buffer_shift_x = 0;
static int graphics_buffer_shift_y = 0;

static bool is_flash_line = false;
static bool is_flash_frame = false;

//буфер 1к графической палитры
static uint16_t palette[2][256];

static uint32_t bg_color[2];
static uint16_t palette16_mask = 0;

static uint16_t txt_palette[16];

//буфер 2К текстовой палитры для быстрой работы
static uint16_t* txt_palette_fast = NULL;
//static uint16_t txt_palette_fast[256*4];

static volatile uint32_t graphics_frame_count = 0;
static uint8_t graphics_buffer[SCREEN_WIDTH * SCREEN_HEIGHT / 2] __aligned(4096) = { 0 };

uint32_t get_frame_count(void) {
    return graphics_frame_count;
}

uint8_t* graphics_get_buffer() {
    return graphics_buffer;
}

void __scratch_x() vsync_handler() {
    // Called from DMA IRQ at frame boundary.
    graphics_frame_count++;
}


void __time_critical_func() dma_handler_VGA() {
    dma_hw->ints0 = 1u << dma_chan_ctrl;
    static uint32_t screen_line = 0;
    screen_line++;

    if (screen_line == N_lines_total) {
        screen_line = 0;
        vsync_handler();
    }

    if (screen_line >= N_lines_visible) {
        //заполнение цветом фона
        if (screen_line == N_lines_visible | screen_line == N_lines_visible + 3) {
            uint32_t* output_buffer_32bit = lines_pattern[2 + (screen_line & 1)];
            output_buffer_32bit += shift_picture / 4;
            uint32_t p_i = (screen_line && is_flash_line) + (graphics_frame_count && is_flash_frame) & 1;
            uint32_t color32 = bg_color[p_i];
            for (int i = visible_line_size / 2; i--;) {
                *output_buffer_32bit++ = color32;
            }
        }

        //синхросигналы
        if (screen_line >= line_VS_begin && screen_line <= line_VS_end)
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[1], false); //VS SYNC
        else
            dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false);
        return;
    }

    uint32_t* * output_buffer = &lines_pattern[2 + (screen_line & 1)];
    int line_number = screen_line / 2;
    if (screen_line % 2) return;
    int y = screen_line / 2 - graphics_buffer_shift_y;

    if (y < 0) {
        dma_channel_set_read_addr(dma_chan_ctrl, &lines_pattern[0], false); // TODO: ensue it is required
        return;
    }
    if (y >= graphics_buffer_height) {
        // заполнение линии цветом фона
        if (y == graphics_buffer_height | y == graphics_buffer_height + 1 |
            y == graphics_buffer_height + 2) {
            uint32_t* output_buffer_32bit = *output_buffer;
            uint32_t p_i = ((line_number && is_flash_line) + (graphics_frame_count && is_flash_frame)) & 1;
            uint32_t color32 = bg_color[p_i];

            output_buffer_32bit += shift_picture / 4;
            for (int i = visible_line_size / 2; i--;) {
                *output_buffer_32bit++ = color32;
            }
        }
        dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
        return;
    };

    //зона прорисовки изображения

    register uint16_t* output_buffer_16bit = (uint16_t *)(*output_buffer);
    output_buffer_16bit += shift_picture / 2; //смещение началы вывода на размер синхросигнала

    graphics_buffer_shift_x &= 0xfffffff2; //2bit buf

    //для div_factor 2
    uint max_width = graphics_buffer_width;
    if (graphics_buffer_shift_x < 0) {
        max_width += graphics_buffer_shift_x;
    }
    else {
#define div_factor (2)
        output_buffer_16bit += graphics_buffer_shift_x * 2 / div_factor;
    }


    int width = MIN((visible_line_size - ((graphics_buffer_shift_x > 0) ? (graphics_buffer_shift_x) : 0)), max_width);
    if (width < 0) return; // TODO: detect a case

    // Индекс палитры в зависимости от настроек чередования строк и кадров
    uint16_t* current_palette = palette[(y && is_flash_line) + (graphics_frame_count & is_flash_frame) & 1];

    //4bit buf
    register uint8_t* input_buffer = graphics_get_buffer() + y * (SCREEN_WIDTH / 2);
    lock_y = y;
    for (register int x = width / 2; x--;) {
        *output_buffer_16bit++ = current_palette[*input_buffer & 15];
        *output_buffer_16bit++ = current_palette[*input_buffer >> 4];
        input_buffer++;
    }
    lock_y = -1;
    dma_channel_set_read_addr(dma_chan_ctrl, output_buffer, false);
}

void graphics_set_mode() {
    if (_SM_VGA < 0) return; // если  VGA не инициализирована -

    // Если мы уже проиницилизированы - выходим
    if (txt_palette_fast && lines_pattern_data) {
        return;
    };
    uint8_t TMPL_VHS8 = 0;
    uint8_t TMPL_VS8 = 0;
    uint8_t TMPL_HS8 = 0;
    uint8_t TMPL_LINE8 = 0;

    int line_size;
    double fdiv = 100;
    int HS_SIZE = 4;
    int HS_SHIFT = 100;

    TMPL_LINE8 = 0b11000000;
    HS_SHIFT = 328 * 2;
    HS_SIZE = 48 * 2;

    line_size = 400 * 2;

    shift_picture = line_size - HS_SHIFT;

    palette16_mask = 0xc0c0;

    visible_line_size = 320;

    N_lines_total = 525;
    N_lines_visible = 480;
    line_VS_begin = 490;
    line_VS_end = 491;

    fdiv = clock_get_hz(clk_sys) / 25175000.0; //частота пиксельклока

    //корректировка  палитры по маске бит синхры
    bg_color[0] = bg_color[0] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;
    bg_color[1] = bg_color[1] & 0x3f3f3f3f | palette16_mask | palette16_mask << 16;
    for (int i = 0; i < 256; i++) {
        palette[0][i] = palette[0][i] & 0x3f3f | palette16_mask;
        palette[1][i] = palette[1][i] & 0x3f3f | palette16_mask;
    }

    //инициализация шаблонов строк и синхросигнала
    if (!lines_pattern_data) //выделение памяти, если не выделено
    {
        const uint32_t div32 = (uint32_t)(fdiv * (1 << 16) + 0.0);
        PIO_VGA->sm[_SM_VGA].clkdiv = div32 & 0xfffff000; //делитель для конкретной sm
        dma_channel_set_trans_count(dma_chan, line_size / 4, false);

        lines_pattern_data = (uint32_t *)calloc(line_size * 4 / 4, sizeof(uint32_t));

        for (int i = 0; i < 4; i++) {
            lines_pattern[i] = &lines_pattern_data[i * (line_size / 4)];
        }
        // memset(lines_pattern_data,N_TMPLS*1200,0);
        TMPL_VHS8 = TMPL_LINE8 ^ 0b11000000;
        TMPL_VS8 = TMPL_LINE8 ^ 0b10000000;
        TMPL_HS8 = TMPL_LINE8 ^ 0b01000000;

        uint8_t* base_ptr = (uint8_t *)lines_pattern[0];
        //пустая строка
        memset(base_ptr, TMPL_LINE8, line_size);
        //memset(base_ptr+HS_SHIFT,TMPL_HS8,HS_SIZE);
        //выровненная синхра вначале
        memset(base_ptr, TMPL_HS8, HS_SIZE);

        // кадровая синхра
        base_ptr = (uint8_t *)lines_pattern[1];
        memset(base_ptr, TMPL_VS8, line_size);
        //memset(base_ptr+HS_SHIFT,TMPL_VHS8,HS_SIZE);
        //выровненная синхра вначале
        memset(base_ptr, TMPL_VHS8, HS_SIZE);

        //заготовки для строк с изображением
        base_ptr = (uint8_t *)lines_pattern[2];
        memcpy(base_ptr, lines_pattern[0], line_size);
        base_ptr = (uint8_t *)lines_pattern[3];
        memcpy(base_ptr, lines_pattern[0], line_size);
    }
}

void graphics_set_res(const uint16_t width, const uint16_t height) {
    graphics_buffer_width = width;
    graphics_buffer_height = height;
}


void graphics_set_offset(const int x, const int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_flashmode(const bool flash_line, const bool flash_frame) {
    is_flash_frame = flash_frame;
    is_flash_line = flash_line;
}

void graphics_set_bgcolor(const uint32_t color888) {
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888 & 0xff) / 42;

    const uint8_t r = (color888 >> 16 & 0xff) / 42;
    const uint8_t g = (color888 >> 8 & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];
    bg_color[0] = ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask) << 16 |
                  ((c_hi << 8 | c_lo) & 0x3f3f | palette16_mask);
    bg_color[1] = ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask) << 16 |
                  ((c_lo << 8 | c_hi) & 0x3f3f | palette16_mask);
}

void graphics_set_palette(const uint8_t i, const uint32_t color888) {
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };

    const uint8_t b = (color888 & 0xff) / 42;

    const uint8_t r = (color888 >> 16 & 0xff) / 42;
    const uint8_t g = (color888 >> 8 & 0xff) / 42;

    const uint8_t c_hi = conv0[r] << 4 | conv0[g] << 2 | conv0[b];
    const uint8_t c_lo = conv1[r] << 4 | conv1[g] << 2 | conv1[b];

    palette[0][i] = (c_hi << 8 | c_lo) & 0x3f3f | palette16_mask;
    palette[1][i] = (c_lo << 8 | c_hi) & 0x3f3f | palette16_mask;
}

void graphics_init() {
    //инициализация палитры по умолчанию
#if 1
    const uint8_t conv0[] = { 0b00, 0b00, 0b01, 0b10, 0b10, 0b10, 0b11, 0b11 };
    const uint8_t conv1[] = { 0b00, 0b01, 0b01, 0b01, 0b10, 0b11, 0b11, 0b11 };
    for (int i = 0; i < 256; i++) {
        const uint8_t b = i & 0b11;
        const uint8_t r = i >> 5 & 0b111;
        const uint8_t g = i >> 2 & 0b111;

        const uint8_t c_hi = 0xc0 | conv0[r] << 4 | conv0[g] << 2 | b;
        const uint8_t c_lo = 0xc0 | conv1[r] << 4 | conv1[g] << 2 | b;

        palette[0][i] = c_hi << 8 | c_lo;
        palette[1][i] = c_lo << 8 | c_hi;
    }
#endif
    //текстовая палитра
    for (int i = 0; i < 16; i++) {
        const uint8_t b = i & 1 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t r = i & 4 ? (i >> 3 ? 3 : 2) : 0;
        const uint8_t g = i & 2 ? (i >> 3 ? 3 : 2) : 0;

        const uint8_t c = r << 4 | g << 2 | b;

        txt_palette[i] = c & 0x3f | 0xc0;
    }
    //инициализация PIO
    //загрузка программы в один из PIO
    const uint offset = pio_add_program(PIO_VGA, &pio_program_VGA);
    _SM_VGA = pio_claim_unused_sm(PIO_VGA, true);
    const uint sm = _SM_VGA;

    for (int i = 0; i < 8; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    }; //резервируем под выход PIO

    //pio_sm_config c = pio_vga_program_get_default_config(offset);

    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, 8, true); //конфигурация пинов на выход

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + (pio_program_VGA.length - 1));

    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX); //увеличение буфера TX за счёт RX до 8-ми
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, VGA_BASE_PIN, 8);
    pio_sm_init(PIO_VGA, sm, offset, &c);

    pio_sm_set_enabled(PIO_VGA, sm, true);

    //инициализация DMA
    dma_chan_ctrl = dma_claim_unused_channel(true);
    dma_chan = dma_claim_unused_channel(true);
    //основной ДМА канал для данных
    dma_channel_config c0 = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c0, DMA_SIZE_32);

    channel_config_set_read_increment(&c0, true);
    channel_config_set_write_increment(&c0, false);

    uint dreq = DREQ_PIO1_TX0 + sm;
    if (PIO_VGA == pio0) dreq = DREQ_PIO0_TX0 + sm;

    channel_config_set_dreq(&c0, dreq);
    channel_config_set_chain_to(&c0, dma_chan_ctrl); // chain to other channel

    dma_channel_configure(
        dma_chan,
        &c0,
        &PIO_VGA->txf[sm], // Write address
        lines_pattern[0], // read address
        600 / 4, //
        false // Don't start yet
    );
    //канал DMA для контроля основного канала
    dma_channel_config c1 = dma_channel_get_default_config(dma_chan_ctrl);
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);

    channel_config_set_read_increment(&c1, false);
    channel_config_set_write_increment(&c1, false);
    channel_config_set_chain_to(&c1, dma_chan); // chain to other channel
    //channel_config_set_dreq(&c1, DREQ_PIO0_TX0);

    dma_channel_configure(
        dma_chan_ctrl,
        &c1,
        &dma_hw->ch[dma_chan].read_addr, // Write address
        &lines_pattern[0], // read address
        1, //
        false // Don't start yet
    );
    //dma_channel_set_read_addr(dma_chan, &DMA_BUF_ADDR[0], false);

    graphics_set_mode();

    irq_set_exclusive_handler(VGA_DMA_IRQ, dma_handler_VGA);

    dma_channel_set_irq0_enabled(dma_chan_ctrl, true);

    irq_set_enabled(VGA_DMA_IRQ, true);
    dma_start_channel_mask(1u << dma_chan);
}

void __not_in_flash_func(adjust_clk)(void) {
    double fdiv = clock_get_hz(clk_sys) / 25175000.0; //частота пиксельклока
    const uint32_t div32 = (uint32_t)(fdiv * (1 << 16) + 0.0);
    PIO_VGA->sm[_SM_VGA].clkdiv = div32 & 0xfffff000; //делитель для конкретной sm
}

void graphics_restore_sync_colors(void) {}

void startVIDEO(uint8_t vol) {
    // Stub
}

bool hdmi_check_and_restart(void) { }
