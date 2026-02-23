#ifndef PICTURE_WEATHER_STATION_CHART_H
#define PICTURE_WEATHER_STATION_CHART_H
#include "slint-esp.h"

class ChartSupportCodeBase
{
protected:
    static slint::Image render_chart(int w, int h, const std::shared_ptr<slint::Model<float>> &data,
                const slint::Color & lineColor,const  slint::Color &gridColor, int gridRows, int gridCols);
};

template <typename UI_CLASS, typename CHART_SUPPORT_CLASS>
class ChartSupportCode : public ChartSupportCodeBase
{
    slint::ComponentHandle<UI_CLASS>* ui_;

    auto global() const -> const CHART_SUPPORT_CLASS&
    {
        return ui_->template global<CHART_SUPPORT_CLASS>();
    }

public:
    explicit ChartSupportCode(slint::ComponentHandle<UI_CLASS>& ui)

    {
        ui_ = &ui;
    }

    inline void setup()
    {
        (*ui_)->template global<CHART_SUPPORT_CLASS>().on_renderChart([
            ](int w, int h, std::shared_ptr<slint::Model<float>> data,
                slint::Color lineColor, slint::Color gridColor, int gridRows, int gridCols)
            {
                return render_chart(w, h, data, lineColor, gridColor, gridRows, gridCols);
            });
    }
};

#endif //PICTURE_WEATHER_STATION_CHART_H
