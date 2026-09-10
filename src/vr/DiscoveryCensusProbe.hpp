#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace gakumas::vr::discovery {
struct ProbeBudget {
    using Clock = std::chrono::steady_clock;
    unsigned runs = 0;
    Clock::time_point next{};
    bool Acquire(bool enabled, bool sampled, Clock::time_point now) {
        if (!enabled || !sampled || runs >= 16 || now < next) return false;
        ++runs;
        next = now + std::chrono::seconds(5);
        return true;
    }
};
using ObjectKeys = std::vector<std::uintptr_t>;
template<class Objects>
ObjectKeys Keys(const Objects& objects) {
    ObjectKeys keys;
    keys.reserve(objects.size());
    for (auto* object : objects) keys.push_back(reinterpret_cast<std::uintptr_t>(object));
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}
// Exact set differences, not a hash/count proxy for equivalence.
inline std::size_t OnlyIn(const ObjectKeys& a, const ObjectKeys& b) {
    std::size_t missing = 0, j = 0;
    for (auto key : a) {
        while (j < b.size() && b[j] < key) ++j;
        if (j == b.size() || b[j] != key) ++missing;
    }
    return missing;
}
}
