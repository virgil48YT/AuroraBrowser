#include "adblock/AdblockEngine.hpp"

#include "util/Log.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

#if defined(AURORA_ADBLOCK_FFI)
// The Rust static library (third_party/adblock). Symbols are resolved at
// link time; nothing is dlopen'd, so a mismatch is a build error, not a
// runtime surprise.
extern "C" {
void* aurora_adblock_engine_new(const char* rules);
void aurora_adblock_engine_free(void* engine);
int aurora_adblock_check(const void* engine, const char* url,
                         const char* source_url, const char* request_type);
// Newline-separated CSS selectors for one page URL; the buffer belongs to
// the caller and goes back through the _free entry point below.
char* aurora_adblock_cosmetic_filters(const void* engine, const char* url);
void aurora_adblock_cosmetic_filters_free(char* selectors);
// Opaque Rust-owned snapshot. Accessor strings are borrowed, not allocations
// C++ may free individually; the matching snapshot deleter owns both.
void* aurora_adblock_cosmetic_resources(const void* engine, const char* url);
const char* aurora_adblock_cosmetic_resources_json(const void* resources);
const char* aurora_adblock_cosmetic_resources_scripts(const void* resources);
void aurora_adblock_cosmetic_resources_free(void* resources);
}
#endif

namespace aurora::adblock {

const char* starter_rules() {
    // Aurora-authored starter list: the biggest ad/tracker networks plus a
    // few conservative generic path patterns. Intentionally tiny and easy
    // to read; a full filter list belongs in the user's adblock-rules.txt.
    return
        "! Aurora starter ad-block rules\n"
        "||doubleclick.net^\n"
        "||googlesyndication.com^\n"
        "||googleadservices.com^\n"
        "||google-analytics.com^\n"
        "||adservice.google.com^\n"
        "||adnxs.com^\n"
        "||adsrvr.org^\n"
        "||criteo.com^\n"
        "||criteo.net^\n"
        "||taboola.com^\n"
        "||outbrain.com^\n"
        "||moatads.com^\n"
        "||amazon-adsystem.com^\n"
        "||scorecardresearch.com^\n"
        "||quantserve.com^\n"
        "||chartbeat.com^\n"
        "||hotjar.com^\n"
        "||mixpanel.com^\n"
        "||appsflyer.com^\n"
        "||adsystem.com^\n"
        "/banner/\n"
        "/adframe.\n"
        "/adserver/\n"
        "/popunder|\n"
        "_300x250_\n"
        "_728x90_\n"
        // Cosmetic (element-hiding) rules, Adblock Plus syntax:
        // "domain##selector" hides the selector on that site, a bare
        // "##selector" everywhere. These are NOT comments despite the '#'.
        // They matter because some ads are served from the very host that
        // serves the content (YouTube streams its video ads off
        // googlevideo.com, exactly like the video the user asked for), so a
        // URL rule cannot remove them without breaking playback. Kept
        // deliberately conservative: only long-lived, ad-only selectors.
        //
        // The in-player ad module (.video-ads) and the skip button
        // (.ytp-ad-skip-button / -text) MUST NOT be hidden here: hiding them
        // blanks the player (black screen) while the ad keeps playing, and
        // removes the skip button the ad-skip scriptlet clicks. The poster
        // frame (.ytp-cued-thumbnail-overlay) is likewise left visible.
        "! Aurora starter cosmetic (element-hiding) rules\n"
        "youtube.com##.ytp-ad-overlay-container\n"
        "youtube.com##.ytp-ad-overlay-slot\n"
        "youtube.com##.ytp-ad-text\n"
        "youtube.com###player-ads\n"
        "youtube.com###masthead-ad\n"
        "youtube.com##ytd-ad-slot-renderer\n"
        "youtube.com##ytd-in-feed-ad-layout-renderer\n"
        "youtube.com##ytd-promoted-video-renderer\n"
        "youtube.com##ytd-companion-slot-renderer\n"
        "##.ad-banner\n"
        "##.ad-container\n"
        "###ad-banner\n";
}

bool engine_available() {
#if defined(AURORA_ADBLOCK_FFI)
    return true;
#else
    return false;
#endif
}

AdblockEngine::AdblockEngine(std::string_view rules) {
#if defined(AURORA_ADBLOCK_FFI)
    const std::string owned(rules);
    engine_ = aurora_adblock_engine_new(owned.c_str());
    if (!engine_) {
        AURORA_LOG(util::LogLevel::Warn, "adblock")
            << "engine creation failed (invalid rules?)";
    }
#else
    (void)rules;
#endif
}

AdblockEngine::~AdblockEngine() {
#if defined(AURORA_ADBLOCK_FFI)
    if (engine_) aurora_adblock_engine_free(engine_);
#endif
}

AdblockEngine::AdblockEngine(AdblockEngine&& other) noexcept
    : engine_(other.engine_) {
    other.engine_ = nullptr;
}

AdblockEngine& AdblockEngine::operator=(AdblockEngine&& other) noexcept {
    if (this != &other) {
#if defined(AURORA_ADBLOCK_FFI)
        if (engine_) aurora_adblock_engine_free(engine_);
#endif
        engine_ = other.engine_;
        other.engine_ = nullptr;
    }
    return *this;
}

bool AdblockEngine::should_block(std::string_view request_url,
                                 std::string_view source_url,
                                 std::string_view request_type) const {
#if defined(AURORA_ADBLOCK_FFI)
    if (!engine_) return false;
    const std::string url(request_url);
    const std::string source(source_url);
    const std::string type(request_type);
    // Only the exact return code 1 blocks; -1 (bad args) and 0 allow.
    return aurora_adblock_check(engine_, url.c_str(), source.c_str(),
                                type.c_str()) == 1;
#else
    (void)request_url;
    (void)source_url;
    (void)request_type;
    return false;
#endif
}

CosmeticResources AdblockEngine::cosmetic_resources(std::string_view url) const {
    CosmeticResources result;
#if defined(AURORA_ADBLOCK_FFI)
    if (!engine_) return result;
    const std::string page(url);
    const std::unique_ptr<void, decltype(&aurora_adblock_cosmetic_resources_free)>
        snapshot(aurora_adblock_cosmetic_resources(engine_, page.c_str()),
                 &aurora_adblock_cosmetic_resources_free);
    if (!snapshot) return result;
    // RAII returns the allocation to Rust even if a std::string copy throws.
    if (const char* json = aurora_adblock_cosmetic_resources_json(snapshot.get())) {
        result.json = json;
    }
    if (const char* scripts = aurora_adblock_cosmetic_resources_scripts(snapshot.get())) {
        result.scripts = scripts;
    }
#else
    (void)url;
#endif
    return result;
}

std::vector<std::string> AdblockEngine::cosmetic_filters(
    std::string_view url) const {
    std::vector<std::string> selectors;
#if defined(AURORA_ADBLOCK_FFI)
    if (!engine_) return selectors;
    const std::string page(url);
    const std::unique_ptr<char, decltype(&aurora_adblock_cosmetic_filters_free)>
        joined(aurora_adblock_cosmetic_filters(engine_, page.c_str()),
               &aurora_adblock_cosmetic_filters_free);
    if (!joined) return selectors;  // bad arguments, or an unusable URL
    const std::string_view list(joined.get());
    size_t pos = 0;
    while (pos <= list.size()) {
        const auto end = list.find('\n', pos);
        const auto len = end == std::string::npos ? std::string::npos : end - pos;
        std::string line(list.substr(pos, len));
        // Filter lists fetched on Windows keep their CR; a selector with a
        // stray \r never matches anything.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) selectors.push_back(std::move(line));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
#else
    (void)url;
#endif
    return selectors;
}

namespace {

// Number of actual filters in a rule list: non-empty lines that are not
// EasyList comments ('!' and the '[Adblock Plus 2.0]' style header).
size_t count_filters(const std::string& list) {
    size_t filters = 0;
    size_t pos = 0;
    while (pos <= list.size()) {
        const auto end = list.find('\n', pos);
        const auto len = end == std::string::npos ? std::string::npos : end - pos;
        const std::string line = list.substr(pos, len);
        if (!line.empty() && line[0] != '!' && line[0] != '[' &&
            line != "\r") {
            ++filters;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return filters;
}

} // namespace

std::string load_rules(const std::vector<std::string>& candidate_paths) {
    std::string rules = starter_rules();
    std::string tried;
    for (const std::string& path : candidate_paths) {
        if (path.empty()) continue;
        if (!tried.empty()) tried += ", ";
        tried += '\'';
        tried += path;
        tried += '\'';
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string extra = ss.str();
        if (extra.empty()) continue;
        rules += "\n";
        rules += extra;
        AURORA_LOG(util::LogLevel::Info, "adblock")
            << "loaded user filter list (EasyList) from '" << path << "': "
            << count_filters(extra) << " rules added to the "
            << count_filters(starter_rules()) << " starter rules";
        return rules;
    }
    // Nothing to look for => nothing to warn about (a caller that passes no
    // path is not asking for a user list).
    if (tried.empty()) return rules;
    AURORA_LOG(util::LogLevel::Warn, "adblock")
        << "EasyList NOT FOUND: no readable filter list at " << tried
        << " — only the " << count_filters(starter_rules())
        << " built-in starter rules are active, so most ads will NOT be "
           "blocked. Run scripts/fetch_filter_lists.sh and restart Aurora.";
    return rules;
}

std::string load_rules(const std::string& user_list_path) {
    return load_rules(std::vector<std::string>{user_list_path});
}

} // namespace aurora::adblock
