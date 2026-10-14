// Aurora — bulk session-restore driver.
//
// Restoring a session opens many tabs in one synchronous burst. That is the
// single most dangerous startup path in a browser: each tab notifies observers,
// and the window's Added handler used to make every restored tab *visible* in
// turn, which created one web engine host (with a native window handle) per
// tab before the event loop ever ran. With 11+ tabs that reliably crashed.
//
// The sequencing rules that avoid it are policy, not UI code, so they live here
// where they can be unit-tested without Qt:
//
//   1. The whole loop runs in "bulk mode". Observers are told, so the UI can
//      suppress its make-this-tab-visible path for the duration.
//   2. The seed tab is closed inside bulk mode too (removing the visible tab
//      is exactly what tempts a stacked-widget container into promoting some
//      arbitrary other tab to visible).
//   3. Bulk mode ends, and only then is exactly ONE tab activated and shown.
//      That single show is the only host the UI may create. The show is an
//      explicit callback rather than a side effect of activation, because
//      activating an already-active tab notifies nothing.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include "core/Types.hpp"
#include "session/SessionService.hpp"

namespace aurora::core { class TabManager; }

namespace aurora::session {

struct RestoreOutcome {
    int opened = 0;                  // tabs actually opened
    core::TabId active{0};           // the one tab left active (invalid if none)
};

// Called with true when bulk mode begins and false when it ends. Exactly one
// true/false pair per call, even if a callee throws.
using BulkModeFn = std::function<void(bool)>;
// Called exactly once, after bulk mode ends, with the single tab whose content
// the window must display. Not called when nothing is active.
using ShowTabFn = std::function<void(core::TabId)>;
// Optional progress hook: (opened_so_far, total_planned, url). Used for Info
// logging so a future startup crash is locatable in the log.
using RestoreProgressFn = std::function<void(int, int, const std::string&)>;

// Open every planned tab into `tabs`, close `seed` (the placeholder tab the
// window starts with) once at least one tab was restored, and leave exactly one
// tab active. Never leaves the manager empty.
RestoreOutcome run_restore(core::TabManager& tabs, const RestorePlan& plan,
                           core::TabId seed,
                           const BulkModeFn& set_bulk_mode = {},
                           const ShowTabFn& show_active = {},
                           const RestoreProgressFn& progress = {});

} // namespace aurora::session
