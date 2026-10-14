# Twitch playback in Aurora

Aurora opens `twitch.tv`, `player.twitch.tv`, and `clips.twitch.tv` in ordinary
CEF tabs; no Twitch proxy, embed wrapper, altered page JavaScript, or external
player is needed. The CEF startup policy advertises a Chrome product version
matching the **bundled Chromium version**, without hard-coding a release or
replacing the OS-specific user-agent string ([CEF user-agent settings](https://cef-builds.spotifycdn.com/docs/140.0/structcef__settings__t.html)).
It allows media autoplay at process startup; this is a **global** Chromium
policy, not a site permission ([CEF embedder autoplay example](https://blog.4d.com/custom-parameters-for-initializing-embedded-web-area/)).
It does **not** bypass the existing microphone/camera/notification prompts.
Twitch HLS manifest/segment requests on `ttvnw.net` and clip media on
`clips-media-assets2.twitch.tv` bypass Shields **only while browsing Twitch**;
other ad/tracker requests are filtered as before. Automatic idle suspension
skips Twitch tabs so a muted background stream does not freeze.

## Required CEF media codecs

The ordinary CEF binary fetched by `scripts/fetch_cef.sh` does **not guarantee**
decoding for Twitch's H.264/AAC streams. [Google's CEF build instructions](https://support.google.com/webdesigner/answer/10043691)
say the default CEF build disables proprietary audio/video codecs and require
`GN_DEFINES="ffmpeg_branding=Chrome proprietary_codecs=true"`. These are CEF
**source-build arguments**, not Aurora CMake definitions. No CMake link
library, `CefSettings` member, or Chromium runtime switch can add decoders to
an already-built `libcef`.

Aurora provides two ways to supply a complete matching CEF distribution:

1. Build it with `scripts/build_cef_codecs.sh`. Set `CEF_BRANCH` to the desired
   CEF release branch; the script follows [CEF's automated-build flow](https://bitbucket.org/chromiumembedded/cef/wiki/AutomatedBuildSetup.md),
   enables Chrome FFmpeg/proprietary codecs, creates a minimal binary
   distribution, and records its codec arguments in `codec-args.gn`.
2. Obtain an equivalent trusted binary distribution and add
   `CEF_ROOT/codec-args.gn` containing the exact arguments from that build:

   ```gn
   ffmpeg_branding = "Chrome"
   proprietary_codecs = true
   ```

Do not swap only the FFmpeg library or mix CEF versions: wrapper headers,
engine, resources, and packaged binaries must agree. Review codec
patent/licensing requirements before distributing such a build; Google's
instructions warn that licenses and fees may apply, and [CEF's codec discussion](https://github.com/chromiumembedded/cef/issues/3559)
explains the issue. This is not legal advice.

Example source build and Aurora build:

```sh
CEF_BRANCH=<release-branch> ./scripts/build_cef_codecs.sh
cmake -S . -B build -DAURORA_ENABLE_CEF=ON -DAURORA_BUILD_APP=ON \
  -DAURORA_REQUIRE_H264_AAC=ON \
  -DCEF_ROOT=$PWD/third_party/cef-codec
cmake --build build
```

`AURORA_REQUIRE_H264_AAC=ON` makes configuration fail unless the CEF root has
the recorded Chrome FFmpeg and proprietary-codec arguments. This prevents an
accidental stock-CEF release, but it is intentionally not a binary capability
test: only actual playback/`MediaSource` verification can prove the build.
Leave the option off for stock CEF development builds; Aurora logs that codec
support is unverified.

## Manual verification on a CEF-enabled desktop build

1. In DevTools on a Twitch tab, check the reported user agent contains
   `Chrome/<bundled Chromium version>`. Test
   `MediaSource.isTypeSupported('video/mp4; codecs="avc1.42E01E, mp4a.40.2"')`;
   `scripts/verify_media_codecs.js` contains the same check. If it is false,
   first check the bundled CEF distribution, not autoplay.
2. Open a live channel, a `/videos/<id>` VOD, a `clips.twitch.tv` clip, and
   `player.twitch.tv/?channel=<name>&parent=twitch.tv`; verify video **and**
   audio, seek on the VOD, and watch for player/network errors in DevTools.
3. Mute the Twitch tab, leave it in the background longer than the configured
   suspension interval, return, and verify playback continues. Confirm
   unrelated idle tabs can still suspend.
4. With Shields on, confirm stream manifest/segments load and unrelated
   blocked requests still appear in the Shields count. Test microphone/camera
   access separately to verify it still prompts; autoplay is not capture
   permission.

Twitch [officially supports certain current desktop browsers](https://help.twitch.tv/s/article/supported-browsers?language=en_US);
Aurora is not on that list. A real Twitch playback smoke test with the selected
CEF binary is necessary; unit tests cannot establish that a live stream plays.
