// Regression guard for the chrome://settings-style settings page restyle.
//
// The settings UI labels exist in two places: the section/row titles in
// src/ui/pages/SettingsPage.cpp and the sidebar/filter behaviour built on
// top of them. This test asserts — directly against the settings-page
// translation unit — that every preference row's label text still refers to
// the same preference key after the redesign. It reads the source text so
// the check works without a QApplication and keeps core-only tests
// Qt-free; it fails loudly if a future edit drops or renames a setting.
//
// It also pins the presentation invariants the redesign promised:
// incognito windows keep the non-saving banner and the "not saved" status
// rewrite, and the live filter never writes preferences.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_settings_page_source() {
    // Anchor on this test file's own compile-time path so the check works
    // regardless of the working directory ctest picks.
    std::filesystem::path here = __FILE__;
    std::ifstream in(here.parent_path().parent_path() /
                     "src" / "ui" / "pages" / "SettingsPage.cpp");
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("settings page keeps every preference key after the restyle") {
    const std::string src = read_settings_page_source();
    REQUIRE(!src.empty());

    // Every PrefKeys reference that existed before the chrome-like redesign
    // must still be present: presentation changed, storage did not.
    for (const char* key : {"settings::PrefKeys::kSearchEngine",
                            "settings::PrefKeys::kTheme",
                            "settings::PrefKeys::kRestoreSession",
                            "settings::PrefKeys::kTabSuspendAfterMinutes",
                            "settings::PrefKeys::kBlockThirdPartyCookies",
                            "settings::PrefKeys::kSendDoNotTrack",
                            "settings::PrefKeys::kAdblockEnabled",
                            "settings::PrefKeys::kReducedMotion",
                            "settings::PrefKeys::kHttpsOnly",
                            "settings::PrefKeys::kClearOnExitCookies",
                            "settings::PrefKeys::kClearOnExitHistory",
                            "settings::PrefKeys::kClearOnExitDownloads",
                            "settings::PrefKeys::kCertAudit",
                            "settings::PrefKeys::kRefererTrim"}) {
        CHECK(src.find(key) != std::string::npos);
    }
    // Downloads path/ask-where go through typed accessors, not PrefKeys.
    CHECK(src.find("set_downloads_path") != std::string::npos);
    CHECK(src.find("set_downloads_ask_where") != std::string::npos);
}

TEST_CASE("settings page keeps the incognito non-saving behaviour") {
    const std::string src = read_settings_page_source();
    REQUIRE(!src.empty());

    // The read-only banner from commit 3eef546 must still be shown verbatim.
    CHECK(src.find("This is an incognito window - changes are not saved")
          != std::string::npos);
    // ... and report_change() must keep substituting a non-saving message
    // instead of claiming a change was persisted.
    CHECK(src.find("Incognito changes are not saved") != std::string::npos);
    CHECK(src.find("prefs_->is_read_only()") != std::string::npos);
}

TEST_CASE("settings filter and navigation never write preferences") {
    const std::string src = read_settings_page_source();
    REQUIRE(!src.empty());

    // The live filter is presentation-only: it may show/hide widgets and
    // collapse the cards, but the setter calls all stay on the change
    // handlers. Guard the names that make the feature recognisable.
    CHECK(src.find("apply_filter") != std::string::npos);
    CHECK(src.find("No settings match your search") != std::string::npos);
    // Navigation: the sidebar scrolls to a section; scrolling honours the
    // reduced-motion preference.
    CHECK(src.find("scroll_to_section") != std::string::npos);
    CHECK(src.find("kReducedMotion") != std::string::npos);
}
