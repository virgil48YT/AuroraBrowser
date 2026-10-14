#include <doctest.h>

#include <algorithm>

#include "cef/MediaPolicy.hpp"

using namespace aurora::cef;

TEST_CASE("Twitch origin matching respects domain boundaries and schemes") {
    CHECK(is_twitch_page("https://twitch.tv/channel"));
    CHECK(is_twitch_page("https://www.twitch.tv/videos/123"));
    CHECK(is_twitch_page("https://player.twitch.tv/?channel=test"));
    CHECK(is_twitch_page("https://clips.twitch.tv/ExampleClip"));
    CHECK(is_twitch_page("http://m.twitch.tv/"));
    CHECK_FALSE(is_twitch_page("https://twitch.tv.evil.test/"));
    CHECK_FALSE(is_twitch_page("https://nottwitch.tv/"));
    CHECK_FALSE(is_twitch_page("https://fake-twitch.tv/"));
    CHECK_FALSE(is_twitch_page("https://twitch.tv@evil.test/"));
    CHECK_FALSE(is_twitch_page("file://twitch.tv/video"));
    CHECK_FALSE(is_twitch_page("about:blank"));
}

TEST_CASE("Shields exclude only Twitch playback CDN resources on Twitch pages") {
    constexpr auto page = "https://www.twitch.tv/channel";
    CHECK(is_twitch_media_resource(page, "https://usher.ttvnw.net/api/channel/hls/test.m3u8"));
    CHECK(is_twitch_media_resource(page, "https://video-weaver.ams.hls.ttvnw.net/segment.ts"));
    CHECK(is_twitch_media_resource(page, "https://clips-media-assets2.twitch.tv/clip.mp4"));
    CHECK_FALSE(is_twitch_media_resource(page, "https://static.jtvnw.net/assets/player.js"));
    CHECK_FALSE(is_twitch_media_resource(page, "https://ads.example.com/ad.ts"));
    CHECK_FALSE(is_twitch_media_resource(page, "https://ttvnw.net.evil.test/segment.ts"));
    CHECK_FALSE(is_twitch_media_resource(page, "https://fakettvnw.net/segment.ts"));
    CHECK_FALSE(is_twitch_media_resource(page, "https://ttvnw.net@evil.test/segment.ts"));
    CHECK_FALSE(is_twitch_media_resource(page, "file://usher.ttvnw.net/segment.ts"));
    CHECK_FALSE(is_twitch_media_resource("https://example.com/", "https://usher.ttvnw.net/a.m3u8"));
    CHECK_FALSE(is_twitch_media_resource("https://twitch.tv.evil.test/",
                                          "https://usher.ttvnw.net/a.m3u8"));
}

TEST_CASE("media policy permits autoplay and uses the engine's version") {
    const auto sw = media_command_line_switches();
    CHECK(std::find(sw.begin(), sw.end(),
                    "autoplay-policy=no-user-gesture-required") != sw.end());
    CHECK(chrome_user_agent_product(152, 0, 7977, 54) == "Chrome/152.0.7977.54");
}
