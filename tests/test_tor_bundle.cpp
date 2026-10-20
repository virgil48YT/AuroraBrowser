// Built-in Tor: bundled-binary lookup, generated torrc, log parsing.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "tor/TorBundle.hpp"

using namespace aurora::tor;
namespace fs = std::filesystem;

TEST_CASE("bootstrap lines from tor's notice log are parsed") {
    auto p = parse_bootstrap_line(
        "Oct 20 10:45:20.000 [notice] Bootstrapped 75% (enough_dirinfo): "
        "Loaded enough directory info to build circuits");
    REQUIRE(p.has_value());
    CHECK(p->percent == 75);
    CHECK(p->tag == "enough_dirinfo");
    CHECK(p->summary == "Loaded enough directory info to build circuits");

    auto done = parse_bootstrap_line("Oct 20 10:45:22.000 [notice] Bootstrapped 100% (done): Done");
    REQUIRE(done.has_value());
    CHECK(done->percent == 100);
    CHECK(done->tag == "done");

    auto zero = parse_bootstrap_line("[notice] Bootstrapped 0% (starting): Starting\r");
    REQUIRE(zero.has_value());
    CHECK(zero->percent == 0);
    CHECK(zero->summary == "Starting");

    CHECK_FALSE(parse_bootstrap_line("[notice] Opening Socks listener on 127.0.0.1:0").has_value());
    CHECK_FALSE(parse_bootstrap_line("Bootstrapped lots").has_value());
    CHECK_FALSE(parse_bootstrap_line("Bootstrapped 250% (x): y").has_value());
}

TEST_CASE("the SOCKS port tor picked is read from its listener notice") {
    auto port = parse_socks_listener_line(
        "Oct 20 10:47:05.713 [notice] Opened Socks listener connection (ready) on 127.0.0.1:38341");
    REQUIRE(port.has_value());
    CHECK(*port == 38341);
    CHECK_FALSE(parse_socks_listener_line("[notice] Opening Socks listener on 127.0.0.1:0").has_value());
    CHECK_FALSE(parse_socks_listener_line("[notice] Bootstrapped 5% (conn): x").has_value());
}

TEST_CASE("generated torrc is client-only, loopback-only and tied to the browser") {
    TorrcOptions o;
    o.socks_port = 0;
    o.data_directory = fs::path("/home/jane doe/.local/share/Aurora/tor");
    o.geoip_file = fs::path("/opt/aurora/tor/data/geoip");
    o.owning_pid = 4242;
    const std::string t = build_torrc(o);
    CHECK(t.find("ClientOnly 1\n") != std::string::npos);
    CHECK(t.find("SocksPort 127.0.0.1:auto\n") != std::string::npos);
    CHECK(t.find("ControlPort 0\n") != std::string::npos);
    CHECK(t.find("Log notice stdout\n") != std::string::npos);
    CHECK(t.find("DataDirectory \"/home/jane doe/.local/share/Aurora/tor\"\n") != std::string::npos);
    CHECK(t.find("GeoIPFile \"/opt/aurora/tor/data/geoip\"\n") != std::string::npos);
    CHECK(t.find("GeoIPv6File") == std::string::npos);  // not given -> not written
    CHECK(t.find("__OwningControllerProcess 4242\n") != std::string::npos);

    o.socks_port = 9150;
    o.owning_pid = 0;
    const std::string fixed = build_torrc(o);
    CHECK(fixed.find("SocksPort 127.0.0.1:9150\n") != std::string::npos);
    CHECK(fixed.find("__OwningControllerProcess") == std::string::npos);
}

TEST_CASE("torrc paths keep Windows backslashes") {
    TorrcOptions o;
    o.data_directory = fs::path("C:\\Users\\Jane\\AppData\\Local\\Aurora\\tor");
    CHECK(build_torrc(o).find("DataDirectory \"C:\\\\Users\\\\Jane\\\\AppData\\\\Local\\\\Aurora\\\\tor\"")
          != std::string::npos);
}

TEST_CASE("bundled tor is found in the Expert Bundle layout next to the browser") {
    const fs::path root = fs::temp_directory_path() / "aurora_tor_bundle_test";
    fs::remove_all(root);
    const fs::path app = root / "app";
    fs::create_directories(app / "tor" / "tor");
    fs::create_directories(app / "tor" / "data");
    CHECK_FALSE(find_bundled_tor(app).has_value());

    const fs::path exe = app / "tor" / "tor" / std::string(tor_executable_name());
    { std::ofstream(exe) << "#!"; }
    auto found = find_bundled_tor(app);
    REQUIRE(found.has_value());
    CHECK(*found == exe);
    CHECK(bundled_data_dir(*found) == app / "tor" / "data");

    // macOS: Contents/MacOS/<browser> with tor in Contents/Resources.
    const fs::path contents = root / "Aurora.app" / "Contents";
    fs::create_directories(contents / "MacOS");
    fs::create_directories(contents / "Resources" / "tor" / "tor");
    const fs::path mac_exe = contents / "Resources" / "tor" / "tor" / std::string(tor_executable_name());
    { std::ofstream(mac_exe) << "#!"; }
    auto mac = find_bundled_tor(contents / "MacOS");
    REQUIRE(mac.has_value());
    CHECK(*mac == mac_exe);
    fs::remove_all(root);
}

TEST_CASE("Tor proxy string and onion hosts") {
    CHECK(socks_proxy_url(9050) == "socks5://127.0.0.1:9050");
    CHECK(is_onion_host("duckduckgogg42xjoc72x3sjasowoarfbgcmvfimaftt6twagswzczad.onion"));
    CHECK(is_onion_host("www.example.ONION."));
    CHECK_FALSE(is_onion_host("onion"));
    CHECK_FALSE(is_onion_host(".onion"));
    CHECK_FALSE(is_onion_host("onion.example.com"));
}
