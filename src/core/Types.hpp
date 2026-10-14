// Aurora — core value types shared across the browser process.
//
// These types are deliberately free of Qt and CEF dependencies so the core
// library can be compiled and unit-tested on a headless CI machine. The UI
// and CEF layers translate between these domain types and their own handles.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace aurora::core {

// Monotonically increasing, process-wide stable identifier for a tab.
// Numeric rather than a UUID because tabs are created at human speed and the
// identifier only needs to be unique within a single browser process lifetime.
class TabId {
public:
    using value_type = uint64_t;
    TabId() : value_(0) {}
    explicit TabId(value_type v) : value_(v) {}
    static TabId generate() {
        static std::atomic<value_type> counter{1};
        return TabId(counter.fetch_add(1, std::memory_order_relaxed));
    }
    value_type value() const { return value_; }
    bool valid() const { return value_ != 0; }
    bool operator==(const TabId& o) const { return value_ == o.value_; }
    bool operator!=(const TabId& o) const { return value_ != o.value_; }
    bool operator<(const TabId& o) const { return value_ < o.value_; }

private:
    value_type value_;
};

} // namespace aurora::core

// std::hash specialization so TabId works as an unordered_* key.
namespace std {
template <>
struct hash<aurora::core::TabId> {
    size_t operator()(const aurora::core::TabId& id) const noexcept {
        return std::hash<aurora::core::TabId::value_type>{}(id.value());
    }
};
} // namespace std
