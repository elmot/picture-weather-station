#include "epd_platform.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include <memory>
#include <vector>
#include "freertos/FreeRTOS.h"

#include "hw_support.h"

static const char *TAG = "epd_platform";
#define DITHERING 1

constexpr auto m_size = slint::PhysicalSize({LCD_H_RES, LCD_V_RES});

static slint::Rgb8Pixel * render_buffer = nullptr;

/*-----------------------------------------------------------------------
 * Override the e-paper component's __weak idle hook. The default does
 * vTaskDelay(5ms) inside the busy-pin polling loop. The 6-color panel
 * stays busy for ~30 s per refresh — light-sleep instead, so the SoC
 * draws ~µA between polls. Wakeup source is the timer; the panel's
 * BUSY pin transition is detected on the next poll.
 *---------------------------------------------------------------------*/
extern "C" void epd_idle(uint32_t /*ms*/)
{
    esp_sleep_enable_timer_wakeup(300ULL * 1000ULL);  /* 300 ms */
    esp_light_sleep_start();
}

/*-----------------------------------------------------------------------
 * 6-color palette
 *---------------------------------------------------------------------*/
static const struct { uint8_t r, g, b, epd; } palette[] = {
    {0,   0,   0,   0x0}, // Black
    {255, 255, 255, 0x1}, // White
    {255, 255, 0,   0x2}, // Yellow
    {255, 0,   0,   0x3}, // Red
    {0,   0,   255, 0x5}, // Blue
    {0,   255, 0,   0x6}, // Green
};

static inline uint8_t rgb888_to_palette_idx(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t best_dist = UINT32_MAX;
    uint8_t  best = 1;
    for (uint8_t i = 0; i < std::size(palette); ++i) {
        const auto &p = palette[i];
        const int dr = static_cast<int>(r) - p.r;
        const int dg = static_cast<int>(g) - p.g;
        const int db = static_cast<int>(b) - p.b;
        const uint32_t d = dr * dr + dg * dg + db * db;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}

static void set_epd_pixel(uint8_t *fb, const int x,const  int y,const  int w,const  uint8_t color)
{
    const uint32_t idx = static_cast<uint32_t>(y) * w + x;
    const uint32_t addr = idx / 2;
    if (idx % 2 == 0)
        fb[addr] = (fb[addr] & 0x0F) | static_cast<uint8_t>(color << 4);
    else
        fb[addr] = (fb[addr] & 0xF0) | (color & 0x0F);
}

auto m_renderer = slint::platform::SoftwareRenderer(slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer);

/*-----------------------------------------------------------------------
 * Minimal WindowAdapter — a SoftwareRenderer and a fixed size, nothing else.
 *---------------------------------------------------------------------*/
class EpdWindowAdapter : public slint::platform::WindowAdapter
{
public:
    explicit EpdWindowAdapter() {}

    slint::PhysicalSize size() override { return m_size; }
    slint::platform::AbstractRenderer &renderer() override { return m_renderer; }

};

/*-----------------------------------------------------------------------
 * Minimal Platform — provides a window adapter and a clock. No event
 * loop, no run_in_event_loop, nothing else is needed for a one-shot render.
 *---------------------------------------------------------------------*/
class EpdPlatform : public slint::platform::Platform
{
public:
    explicit EpdPlatform() {}

    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override
    {
        return  std::make_unique<EpdWindowAdapter>();
    }

    /* One-shot render: pin the clock far in the future so any `animate`
     * transitions in .slint files settle to their end state. We don't run
     * an event loop, so timers never need to advance. */
    std::chrono::milliseconds duration_since_start() override
    {
        return std::chrono::milliseconds(1'000'000'000);
    }

};

/*-----------------------------------------------------------------------
 * Module-level state
 *---------------------------------------------------------------------*/

void epd_platform_init()
{
    slint::platform::set_platform(std::make_unique<EpdPlatform>());
    ESP_LOGI(TAG, "EpdPlatform installed (%dx%d)", int(m_size.width), int(m_size.height));

    render_buffer = static_cast<slint::Rgb8Pixel*>(heap_caps_malloc(m_size.height * m_size.width * sizeof(slint::Rgb8Pixel), MALLOC_CAP_SPIRAM));
    if (!render_buffer) {
        ESP_LOGE(TAG, "Failed to allocate dithering buffer");
        return;
    }
}
static void apply_floyd_steinberg_dithering();

void epd_platform_render()
{
    epd_panel_info_t info;
    epd_get_info(s_epd, &info);
    uint8_t *fb = epd_get_framebuffer(s_epd);
    if (!fb) { ESP_LOGE(TAG, "no framebuffer"); return; }

    const int panel_w = info.width;

    /* Single line scratch buffer in DRAM. */
    std::vector<slint::platform::Rgb565Pixel> line_buf(panel_w);

    ESP_LOGI(TAG, "Rendering %dx%d into e-paper framebuffer", panel_w, info.height);

    /* Drain deferred property-change handlers (e.g., Chart's `changed data =>`
     * that computes minY/maxY) so the renderer sees an up-to-date state. */
    slint::platform::update_timers_and_animations();

    std::span rgb8_pixels(render_buffer, m_size.height * m_size.width);
    // ReSharper disable once CppExpressionWithoutSideEffects
    m_renderer.render(rgb8_pixels,m_size.width);
#ifdef DITHERING
    apply_floyd_steinberg_dithering();
#else
    for (int y = 0; y < m_size.height; ++y)
    {
        for (int x = 0; x < m_size.width; ++x)
        {
            const auto& px = rgb8_pixels[y * m_size.width + x];
            set_epd_pixel(fb, x, y, panel_w, palette[rgb888_to_palette_idx(px.r, px.g, px.b)].epd);
        }
    }
#endif

    ESP_LOGI(TAG, "Flushing e-paper");
    epd_flush_framebuffer(s_epd);
    ESP_LOGI(TAG, "Flush done");
}


static inline int clamp_byte(const int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void apply_floyd_steinberg_dithering()
{
    uint8_t* _fb = epd_get_framebuffer(s_epd);
    ESP_LOGI(TAG, "Applying Floyd-Steinberg dithering...");

    for (int y = 0; y < m_size.height; y++) {
        // Yield every 20 rows to prevent watchdog
        if (y % 20 == 0) {
            vTaskDelay(1);
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
            set_epd_pixel(_fb, x, y, m_size.width, epaper_color);

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

    ESP_LOGI(TAG, "Dithering complete");
}

