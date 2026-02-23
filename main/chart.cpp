#include <cmath>
#include <cstring>

#include "chart.h"

/*-----------------------------------------------------------------------
 * Simple chart renderer — draws AHT20 temperature history into an image
 *---------------------------------------------------------------------*/

static void draw_line(slint::Rgba8Pixel* buf, int w, int h,
                      int x0, int y0, int x1, int y1, const slint::Rgba8Pixel& color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;)
    {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
            buf[y0 * w + x0] = color;
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
                                                const slint::Color& lineColor, const slint::
                                                Color& gridColor, const int gridRows, const int gridCols)
{
    slint::SharedPixelBuffer<slint::Rgba8Pixel> pxbuf(w, h);
    auto* px = pxbuf.begin();

    /* Clear to transparent */
    memset(px, 0, w * h * sizeof(slint::Rgba8Pixel));

    /* Draw horizontal grid lines */
    const slint::Rgba8Pixel gridPixel{gridColor.red(), gridColor.green(), gridColor.blue(), gridColor.alpha()};

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
    if (count < 2) return {pxbuf};

    float minY = INFINITY;
    float maxY = -INFINITY;
    for (int i = 0; i < count; i++)
    {
        const float t = data.get()->row_data(i).value_or(0.0f);
        if (t < minY) minY = t;
        if (t > maxY) maxY = t;
    }

    if (!std::isfinite(minY)) minY = 0;
    if (!std::isfinite(maxY)) maxY = 0;
    /* Add margin */
    const float delta = (maxY - minY) / 20;
    maxY += delta;
    minY -= delta;
    float range;
    do
    {
        range = maxY - minY;
        if (range >= 0.0001) break;
        maxY += 0.00005f;
        minY -= 0.00005f;
    }
    while (true);

    /* Draw chart line */
    int prev_x = -1, prev_y = -1;
    for (int i = 0; i < count; i++)
    {
        const slint::Rgba8Pixel linePx{lineColor.red(), lineColor.green(), lineColor.blue(), lineColor.alpha()};
        const int x = i * (w - 1) / (static_cast<int>(count) - 1);
        int y = h - 1 - static_cast<int>(
            (data.get()->row_data(i).value_or(0.0f) - minY) / range * static_cast<float>(h - 1));
        y = std::clamp(y, 0, h - 1);

        if (prev_x >= 0)
            draw_line(px, w, h, prev_x, prev_y, x, y, linePx);
        prev_x = x;
        prev_y = y;
    }

    return {pxbuf};
}
