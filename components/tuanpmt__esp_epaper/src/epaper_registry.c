/**
 * @file epaper_registry.c
 * @brief Data-driven panel registry and controller lookup
 *
 * To add a new panel:
 * 1. Add enum in epaper_config.h
 * 2. Add entry in panel_registry[] below
 *
 * To add a new controller:
 * 1. Add enum in epaper_panel.h (epd_controller_type_t)
 * 2. Implement controller in src/controllers/
 * 3. Add entry in controller_ops[] below
 */

#include "epaper_panel.h"
#include "epaper_config.h"

/*=============================================================================
 * Controller Operations (forward declarations)
 *============================================================================*/

// ACeP 6-color controller
extern esp_err_t acep6c_init(epd_device_t* dev);
extern esp_err_t acep6c_update(epd_device_t* dev, epd_update_mode_t mode);
extern esp_err_t acep6c_write_ram(epd_device_t* dev, const uint8_t* data, uint32_t len);
extern esp_err_t acep6c_sleep(epd_device_t* dev);
extern esp_err_t acep6c_wake(epd_device_t* dev);

/*=============================================================================
 * Panel Registry Table
 *
 * Format: { name, width, height, color_mode, bpp, caps, controller, init_data }
 *============================================================================*/


/*=============================================================================
 * Registry Lookup Functions
 *============================================================================*/

const epd_panel_desc_t* epd_get_panel_desc()
{
    static const epd_panel_desc_t panel_registry = {

        .name = "GDEP073E01",
        .width = 800, .height = 480,
        .color_mode = EPD_COLOR_6COLOR, .bits_per_pixel = 4,
        .caps = EPD_CAP_BUSY_INV, // Inverted busy, no partial/fast
        .init_data = NULL,
    };
    return &panel_registry;
}
