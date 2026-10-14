#include "cef/MediaPolicy.hpp"

#include "adblock/AdblockService.hpp"

namespace aurora::cef {
namespace {

bool is_web_url(std::string_view url) {
    return url.starts_with("https://") || url.starts_with("http://");
}

bool domain_or_subdomain(std::string_view host, std::string_view domain) {
    return host == domain ||
           (host.size() > domain.size() &&
            host[host.size() - domain.size() - 1] == '.' &&
            host.ends_with(domain));
}

} // namespace

bool is_twitch_page(std::string_view url) {
    return is_web_url(url) &&
           domain_or_subdomain(adblock::site_key(url), "twitch.tv");
}

bool is_twitch_media_resource(std::string_view page_url,
                              std::string_view resource_url) {
    if (!is_twitch_page(page_url) || !is_web_url(resource_url)) return false;
    const std::string host = adblock::site_key(resource_url);
    return domain_or_subdomain(host, "ttvnw.net") ||
           host == "clips-media-assets2.twitch.tv";
}

std::vector<std::string> media_command_line_switches() {
    // CEF/Chromium's gesture requirement prevents a stream from starting when
    // a Twitch tab is restored or opened from a link. This Chromium policy is
    // process-wide (there is no per-tab autoplay switch); it does not grant
    // camera or microphone access, which still uses the permission handler.
    return {"autoplay-policy=no-user-gesture-required"};
}

std::string chrome_user_agent_product(int major, int minor, int build,
                                      int patch) {
    return "Chrome/" + std::to_string(major) + "." + std::to_string(minor) +
           "." + std::to_string(build) + "." + std::to_string(patch);
}

} // namespace aurora::cef
