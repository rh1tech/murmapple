#pragma once

#include "../src/board_config.h"

#define PIO_VGA (pio0)
#ifndef VGA_BASE_PIN
#define VGA_BASE_PIN HDMI_BASE_PIN
#endif
#define VGA_DMA_IRQ (DMA_IRQ_0)

#define RGB888(r, g, b) ( ((r) << 16) | ((g) << 8 ) | (b) )

#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)
