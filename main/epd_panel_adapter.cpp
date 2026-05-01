#include "epd_panel_adapter.h"
#include "esp_lcd_panel_interface.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "slint.h"
#include <cstring>

static const char *TAG = "epd_adapter";

/*-----------------------------------------------------------------------
 * 6-color palette: RGB888 values and e-paper color indices
 *---------------------------------------------------------------------*/
static const struct { uint8_t r, g, b, epd; } palette[] = {
    {0,   0,   0,   0x0}, // Black
    {255, 255, 255, 0x1}, // White
    {255, 255, 0,   0x2}, // Yellow
    {255, 0,   0,   0x3}, // Red
    {0,   0,   255, 0x5}, // Blue
    {0,   255, 0,   0x6}, // Green
};
#define PALETTE_SIZE (sizeof(palette) / sizeof(palette[0]))

/*-----------------------------------------------------------------------
 * Adapter context stored in panel->user_data
 *---------------------------------------------------------------------*/
struct epd_adapter_ctx {
    epd_handle_t epd;
    uint16_t     render_w;
    uint16_t     render_h;
    uint16_t     panel_w;
    bool         dirty;
    bool         refreshing;      // true while e-paper is physically updating
    int64_t      last_flush_us;   // timestamp of last flush start
};

/*-----------------------------------------------------------------------
 * RGB565 → nearest e-paper color (Euclidean distance, no dithering)
 *---------------------------------------------------------------------*/
static inline uint8_t rgb565_to_epd(uint16_t px)
{
    uint8_t r5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 = px & 0x1F;
    int r = (r5 << 3) | (r5 >> 2);
    int g = (g6 << 2) | (g6 >> 4);
    int b = (b5 << 3) | (b5 >> 2);

    uint32_t best_dist = UINT32_MAX;
    uint8_t  best = 0x1; // white
    for (int i = 0; i < (int)PALETTE_SIZE; i++) {
        int dr = r - palette[i].r;
        int dg = g - palette[i].g;
        int db = b - palette[i].b;
        uint32_t d = dr * dr + dg * dg + db * db;
        if (d < best_dist) { best_dist = d; best = palette[i].epd; }
    }
    return best;
}

/*-----------------------------------------------------------------------
 * Write one pixel into the 4-bit-packed e-paper framebuffer
 *---------------------------------------------------------------------*/
static inline void set_epd_pixel(uint8_t *fb, int x, int y, int w, uint8_t color)
{
    uint32_t idx = (uint32_t)y * w + x;
    uint32_t addr = idx / 2;
    if (idx % 2 == 0)
        fb[addr] = (fb[addr] & 0x0F) | (color << 4);
    else
        fb[addr] = (fb[addr] & 0xF0) | (color & 0x0F);
}

// Minimum microseconds between flush starts (60 s covers the refresh and cooldown)
#define EPD_FLUSH_COOLDOWN_US  (60LL * 1000 * 1000)

/*-----------------------------------------------------------------------
 * Flush the e-paper if enough time has passed since the last refresh.
 * Called from the Slint event-loop thread, so no extra sync needed.
 *---------------------------------------------------------------------*/
static void maybe_flush(epd_adapter_ctx *ctx)
{
    if (!ctx->dirty) return;

    int64_t now = esp_timer_get_time();
    if (ctx->refreshing && (now - ctx->last_flush_us < EPD_FLUSH_COOLDOWN_US))
        return;  // still cooling down from previous refresh

    ESP_LOGI(TAG, "Flushing e-paper (%dx%d on %d-wide panel)",
             ctx->render_w, ctx->render_h, ctx->panel_w);
    ctx->last_flush_us = now;
    ctx->refreshing    = true;
    ctx->dirty         = false;
    epd_flush_framebuffer(ctx->epd);
    ESP_LOGI(TAG, "Flush returned, asking slint loop to quit");

    // The flush above is synchronous — the panel has finished refreshing.
    // One-shot wake-render-sleep cycle: tell Slint to exit ui->run().
    slint::quit_event_loop();
}

/*-----------------------------------------------------------------------
 * esp_lcd_panel_t callbacks
 *---------------------------------------------------------------------*/
static esp_err_t adapter_draw_bitmap(esp_lcd_panel_t *panel,
                                     int x_start, int y_start,
                                     int x_end, int y_end,
                                     const void *color_data)
{
    auto *ctx = static_cast<epd_adapter_ctx *>(panel->user_data);
    uint8_t *fb = epd_get_framebuffer(ctx->epd);
    if (!fb) return ESP_ERR_INVALID_STATE;

    const uint16_t *src = static_cast<const uint16_t *>(color_data);
    for (int y = y_start; y < y_end; y++) {
        for (int x = x_start; x < x_end; x++) {
            set_epd_pixel(fb, x, y, ctx->panel_w, rgb565_to_epd(*src++));
        }
    }

    ctx->dirty = true;

    // If this is the last line of the render area, try to flush now
    if (y_end >= ctx->render_h) {
        maybe_flush(ctx);
    }
    return ESP_OK;
}

static esp_err_t adapter_noop(esp_lcd_panel_t *) { return ESP_OK; }
static esp_err_t adapter_noop_bb(esp_lcd_panel_t *, bool, bool) { return ESP_OK; }
static esp_err_t adapter_noop_b(esp_lcd_panel_t *, bool) { return ESP_OK; }
static esp_err_t adapter_noop_ii(esp_lcd_panel_t *, int, int) { return ESP_OK; }

/*-----------------------------------------------------------------------
 * Public: create the adapter panel
 *---------------------------------------------------------------------*/
esp_lcd_panel_handle_t epd_panel_adapter_create(epd_handle_t epd,
                                                uint16_t render_w,
                                                uint16_t render_h)
{
    epd_panel_info_t info;
    epd_get_info(epd, &info);

    // Clear framebuffer to white (0x11 = two white pixels packed)
    uint8_t *fb = epd_get_framebuffer(epd);
    if (fb) memset(fb, 0x11, info.buffer_size);

    auto *ctx = new epd_adapter_ctx{};
    ctx->epd      = epd;
    ctx->render_w = render_w;
    ctx->render_h = render_h;
    ctx->panel_w  = info.width;
    ctx->dirty       = false;
    ctx->refreshing  = false;
    ctx->last_flush_us = 0;

    auto *panel = new esp_lcd_panel_t{};
    panel->reset       = adapter_noop;
    panel->init        = adapter_noop;
    panel->del         = adapter_noop;
    panel->draw_bitmap = adapter_draw_bitmap;
    panel->mirror      = adapter_noop_bb;
    panel->swap_xy     = adapter_noop_b;
    panel->set_gap     = adapter_noop_ii;
    panel->invert_color = adapter_noop_b;
    panel->disp_on_off = adapter_noop_b;
    panel->disp_sleep  = adapter_noop_b;
    panel->user_data   = ctx;

    ESP_LOGI(TAG, "E-paper adapter created: render %dx%d on %dx%d panel",
             render_w, render_h, info.width, info.height);
    return panel;
}
