#include "esp_attr.h"
#include "esp_log.h"
#include "rtc_state.h"

static const char *TAG = "rtc_state";

constexpr uint32_t RTC_MAGIC = 0xE5C30001U;
constexpr size_t RTC_CAP = 96;

RTC_DATA_ATTR static uint32_t          rtc_magic;
RTC_DATA_ATTR static uint16_t          rtc_count;
RTC_DATA_ATTR static shtc3_reading_t   rtc_buf[RTC_CAP];

void rtc_history_save(const SensorHistory<shtc3_reading_t, RTC_CAP> &src)
{
    auto data = src.map([](const shtc3_reading_t &r) { return r; });
    rtc_count = static_cast<uint16_t>(data.size());
    for (size_t i = 0; i < rtc_count; ++i) rtc_buf[i] = data[i];
    rtc_magic = RTC_MAGIC;
    ESP_LOGI(TAG, "Saved %u SHTC3 samples to RTC memory", rtc_count);
}

void rtc_history_restore(SensorHistory<shtc3_reading_t, RTC_CAP> &dst)
{
    if (rtc_magic != RTC_MAGIC) {
        ESP_LOGI(TAG, "No valid RTC history (magic=0x%08lX)",
                 static_cast<unsigned long>(rtc_magic));
        return;
    }
    uint16_t n = rtc_count;
    if (n > RTC_CAP) n = RTC_CAP;
    for (uint16_t i = 0; i < n; ++i) dst.push(rtc_buf[i]);
    ESP_LOGI(TAG, "Restored %u SHTC3 samples from RTC memory", n);
}
