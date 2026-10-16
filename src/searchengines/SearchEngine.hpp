// Aurora — search engine model: built-in engines, user-defined (custom)
// engines, template validation, and default-engine resolution.
//
// Pure core: no Qt, no CEF, no SQLite. The settings page validates input with
// these functions before it ever posts work to SearchEngineService, and the
// service validates again on its worker thread (the authoritative check).
//
// A custom engine is a name plus an HTTPS URL template that contains exactly
// one "%s", which is replaced by the URL-encoded search terms:
//
//     https://search.example.com/?q=%s
//
// The default engine is stored in preferences under PrefKeys::kSearchEngine
// (search.engine), exactly as before: a built-in identifier ("duckduckgo",
// "google", "bing", "startpage"), or "custom:<id>" for a custom engine row.
// Existing profiles therefore keep their choice with no preference migration.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::searchengines {

inline constexpr size_t kMaxNameLength = 64;
inline constexpr size_t kMaxTemplateLength = 2048;

// A user-defined engine as stored in the search_engines table.
struct SearchEngine {
    int64_t id = 0;
    std::string name;
    std::string url_template;  // https://..., exactly one "%s"
};

// A built-in engine. Templates and home pages come from core/Url.cpp
// (search_engine_template / search_engine_home), the single source of truth.
struct BuiltinEngine {
    std::string_view key;    // value stored in preferences
    std::string_view name;   // label shown to the user
};

// DuckDuckGo first: it is the fallback for unknown or deleted engines.
std::span<const BuiltinEngine> builtin_engines();
bool is_builtin_key(std::string_view key);

// --- Validation --------------------------------------------------------------

enum class TemplateError {
    Ok,
    Empty,
    TooLong,
    InvalidCharacter,      // whitespace or control characters
    NotHttps,              // must start with https://
    InvalidHost,           // empty host, credentials, or odd host characters
    PlaceholderInHost,     // "%s" may not be part of the host
    MissingPlaceholder,    // no "%s"
    MultiplePlaceholders,  // more than one "%s"
};

enum class NameError {
    Ok,
    Empty,
    TooLong,
    InvalidCharacter,  // control characters
};

// `url_template` is checked as given; callers trim user input first with
// normalize_input().
TemplateError validate_template(std::string_view url_template);
NameError validate_name(std::string_view name);

// One-sentence explanations, suitable for an inline form error.
std::string_view describe(TemplateError error);
std::string_view describe(NameError error);

// Trim leading/trailing ASCII whitespace from user input.
std::string normalize_input(std::string_view text);

// --- Expansion ---------------------------------------------------------------

// The search-results URL for `terms` (URL-encoded into the placeholder).
std::string expand_template(std::string_view url_template, std::string_view terms);

// "https://host[:port]/" of a template — the engine's home page, used for the
// Home button and new tabs (which open the default engine's home page).
// Empty when the template is not valid.
std::string template_home(std::string_view url_template);

// --- Default-engine resolution -----------------------------------------------

// Preference value for a custom engine row: "custom:<id>".
std::string custom_engine_key(int64_t id);
// The row id of a "custom:<id>" preference value, or nullopt.
std::optional<int64_t> parse_custom_engine_key(std::string_view key);

struct ResolvedEngine {
    std::string key;           // the preference value this resolves
    std::string name;
    std::string url_template;  // always contains exactly one "%s"
    std::string home;
    bool is_custom = false;
    // True when `pref_value` named an engine that does not exist (unknown
    // identifier, or a custom engine that was deleted / is not loaded yet)
    // and DuckDuckGo was used instead.
    bool fell_back = false;
};

// Resolve the default engine preference against the current custom engines.
ResolvedEngine resolve_default_engine(std::string_view pref_value,
                                      const std::vector<SearchEngine>& custom);

} // namespace aurora::searchengines
