#pragma once
#include <unordered_set>

namespace gakumas::vr {
// Metadata only, scoped to one discovery batch. Never retains Unity objects.
class DiscoveryPresenceIndex {
public:
    template<class Objects, class GetClass>
    bool Build(const Objects& objects, GetClass getClass) {
        Reset();
        for (auto* object : objects) {
            if (!object) continue;
            auto* type = getClass(object);
            if (!type) { Reset(); return false; }
            classes_.insert(type);
        }
        complete_ = true;
        return true;
    }
    template<class Assignable>
    bool ProvesAbsent(void* target, Assignable assignable) const {
        if (!complete_ || !target) return false;
        for (auto* actual : classes_) if (assignable(target, actual)) return false;
        return true;
    }
    void Reset() { complete_ = false; classes_.clear(); }
    std::size_t ClassCount() const { return classes_.size(); }
private:
    bool complete_ = false;
    std::unordered_set<void*> classes_;
};
}
