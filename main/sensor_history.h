#pragma once

#include <cstddef>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

template <typename T, size_t Capacity>
class SensorHistory {
    T readings_[Capacity]{};
    size_t count_ = 0;
    size_t head_ = 0;
    mutable SemaphoreHandle_t mutex_ = xSemaphoreCreateMutex();

public:
    void push(const T& value) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        readings_[head_] = value;
        head_ = (head_ + 1) % Capacity;
        if (count_ < Capacity) count_++;
        xSemaphoreGive(mutex_);
    }

    /** Get reading by age: 0 = most recent, 1 = previous, … */
    T operator[](size_t age) const {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        T copy = readings_[(head_ + Capacity - 1 - age) % Capacity];
        xSemaphoreGive(mutex_);
        return copy;
    }

    T last() const { return (*this)[0]; }

    [[nodiscard]] size_t count() const {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        size_t c = count_;
        xSemaphoreGive(mutex_);
        return c;
    }

    static constexpr size_t capacity() { return Capacity; }

    template <typename Func>
    auto map(Func&& func) const -> std::vector<decltype(func(std::declval<T>()))>
    {
        using ResultType = decltype(func(std::declval<T>()));
        std::vector<ResultType> result;

        xSemaphoreTake(mutex_, portMAX_DELAY);
        result.reserve(count_);
        for (size_t i = 0; i < count_; ++i)
        {
            size_t idx = (head_ + Capacity - count_ + i) % Capacity;
            result.push_back(func(readings_[idx]));
        }
        xSemaphoreGive(mutex_);

        return result;
    }
};
