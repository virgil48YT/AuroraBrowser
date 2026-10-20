// Aurora — built-in Tor (bundled tor daemon), the engine-independent part.
//
// Like Brave's "New Private Window with Tor", Aurora ships a tor binary (the
// Tor Project's "Tor Expert Bundle", fetched by scripts/fetch_tor.sh and
// copied next to the browser as tor/) and starts it on demand. The Qt side
// (ui/TorLauncher) runs the process; this file holds everything that can be
// unit-tested without Qt or CEF:
//
//   * where the bundled binary and its GeoIP data live in an installed
//     package (Windows/Linux folder layout and the macOS .app bundle);
//   * the torrc Aurora generates (client only, SOCKS on loopback, tied to
//     the browser's PID so tor exits when Aurora does, even on a crash);
//   * parsing tor's "Bootstrapped NN% (tag): summary" log lines;
//   * the Chromium proxy string for a Tor window's private request context.
//
// No Qt, no CEF.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::tor {

// File name of the tor executable on this platform.
std::string_view tor_executable_name();

// Where a packaged Aurora keeps the bundled tor, most specific first:
//   <app_dir>/tor/tor/tor[.exe]   (Tor Expert Bundle layout, all platforms)
//   <app_dir>/tor/tor[.exe]       (flattened copy)
//   <app_dir>/../Resources/tor/tor/tor   (macOS: Contents/MacOS -> Resources)
// `app_dir` is the directory holding the browser executable.
std::vector<std::filesystem::path> bundled_tor_candidates(
    const std::filesystem::path& app_dir);

// The first candidate that exists as a regular file, if any.
std::optional<std::filesystem::path> find_bundled_tor(
    const std::filesystem::path& app_dir);

// The Expert Bundle's data/ directory (geoip, geoip6) for a tor binary found
// by find_bundled_tor(): <bundle>/data next to <bundle>/tor/.
std::filesystem::path bundled_data_dir(const std::filesystem::path& tor_binary);

struct TorrcOptions {
    uint16_t socks_port = 0;              // 0 = let tor pick a free port ("auto")
    std::filesystem::path data_directory; // tor state (consensus cache, keys)
    std::filesystem::path geoip_file;     // optional
    std::filesystem::path geoip6_file;    // optional
    int64_t owning_pid = 0;               // tor exits when this PID does (0 = off)
};

// The torrc Aurora writes for its bundled tor. Client only (never a relay or
// exit), SOCKS bound to 127.0.0.1 only, no control port (nothing else needs
// to talk to it), log to stdout so the launcher can follow bootstrap
// progress (and, with socks_port 0, the port tor picked).
std::string build_torrc(const TorrcOptions& options);

struct BootstrapProgress {
    int percent = 0;     // 0..100
    std::string tag;     // e.g. "loading_descriptors", "done"
    std::string summary; // e.g. "Loading relay descriptors"
};

// Parses one line of tor's notice log, e.g.
//   "Oct 20 10:45:20.000 [notice] Bootstrapped 75% (enough_dirinfo): Loaded…"
// Returns nothing for any other line.
std::optional<BootstrapProgress> parse_bootstrap_line(std::string_view line);

// Parses tor's "Opened Socks listener connection (ready) on 127.0.0.1:PORT"
// notice, which reports the port when the torrc asked for "auto".
std::optional<uint16_t> parse_socks_listener_line(std::string_view line);

// "socks5://127.0.0.1:PORT": the proxy server string for the Tor window's
// private CEF request context. Chromium sends host names (not resolved
// addresses) to a SOCKS5 proxy, so DNS happens inside Tor.
std::string socks_proxy_url(uint16_t port);

// True for a .onion host (with or without subdomains), case-insensitive.
bool is_onion_host(std::string_view host);

} // namespace aurora::tor
