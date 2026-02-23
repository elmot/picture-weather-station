#pragma once

#include <cstddef>
#include <vector>

template <typename T, size_t Capacity>
class SensorHistory {
    T readings_[Capacity]{};
    size_t count_ = 0;
    size_t head_ = 0;

public:
    void push(const T& value) {
        readings_[head_] = value;
        head_ = (head_ + 1) % Capacity;
        if (count_ < Capacity) count_++;
    }

    /** Get reading by age: 0 = most recent, 1 = previous, … */
    const T& operator[](size_t age) const {
        return readings_[(head_ + Capacity - 1 - age) % Capacity];
    }

    const T& last() const { return (*this)[0]; }
    [[nodiscard]] size_t count() const { return count_; }
    static constexpr size_t capacity() { return Capacity; }

    template <typename Func>
    auto map(Func&& func) const -> std::vector<decltype(func(std::declval<T>()))>
    {
        using ResultType = decltype(func(std::declval<T>()));
        std::vector<ResultType> result;
        result.reserve(count_);

        for (size_t i = 0; i < count_; ++i)
        {
            size_t idx = (head_ + Capacity - count_ + i) % Capacity;
            result.push_back(func(readings_[idx]));
        }

        return result;
    }
};
