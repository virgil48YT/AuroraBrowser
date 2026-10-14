// Aurora — navigation entry.
//
// One step in a tab's back/forward history. Mirrors the concept of a
// Chromium navigation entry but is a plain value type so it can be persisted
// (session restore) and unit-tested without CEF.
#pragma once

#include <string>
#include <chrono>
#include "core/Url.hpp"

namespace aurora::core {

struct NavigationEntry {
    Url url;
    std::string title;
    // Favicon URL if known; empty when not yet loaded.
    std::string favicon_url;
    std::chrono::system_clock::time_point visited_at =
        std::chrono::system_clock::now();
};

} // namespace aurora::core
