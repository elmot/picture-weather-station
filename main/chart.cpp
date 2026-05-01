#include <cmath>
#include <cstring>

#include "chart.h"

/*-----------------------------------------------------------------------
 * Simple chart renderer — draws a value history into an image
 *---------------------------------------------------------------------*/

static void draw_line(slint::Rgba8Pixel* buf, const int w, const int h,
                      int x0, int y0, const int x1, const int y1, const slint::Color& color)
{
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;)
    {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
            buf[y0 * w + x0] = slint::Rgba8Pixel(color.red(), color.green(), color.blue(), color.alpha());
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

slint::Image ChartSupportCodeBase::render_chart(const int w, const int h,
                                                const std::shared_ptr<slint::Model<float>>& data,
                                                float minY, float maxY,
                                                const slint::Color& lineColor, const slint::Color& gridColor,
                                                int gridRows, const int autoGridRows,
                                                const int gridCols)
{
    if (data == nullptr) return {};
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pxBuf(w, h);
    auto* px = pxBuf.begin();

    /* Clear to transparent */
    memset(px, 0, w * h * sizeof(slint::Rgba8Pixel));

    /* Draw horizontal grid lines */
    const slint::Rgba8Pixel gridPixel{gridColor.red(), gridColor.green(), gridColor.blue(), gridColor.alpha()};

    if (gridRows <0 ) gridRows = autoGridRows;
    if (gridRows > 0)
    {
        for (int i = 1; i < gridRows; i++)
        {
            const int y = h - 1 - (i * (h - 1) / gridRows);
            for (int x = 0; x < w; x += 3)
            {
                px[x + y * w] = gridPixel;
            }
        }
    }

    /* Draw vertical grid lines */
    if (gridCols > 0)
    {
        for (int i = 1; i < gridCols; i++)
        {
            const int x = i * (w - 1) / gridCols;
            for (int y = 0; y < h; y += 3)
            {
                px[x + y * w] = gridPixel;
            }
        }
    }

    const unsigned int count = data->row_count();
    if (count < 2) return {pxBuf};

    float range = maxY - minY;
    if (range < 0.0001f) range = 0.0001f;

    /* Draw chart line */
    int prev_x = -1, prev_y = -1;
    for (int i = 0; i < count; i++)
    {
        const int x = i * (w - 1) / (static_cast<int>(count) - 1);
        const auto value = data.get()->row_data(i).value_or(0.0f);
        int y = h - 1 - static_cast<int>(
            (value - minY) / range * static_cast<float>(h - 1));
        y = std::clamp(y, 0, h - 1);

        if (prev_x >= 0)
        {
            draw_line(px, w, h, prev_x, prev_y, x, y, lineColor);
        }
        prev_x = x;
        prev_y = y;
    }

    return {pxBuf};
}

std::shared_ptr<slint::Model<float>> ChartSupportCodeBase::calcBounds(const std::shared_ptr<slint::Model<float>>& data)
{
    if (data == nullptr)
    {
        return {};
    }
    const unsigned int count = data->row_count();
    if (count == 0)
    {
        auto result = std::make_shared<slint::VectorModel<float>>();
        result->push_back(0.0f);
        result->push_back(10.0f);
        result->push_back(5.0f);
        return result;
    }

    float minVal = INFINITY;
    float maxVal = -INFINITY;

    for (unsigned int i = 0; i < count; i++)
    {
        if (data->row_data(i).has_value())
        {
            const float val = data->row_data(i).value_or(0.0f);
            if (val < minVal) minVal = val;
            if (val > maxVal) maxVal = val;
        }
    }

    if (!std::isfinite(minVal)) minVal = 0.0f;
    if (!std::isfinite(maxVal)) maxVal = 1.0f;

    float range = maxVal - minVal;
    if (range < 0.0001f) range = 0.0001f;
    maxVal += range * 0.051f;
    minVal -= range * 0.051f;
    range = maxVal - minVal;

    // Determine granularity based on range
    // detect most significant
    // Calculate granularity based on order of magnitude
    // Find the power of 10 that's less than or equal to range
    float magnitude = std::pow(10.0f, std::floor(std::log10(range)));

    // Choose a nice step value (1, 2, or 5 times the magnitude)
    float granularity;
    float normalized = range / magnitude;

    if (normalized <= 2.0f)
    {
        granularity = magnitude * 0.5f;
    }
    else if (normalized <= 5.0f)
    {
        granularity = magnitude;
    }
    else
    {
        granularity = magnitude * 2.0f;
    }
    // Round bounds to granularity
    float boundMin = std::floor(minVal / granularity) * granularity;
    float boundMax = std::ceil(maxVal / granularity) * granularity;

    auto result = std::make_shared<slint::VectorModel<float>>();
    result->push_back(boundMin);
    result->push_back(boundMax);
    result->push_back((boundMax - boundMin) / granularity);
    return result;
}
