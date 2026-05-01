#ifndef _EPAPER_H_
#define _EPAPER_H_

#include <stdint.h>
#include <stdbool.h>
#include "epaper_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle
typedef struct epd_device* epd_handle_t;

// Panel info (read-only after init)
typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t buffer_size;       // In bytes
    epd_color_mode_t color_mode;
} epd_panel_info_t;

/**
 * @brief Initialize e-paper display
 * @param config Device configuration
 * @param handle Output handle
 * @return ESP_OK on success
 */
esp_err_t epd_init(const epd_config_t *config, epd_handle_t *handle);

/**
 * @brief Deinitialize e-paper display
 */
esp_err_t epd_deinit(epd_handle_t handle);

/**
 * @brief Get panel information
 */
esp_err_t epd_get_info(epd_handle_t handle, epd_panel_info_t *info);

/**
 * @brief Clear display (full white)
 */
esp_err_t epd_clear(epd_handle_t handle);

/**
 * @brief Fill display with color (0x00=black, 0xFF=white)
 */
esp_err_t epd_fill(epd_handle_t handle, uint8_t color);

/**
 * @brief Update full screen with buffer
 * @param buffer Pixel data (1 bit per pixel, MSB first)
 */
esp_err_t epd_update(epd_handle_t handle, const uint8_t *buffer);

/**
 * @brief Enter deep sleep mode
 */
esp_err_t epd_sleep(epd_handle_t handle);

/**
 * @brief Wake from deep sleep
 */
esp_err_t epd_wake(epd_handle_t handle);

/**
 * @brief Check if display is busy
 */
bool epd_is_busy(epd_handle_t handle);

/**
 * @brief Wait until display is ready
 * @param timeout_ms Timeout in milliseconds (0 = infinite)
 */
esp_err_t epd_wait_busy(epd_handle_t handle, uint32_t timeout_ms);

/**
 * @brief Get framebuffer for direct rendering
 * @return Pointer to internal framebuffer
 */
uint8_t* epd_get_framebuffer(epd_handle_t handle);

/**
 * @brief Flush framebuffer to display
 */
esp_err_t epd_flush_framebuffer(epd_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // _EPAPER_H_
