// Media playback policy shared by the CEF startup, request handler and tab
// model. This file intentionally has no CEF or Qt dependency so CI can test it.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aurora::cef {

// Only actual Twitch origins, including player/clips subdomains. Never match
// similarly named sites (twitch.tv.example, nottwitch.tv, etc.).
bool is_twitch_page(std::string_view url);

// Shields must not cancel Twitch's HLS manifests or segments. Player scripts
// fetch these as XHR/fetch, not necessarily as Chromium RT_MEDIA resources.
// This exception applies only to Twitch pages and Twitch-owned media hosts;
// all other requests still go through the user's normal Shields rules.
bool is_twitch_media_resource(std::string_view page_url,
                              std::string_view resource_url);

// Startup switches, applied before CEF creates its renderer processes.
std::vector<std::string> media_command_line_switches();

// Use the version of the actual CEF/Chromium binary, not a hard-coded Chrome
// release. CEF builds the rest of the platform-specific user agent itself.
std::string chrome_user_agent_product(int major, int minor, int build,
                                      int patch);

} // namespace aurora::cef
