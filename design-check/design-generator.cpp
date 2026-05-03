// Renders the WeatherStation Slint window with fake data, runs the same
// Floyd-Steinberg dithering used by the firmware, and writes the result as a
// paletted (8-bit indexed) BMP using the firmware's 6-color EPD palette.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <slint-platform.h>
#include "dithering.h"
#include "weather-station.h"

extern void apply_floyd_steinberg_dithering(uint8_t *target_fb);

// dithering.cpp expects these as globals.
slint::Rgb8Pixel *render_buffer = nullptr;

const palette_item_t palette[] = {
    {  0,   0,   0, 0x0}, // 0 Black
    {255, 255, 255, 0x1}, // 1 White
    {255, 255,   0, 0x2}, // 2 Yellow
    {255,   0,   0, 0x3}, // 3 Red
    {  0,   0, 255, 0x5}, // 4 Blue
    {  0, 255,   0, 0x6}, // 5 Green
};

uint8_t rgb888_to_palette_idx(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t best_dist = UINT32_MAX;
    uint8_t best = 1;
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

void set_epd_pixel(uint8_t *fb, const int x, const int y, const int w, const uint8_t color) {
    const uint32_t idx = static_cast<uint32_t>(y) * w + x;
    const uint32_t addr = idx / 2;
    if (idx % 2 == 0)
        fb[addr] = (fb[addr] & 0x0F) | static_cast<uint8_t>(color << 4);
    else
        fb[addr] = (fb[addr] & 0xF0) | (color & 0x0F);
}

void dithering_periodic_call() {}

namespace {

slint::platform::SoftwareRenderer g_renderer{
    slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};

class HeadlessWindowAdapter : public slint::platform::WindowAdapter {
public:
    slint::PhysicalSize size() override { return m_size; }
    slint::platform::AbstractRenderer &renderer() override { return g_renderer; }
};

class HeadlessPlatform : public slint::platform::Platform {
public:
    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
        return std::make_unique<HeadlessWindowAdapter>();
    }
};

// ---------------------------------------------------------------------------
// Chart support — replicates the production ChartSupportCode behavior so that
// `Chart` elements in the Slint design render meaningful content.
// ---------------------------------------------------------------------------

void draw_line(slint::Rgba8Pixel *buf, int w, int h, int x0, int y0, int x1, int y1,
               const slint::Color &color, int thickness) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const slint::Rgba8Pixel pixel{color.red(), color.green(), color.blue(), color.alpha()};

    for (;;) {
        const int r = thickness <= 1 ? 0 : thickness / 2;
        for (int dy2 = -r; dy2 <= r; ++dy2) {
            for (int dx2 = -r; dx2 <= r; ++dx2) {
                const int nx = x0 + dx2;
                const int ny = y0 + dy2;
                if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                    buf[ny * w + nx] = pixel;
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

slint::Image render_chart(int w, int h, const std::shared_ptr<slint::Model<float>> &data,
                          float minY, float maxY, const slint::Color &lineColor,
                          const slint::Color &gridColor, int gridRows, int autoGridRows,
                          int gridCols) {
    if (!data || w <= 0 || h <= 0) return {};
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pxBuf(w, h);
    auto *px = pxBuf.begin();
    std::memset(px, 0, static_cast<size_t>(w) * h * sizeof(slint::Rgba8Pixel));

    const slint::Rgba8Pixel gridPixel{gridColor.red(), gridColor.green(), gridColor.blue(),
                                      gridColor.alpha()};

    if (gridRows < 0) gridRows = autoGridRows;
    if (gridRows > 0) {
        for (int i = 1; i < gridRows; ++i) {
            const int y = h - 1 - (i * (h - 1) / gridRows);
            for (int x = 0; x < w; x += 3) px[x + y * w] = gridPixel;
        }
    }
    if (gridCols > 0) {
        for (int i = 1; i < gridCols; ++i) {
            const int x = i * (w - 1) / gridCols;
            for (int y = 0; y < h; y += 3) px[x + y * w] = gridPixel;
        }
    }

    const auto count = static_cast<int>(data->row_count());
    if (count < 2) return {pxBuf};

    float range = maxY - minY;
    if (range < 0.0001f) range = 0.0001f;

    int prev_x = -1, prev_y = -1;
    for (int i = 0; i < count; ++i) {
        const float value = data->row_data(i).value_or(NAN);
        if (!std::isfinite(value)) { prev_x = -1; continue; }
        const int x = i * (w - 1) / (count - 1);
        int y = h - 1 - static_cast<int>((value - minY) / range * static_cast<float>(h - 1));
        y = std::clamp(y, 0, h - 1);
        if (prev_x >= 0) draw_line(px, w, h, prev_x, prev_y, x, y, lineColor, 2);
        prev_x = x;
        prev_y = y;
    }
    return {pxBuf};
}

std::shared_ptr<slint::Model<float>> calc_bounds(const std::shared_ptr<slint::Model<float>> &data) {
    auto result = std::make_shared<slint::VectorModel<float>>();
    if (!data || data->row_count() == 0) {
        result->push_back(0.0f);
        result->push_back(10.0f);
        result->push_back(5.0f);
        return result;
    }
    float minVal = INFINITY, maxVal = -INFINITY;
    const auto count = data->row_count();
    for (unsigned i = 0; i < count; ++i) {
        const float v = data->row_data(i).value_or(NAN);
        if (!std::isfinite(v)) continue;
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
    }
    if (!std::isfinite(minVal)) minVal = 0.0f;
    if (!std::isfinite(maxVal)) maxVal = 1.0f;
    float range = maxVal - minVal;
    if (range < 0.0001f) range = 0.0001f;
    maxVal += range * 0.051f;
    minVal -= range * 0.051f;
    range = maxVal - minVal;
    const float magnitude = std::pow(10.0f, std::floor(std::log10(range)));
    const float normalized = range / magnitude;
    float granularity;
    if (normalized <= 2.0f)      granularity = magnitude * 0.5f;
    else if (normalized <= 5.0f) granularity = magnitude;
    else                         granularity = magnitude * 2.0f;
    const float boundMin = std::floor(minVal / granularity) * granularity;
    const float boundMax = std::ceil(maxVal / granularity) * granularity;
    result->push_back(boundMin);
    result->push_back(boundMax);
    result->push_back((boundMax - boundMin) / granularity);
    return result;
}

// ---------------------------------------------------------------------------
// Fake data setup
// ---------------------------------------------------------------------------

void populate_fake_data(const slint::ComponentHandle<WeatherStation> &ui) {
    ui->set_weather_code(3);
    ui->set_day(true);

    ui->set_indoor_data(LocalData{.tempC = 21.4f, .relHumidity = 47.0f, .tempHistory = {}});

    ui->set_adafruit_data(AdaIoData{
        .value = 612.0f, .humidity = 49.0f, .temperature = 22.3f, .connFail = false});

    ui->set_ruuvi_data(RuuviData{
        .tempC = 4.6f, .atmPHgmm = 758.0f, .relHumidity = 81.0f,
        .connFail = false, .battV = 2.95f});

    ui->set_meteo_data(OpenMeteoData{
        .tempC = 7.1f, .tempFeelsC = 4.5f, .windSpeed = 3.7f, .windGusts = 6.2f,
        .windDir = slint::SharedString("NW"),
        .condition = FoxConditionEnum::Cloudy, .connFail = false});

    std::vector<float> co2;
    co2.reserve(96);
    for (int i = 0; i < 96; ++i) {
        const float t = static_cast<float>(i) / 96.0f;
        co2.push_back(450.0f + 250.0f * std::sin(t * 6.28f * 2.0f) +
                      80.0f * std::sin(t * 6.28f * 7.0f));
    }
    ui->set_co2_history(std::make_shared<slint::VectorModel<float>>(co2));

    std::vector<float> pressure;
    pressure.reserve(96);
    for (int i = 0; i < 96; ++i) {
        const float t = static_cast<float>(i) / 96.0f;
        pressure.push_back(755.0f + 6.0f * std::sin(t * 6.28f) +
                           1.5f * std::cos(t * 6.28f * 4.0f));
    }
    ui->set_pressure_history(std::make_shared<slint::VectorModel<float>>(pressure));

    ui->set_charging(false);
    ui->set_charge(0.78f);
    ui->set_last_update(slint::SharedString("2026-05-02 14:30"));
}

// ---------------------------------------------------------------------------
// Paletted BMP writer — 8-bit indexed, bottom-up.
// ---------------------------------------------------------------------------

bool write_indexed_bmp(const std::string &path, const std::vector<uint8_t> &indexes,
                       uint32_t width, uint32_t height) {
    constexpr uint32_t color_table_entries = 256;
    constexpr uint32_t color_table_bytes = color_table_entries * 4;
    const uint32_t row_stride = (width + 3u) & ~3u;          // 4-byte aligned
    const uint32_t pixel_data_size = row_stride * height;
    constexpr uint32_t file_header_size = 14;
    constexpr uint32_t info_header_size = 40;
    constexpr uint32_t pixel_data_offset =
        file_header_size + info_header_size + color_table_bytes;
    const uint32_t file_size = pixel_data_offset + pixel_data_size;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    auto put_u16 = [&](uint16_t v) {
        const uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)};
        out.write(reinterpret_cast<const char *>(b), 2);
    };
    auto put_u32 = [&](uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF),
                              static_cast<uint8_t>((v >> 8) & 0xFF),
                              static_cast<uint8_t>((v >> 16) & 0xFF),
                              static_cast<uint8_t>((v >> 24) & 0xFF)};
        out.write(reinterpret_cast<const char *>(b), 4);
    };

    // BITMAPFILEHEADER
    out.put('B'); out.put('M');
    put_u32(file_size);
    put_u16(0); put_u16(0);
    put_u32(pixel_data_offset);

    // BITMAPINFOHEADER
    put_u32(info_header_size);
    put_u32(width);
    put_u32(height);   // positive => bottom-up
    put_u16(1);        // planes
    put_u16(8);        // bpp
    put_u32(0);        // BI_RGB
    put_u32(pixel_data_size);
    put_u32(2835); put_u32(2835); // 72 DPI
    put_u32(color_table_entries);
    put_u32(color_table_entries);

    // Color table — BGRA per entry. The first std::size(palette) entries map
    // to the EPD palette; the remainder is zeroed (unused).
    std::vector<uint8_t> color_table(color_table_bytes, 0);
    for (size_t i = 0; i < std::size(palette); ++i) {
        color_table[i * 4 + 0] = palette[i].b;
        color_table[i * 4 + 1] = palette[i].g;
        color_table[i * 4 + 2] = palette[i].r;
        color_table[i * 4 + 3] = 0;
    }
    out.write(reinterpret_cast<const char *>(color_table.data()), color_table_bytes);

    // Pixel data — bottom-up, row-padded.
    std::vector<uint8_t> row(row_stride, 0);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        std::memcpy(row.data(), &indexes[static_cast<size_t>(y) * width], width);
        out.write(reinterpret_cast<const char *>(row.data()), row_stride);
    }
    return out.good();
}

}  // namespace

int main(int argc, char **argv) {
    const std::string out_path = argc > 1 ? argv[1] : "weather-station.bmp";

    slint::platform::set_platform(std::make_unique<HeadlessPlatform>());

    auto ui = WeatherStation::create();
    ui->global<ChartSupport>().on_renderChart(render_chart);
    ui->global<ChartSupport>().on_calcLimits(calc_bounds);
    populate_fake_data(ui);
    ui->show();

    slint::platform::update_timers_and_animations();

    const size_t pixel_count = static_cast<size_t>(m_size.width) * m_size.height;
    std::vector<slint::Rgb8Pixel> render_storage(pixel_count);
    render_buffer = render_storage.data();
    g_renderer.render(std::span<slint::Rgb8Pixel>(render_buffer, pixel_count), m_size.width);

    ui->hide();

    // Floyd-Steinberg dithering modifies render_buffer in place via error
    // diffusion. The scratch fb mirrors the EPD layout (4 bits per pixel,
    // ~192000 bytes); we don't read from it — we re-quantize render_buffer
    // afterwards, which yields the same palette indices the dithering chose.
    // The dithering's `m_size.{width,height} - {x,y}` transform reaches
    // dest=(800, 480), one past each axis, so we oversize the buffer to keep
    // those writes inside our allocation.
    std::vector<uint8_t> epd_fb(((m_size.height + 2) * m_size.width) / 2, 0);
    apply_floyd_steinberg_dithering(epd_fb.data());

    std::vector<uint8_t> indexes(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
        const auto &px = render_buffer[i];
        indexes[i] = rgb888_to_palette_idx(px.r, px.g, px.b);
    }

    if (!write_indexed_bmp(out_path, indexes, m_size.width, m_size.height)) {
        std::fprintf(stderr, "Failed to write %s\n", out_path.c_str());
        return 1;
    }
    std::printf("Wrote %s (%ux%u, 8-bit indexed)\n", out_path.c_str(),
                static_cast<unsigned>(m_size.width),
                static_cast<unsigned>(m_size.height));
    return 0;
}
