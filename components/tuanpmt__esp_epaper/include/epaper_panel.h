#ifndef _EPAPER_PANEL_H_
#define _EPAPER_PANEL_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "epaper_config.h"

// Forward declaration
typedef struct epd_device epd_device_t;

/*=============================================================================
 * Capability Flags
 *============================================================================*/
#define EPD_CAP_PARTIAL     (1 << 0)  // Supports partial refresh
#define EPD_CAP_FAST        (1 << 1)  // Supports fast refresh
#define EPD_CAP_GRAYSCALE   (1 << 2)  // Supports grayscale mode
#define EPD_CAP_BUSY_INV    (1 << 3)  // Inverted busy signal (HIGH=ready)


/*=============================================================================
 * Panel Descriptor (Data-Driven)
 *============================================================================*/
typedef struct {
    const char *name;               // Panel name for logging
    uint16_t width;                 // Default width in pixels
    uint16_t height;                // Default height in pixels
    epd_color_mode_t color_mode;    // Color mode
    uint8_t bits_per_pixel;         // 1, 2, or 4
    uint32_t caps;                  // Capability flags (EPD_CAP_*)
    const void *init_data;          // Panel-specific init data (LUT, etc.)
} epd_panel_desc_t;

/*=============================================================================
 * Registry Functions
 *============================================================================*/

// Get panel descriptor by type
const epd_panel_desc_t* epd_get_panel_desc();

// Check if panel supports a capability
static inline bool epd_panel_has_cap(const epd_panel_desc_t *panel, uint32_t cap) {
    return panel && (panel->caps & cap);
}

// Calculate buffer size for panel
static inline uint32_t epd_calc_buffer_size(uint16_t w, uint16_t h, uint8_t bpp) {
    return ((uint32_t)w * h * bpp + 7) / 8;
}

#endif // _EPAPER_PANEL_H_
