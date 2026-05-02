#pragma once

#include "slint-platform.h"
typedef  struct { uint8_t r, g, b, epd; } palette_item_t;
extern const palette_item_t palette[];

constexpr auto m_size = slint::PhysicalSize({800, 480});

extern void set_epd_pixel(uint8_t *fb, const int x,const  int y,const  int w,const  uint8_t color);
extern uint8_t rgb888_to_palette_idx(uint8_t r, uint8_t g, uint8_t b);

extern void dithering_periodic_call();