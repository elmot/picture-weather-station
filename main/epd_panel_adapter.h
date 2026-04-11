#pragma once

#include "esp_lcd_types.h"
#include "epaper.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create an esp_lcd_panel_handle_t that wraps an e-paper display.
 * Slint (or any esp_lcd consumer) calls draw_bitmap with RGB565 data;
 * the adapter converts to the nearest 6-color e-paper pixel and writes
 * into the e-paper framebuffer.  A debounced timer triggers the actual
 * e-paper refresh a few seconds after the last draw.
 *
 * @param epd       Initialised e-paper handle (from epd_init)
 * @param render_w  Logical render width  (e.g. 320)
 * @param render_h  Logical render height (e.g. 240)
 */
esp_lcd_panel_handle_t epd_panel_adapter_create(epd_handle_t epd,
                                                uint16_t render_w,
                                                uint16_t render_h);

#ifdef __cplusplus
}
#endif
