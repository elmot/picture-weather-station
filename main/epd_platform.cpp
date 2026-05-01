#include "epd_platform.h"
#include "esp_log.h"
#include <memory>
#include <vector>

static const char *TAG = "epd_platform";

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

static inline uint8_t rgb888_to_epd(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t best_dist = UINT32_MAX;
    uint8_t  best = 0x1;
    for (auto &p : palette) {
        int dr = int(r) - p.r;
        int dg = int(g) - p.g;
        int db = int(b) - p.b;
        uint32_t d = dr * dr + dg * dg + db * db;
        if (d < best_dist) { best_dist = d; best = p.epd; }
    }
    return best;
}

static inline void set_epd_pixel(uint8_t *fb, int x, int y, int w, uint8_t color)
{
    uint32_t idx = uint32_t(y) * w + x;
    uint32_t addr = idx / 2;
    if (idx % 2 == 0)
        fb[addr] = (fb[addr] & 0x0F) | uint8_t(color << 4);
    else
        fb[addr] = (fb[addr] & 0xF0) | (color & 0x0F);
}

/*-----------------------------------------------------------------------
 * Minimal WindowAdapter — a SoftwareRenderer and a fixed size, nothing else.
 *---------------------------------------------------------------------*/
class EpdWindowAdapter : public slint::platform::WindowAdapter
{
public:
    explicit EpdWindowAdapter(slint::PhysicalSize size)
        : m_renderer(slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer),
          m_size(size) {}

    slint::PhysicalSize size() override { return m_size; }
    slint::platform::AbstractRenderer &renderer() override { return m_renderer; }

    slint::platform::SoftwareRenderer m_renderer;
    slint::PhysicalSize               m_size;
};

/*-----------------------------------------------------------------------
 * Minimal Platform — provides a window adapter and a clock. No event
 * loop, no run_in_event_loop, nothing else needed for a one-shot render.
 *---------------------------------------------------------------------*/
class EpdPlatform : public slint::platform::Platform
{
public:
    explicit EpdPlatform(slint::PhysicalSize size) : m_size(size) {}

    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override
    {
        auto window = std::make_unique<EpdWindowAdapter>(m_size);
        m_window = window.get();
        return window;
    }

    /* One-shot render: pin the clock far in the future so any `animate`
     * transitions in .slint files settle to their end state. We don't run
     * an event loop, so timers never need to advance. */
    std::chrono::milliseconds duration_since_start() override
    {
        return std::chrono::milliseconds(1'000'000'000);
    }

    EpdWindowAdapter *m_window = nullptr;

private:
    slint::PhysicalSize m_size;
};

/*-----------------------------------------------------------------------
 * Module-level state
 *---------------------------------------------------------------------*/
static EpdPlatform *s_platform = nullptr;
static epd_handle_t s_epd      = nullptr;

void epd_platform_init(epd_handle_t epd, slint::PhysicalSize size)
{
    s_epd = epd;
    auto platform = std::make_unique<EpdPlatform>(size);
    s_platform = platform.get();
    slint::platform::set_platform(std::move(platform));
    ESP_LOGI(TAG, "EpdPlatform installed (%dx%d)", int(size.width), int(size.height));
}

void epd_platform_render(void)
{
    if (!s_platform || !s_platform->m_window) {
        ESP_LOGE(TAG, "render called before window adapter exists");
        return;
    }

    epd_panel_info_t info;
    epd_get_info(s_epd, &info);
    uint8_t *fb = epd_get_framebuffer(s_epd);
    if (!fb) { ESP_LOGE(TAG, "no framebuffer"); return; }

    const int panel_w = info.width;

    /* Single line scratch buffer in DRAM. */
    std::vector<slint::platform::Rgb565Pixel> line_buf(panel_w);

    ESP_LOGI(TAG, "Rendering %dx%d into e-paper framebuffer", panel_w, info.height);

    /* Drain deferred property-change handlers (e.g. Chart's `changed data =>`
     * that computes minY/maxY) so the renderer sees up-to-date state. */
    slint::platform::update_timers_and_animations();

    s_platform->m_window->m_renderer.render_by_line<slint::platform::Rgb565Pixel>(
            [&](std::size_t line_y, std::size_t line_start, std::size_t line_end,
                auto &&render_fn) {
                std::span<slint::platform::Rgb565Pixel> view {
                        line_buf.data(), line_end - line_start };
                render_fn(view);
                for (std::size_t i = 0; i < line_end - line_start; ++i) {
                    const auto &px = line_buf[i];
                    /* Expand 5/6/5 to 8 bits. */
                    uint8_t r = uint8_t((px.r << 3) | (px.r >> 2));
                    uint8_t g = uint8_t((px.g << 2) | (px.g >> 4));
                    uint8_t b = uint8_t((px.b << 3) | (px.b >> 2));
                    set_epd_pixel(fb, int(line_start + i), int(line_y),
                                  panel_w, rgb888_to_epd(r, g, b));
                }
            });

    ESP_LOGI(TAG, "Flushing e-paper");
    epd_flush_framebuffer(s_epd);
    ESP_LOGI(TAG, "Flush done");
}
