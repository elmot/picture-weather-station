#include "cstdint"
#include "dithering.h"

extern slint::Rgb8Pixel * render_buffer;


//
// Created by elmot on 02/05/2026.
//
static inline int clamp_byte(const int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

void apply_floyd_steinberg_dithering(uint8_t* target_fb)
{

    for (int y = 0; y < m_size.height; y++) {
        // Yield every 20 rows to prevent watchdog
        if (y % 20 == 0) {
            dithering_periodic_call();
        }

        for (int x = 0; x < m_size.width; x++) {
            int idx = (y * m_size.width + x);

            // Get current pixel RGB
            int r = render_buffer[idx].r;
            int g = render_buffer[idx].g;
            int b = render_buffer[idx].b;

            // Find nearest palette color
            uint8_t pal_idx = rgb888_to_palette_idx(r, g, b);
            uint8_t epaper_color;
            int pal_r, pal_g, pal_b;


                epaper_color = palette[pal_idx].epd;
                pal_r = palette[pal_idx].r;
                pal_g = palette[pal_idx].g;
                pal_b = palette[pal_idx].b;

            // Set pixel in framebuffer
            set_epd_pixel(target_fb, m_size.width - x, m_size.height - y, m_size.width, epaper_color);

            // Calculate quantization error
            int err_r = r - pal_r;
            int err_g = g - pal_g;
            int err_b = b - pal_b;

            // Distribute error to neighbors (Floyd-Steinberg)
            // Right pixel: 7/16
            if (x + 1 < m_size.width) {
                int ni = idx + 1;
                render_buffer[ni].r = clamp_byte(render_buffer[ni].r + err_r * 7 / 16);
                render_buffer[ni].g = clamp_byte(render_buffer[ni].g + err_g * 7 / 16);
                render_buffer[ni].b = clamp_byte(render_buffer[ni].b + err_b * 7 / 16);
            }
            // Bottom-left pixel: 3/16
            if (y + 1 < m_size.height && x > 0) {
                int ni = (y + 1) * m_size.width + x - 1;
                render_buffer[ni].r = clamp_byte(render_buffer[ni].r + err_r * 3 / 16);
                render_buffer[ni].g = clamp_byte(render_buffer[ni].g + err_g * 3 / 16);
                render_buffer[ni].b = clamp_byte(render_buffer[ni].b + err_b * 3 / 16);
            }
            // Bottom pixel: 5/16
            if (y + 1 < m_size.height) {
                int ni = (y + 1) * m_size.width + x;
                render_buffer[ni].r = clamp_byte(render_buffer[ni].r + err_r * 5 / 16);
                render_buffer[ni].g = clamp_byte(render_buffer[ni].g + err_g * 5 / 16);
                render_buffer[ni].b = clamp_byte(render_buffer[ni].b + err_b * 5 / 16);
            }
            // Bottom-right pixel: 1/16
            if (y + 1 < m_size.height && x + 1 < m_size.width) {
                int ni = (y + 1) * m_size.width + x + 1;
                render_buffer[ni].r = clamp_byte(render_buffer[ni].r + err_r / 16);
                render_buffer[ni].g = clamp_byte(render_buffer[ni].g + err_g / 16);
                render_buffer[ni].b = clamp_byte(render_buffer[ni].b + err_b / 16);
            }
        }
    }

}
