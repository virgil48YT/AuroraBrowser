// Aurora — network request blocker.
//
// Thin C++ owner of the Rust adblock engine (brave/adblock-rust behind a C
// ABI, third_party/adblock). Methods lock nothing on their own: the caller
// (the CEF request interceptor on the IO thread, or
// the UI thread while installing rules) serializes access externally — see
// CefAdapter's adblock state mutex.
//
// When Aurora is built WITHOUT the Rust static library
// (AURORA_ADBLOCK_FFI undefined — no cargo toolchain, or
// AURORA_ENABLE_ADBLOCK=OFF) this compiles to an inert stub: available() is
// false and should_block() is always false, so the rest of the browser (and
// every unit test) runs unchanged. The shield UI reports "engine not
// available" instead of pretending to block.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aurora::adblock {

// A small self-written starter rule list (Aurora-authored, no third-party
// license attached): the major ad/tracker hosts plus a few conservative
// generic path patterns. Users can layer a real filter list (EasyList etc.)
// on top via the profile's adblock-rules.txt — see
// scripts/fetch_filter_lists.sh and README "Ad blocking".
const char* starter_rules();

struct CosmeticResources {
    // The engine's complete serialized resource object: hide_selectors,
    // non_hide_styles, remove_attrs/remove_classes, exceptions and any
    // procedural action representation supplied by the pinned engine.
    // Kept as JSON so the core/stub build needs no CEF or JSON dependency.
    std::string json;
    // Already-expanded engine scriptlets, kept separate so CEF can execute
    // them directly rather than using page eval()/script tags (CSP).
    std::string scripts;

    bool empty() const { return json.empty() && scripts.empty(); }
};

class AdblockEngine {
public:
    // Parses the (newline-separated) rule list. Cheap on failure: an
    // invalid/empty list yields an engine that blocks nothing.
    explicit AdblockEngine(std::string_view rules);
    ~AdblockEngine();
    AdblockEngine(const AdblockEngine&) = delete;
    AdblockEngine& operator=(const AdblockEngine&) = delete;
    AdblockEngine(AdblockEngine&&) noexcept;
    AdblockEngine& operator=(AdblockEngine&&) noexcept;

    // True when a real engine is behind the pointer (Rust build). A stub
    // engine is "valid" in the sense that it is safe to call — but callers
    // that want to report engine availability use this.
    bool available() const { return engine_ != nullptr; }

    // request_url: the resource being fetched. source_url: the page it is
    // fetched for (the first-party URL; may be empty). request_type: the
    // adblock-rust spelling of the resource type ("script", "image",
    // "stylesheet", "xmlhttprequest", "subdocument", "media", "font",
    // "websocket", "other"). False whenever unsure — blocking is the
    // risky action, allowing is always safe.
    bool should_block(std::string_view request_url,
                      std::string_view source_url,
                      std::string_view request_type) const;

    // All cosmetic resources for one page, copied out of a Rust-owned
    // snapshot while the caller holds the engine-state mutex. Empty on a
    // stub build or query failure. The returned strings own their storage
    // and can be used after releasing that mutex or replacing the engine.
    CosmeticResources cosmetic_resources(std::string_view url) const;

    // Selector-only compatibility API. New renderer code should use the
    // complete resources above. Element hiding removes ad markup, not a
    // video ad's media timeline; playback needs the separate YouTube helper.
    std::vector<std::string> cosmetic_filters(std::string_view url) const;

private:
    void* engine_ = nullptr;  // adblock::Engine* from the Rust side
};

// True when this build linked the real Rust engine. Reported by the shield
// UI so it never claims to block when it cannot.
bool engine_available();

// Combine the starter rules with the first readable user filter list among
// `candidate_paths`, tried in order (nothing readable => starter rules only,
// logged as a warning). More than one candidate exists because the fetch
// script has written adblock-rules.txt to a legacy location in older
// versions — see scripts/fetch_filter_lists.sh and main.cpp's startup path.
// Empty entries are skipped.
std::string load_rules(const std::vector<std::string>& candidate_paths);

// Single-path convenience overload (the original signature): equivalent to
// load_rules({user_list_path}). Exposed for tests and older callers.
std::string load_rules(const std::string& user_list_path);

} // namespace aurora::adblock
