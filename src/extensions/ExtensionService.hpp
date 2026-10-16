// Aurora — Chrome extension registry.
//
// Aurora's engine is Chromium (through CEF), so it can run real Chrome
// extensions. What this service owns is the *registry*: the list of unpacked
// extension directories the user installed, their manifest metadata, and
// whether each one is enabled. At startup the enabled paths are handed to the
// engine (Chromium loads them itself, exactly as `--load-extension` does in
// Chrome), which is why this file contains no CEF or Qt code and is fully
// unit-testable.
//
// Deliberate scope, stated honestly:
//   * Unpacked extensions (a folder containing manifest.json) are supported.
//     Web-Store .crx installation is not — it needs Chrome's signature
//     verification and update machinery, which CEF does not expose.
//   * Enabling or removing an extension takes effect on the next launch,
//     because Chromium reads the load list during engine startup.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/Database.hpp"

namespace aurora::extensions {

// Metadata read out of a manifest.json.
struct Manifest {
    std::string name;
    std::string version;
    std::string description;
    int manifest_version = 0;
    // Icon path as written in the manifest, RELATIVE to the extension folder
    // (e.g. "images/icon128.png"). The largest entry of the top-level "icons"
    // map, else the action's default_icon; empty when the manifest ships
    // neither. Use icon_file_for_dir() to turn it into a path on disk.
    std::string icon_path;
    // Relative options page (options_ui.page or options_page). Empty when the
    // manifest declares neither; callers then fall back to options.html.
    std::string options_page;
    // Manifest "key" (base64 DER public key). When present it is what Chromium
    // hashes to produce the chrome-extension:// id.
    std::string public_key;
};

// The outcome of install_from_file(): a packaged extension was installed, was
// recognized but could not be unpacked safely, or was not a .crx at all.
enum class CrxInstallResult {
    Installed,
    NotACrx,       // not a well-formed CRX package (wrong magic, truncated, ...)
    Unsupported,   // recognized, but its ZIP features/content are unsupported
};

struct CrxInstall {
    CrxInstallResult result = CrxInstallResult::NotACrx;
    int format_version = 0;  // 2 or 3 when recognized as a CRX package
    // Human-readable explanation, suitable for a dialog body.
    std::string detail;
};

// A registered extension.
struct Extension {
    int64_t id = 0;
    std::string path;          // absolute path of the unpacked directory
    std::string name;
    std::string version;
    std::string description;
    int manifest_version = 0;
    bool enabled = true;
    // Absolute path of the extension's own icon file on disk, or empty when
    // the manifest declares none / the file is missing. Only filled by
    // all_detailed(), which is the call the UI uses.
    std::string icon_file;
};

// Parse the fields Aurora needs out of a manifest.json. Returns nullopt when
// the text is not an object with a usable "name" (the one field Chrome also
// requires). Intentionally a narrow, allocation-light reader for four known
// keys rather than a general JSON parser: it must never be a parsing attack
// surface for extension-supplied text.
std::optional<Manifest> parse_manifest(std::string_view json);

// Read and parse `<dir>/manifest.json`. Returns nullopt when the directory has
// no readable manifest.
std::optional<Manifest> read_manifest_dir(std::string_view dir);

// Resolve a manifest's relative icon_path against the extension directory and
// return it only if it really is a regular file inside that directory. Returns
// an empty string otherwise, so a manifest pointing at "../../secrets.png"
// (or at nothing) simply yields no icon.
std::string icon_file_for_dir(std::string_view dir, const Manifest& manifest);

class ExtensionService {
public:
    explicit ExtensionService(storage::Database& db) : db_(db) {}

    // Register the unpacked extension in `dir`. Reads its manifest, rejects a
    // directory without one, and is idempotent on path (re-adding refreshes
    // the stored metadata instead of duplicating the row). Returns the row id,
    // or 0 on failure.
    int64_t add_unpacked(std::string_view dir);

    // Attempt to install a packaged extension file (.crx / .crx3).
    //
    // Validates CRX2/CRX3 framing, extracts the ZIP payload into the profile's
    // managed `extensions` directory, validates manifest.json, then registers
    // that unpacked directory. Extraction rejects traversal, links, encrypted
    // entries, unsupported compression and excessive expanded sizes. The
    // package signature is not re-verified because CEF exposes no Chrome Web
    // Store verifier; callers should only route packages from a trusted store.
    // The function never throws, and it never opens a URL (in-app or
    // external) — registration is the only side effect.
    CrxInstall install_from_file(std::string_view path);

    // Every registered extension, ordered by name. Registry rows only: no
    // filesystem access, because the engine-startup path (enabled_paths())
    // goes through here.
    std::vector<Extension> all() const;

    // Every registered extension with its metadata re-read from the extension
    // folder: the manifest's current name/version/description (the user may
    // have edited or updated the extension since it was registered) plus the
    // resolved icon file. This is what the extensions UI shows, so a card can
    // display the extension's real name and artwork instead of a stand-in.
    // Costs one manifest read per extension, which is why it is a separate
    // call from all().
    std::vector<Extension> all_detailed() const;

    // Enabled extensions' directories, in registry order. At startup this is
    // filtered by extensions::build_load_list() (ExtensionLoadList.hpp: drops
    // folders that no longer exist) and handed to the engine.
    std::vector<std::string> enabled_paths() const;

    bool set_enabled(int64_t id, bool enabled);

    // The registry row for `id`, or nullopt when there is none. Registry
    // fields only (no manifest re-read), so it is cheap enough to call from a
    // UI handler.
    std::optional<Extension> find(int64_t id) const;

    // Fully-qualified chrome-extension:// URL for this extension's options
    // page (manifest options_ui / options_page, else options.html). Empty when
    // `id` is unknown. Callers must open this in-app — never via the OS browser.
    std::string extension_options_url(int64_t id) const;

    // What uninstall() did, so the UI can tell the user the truth instead of
    // a fixed "its files were left on disk" sentence that is wrong half the
    // time.
    struct Removal {
        bool ok = false;            // the registry row is gone
        bool files_deleted = false; // Aurora also deleted the unpacked folder
        std::string path;           // the folder the extension lived in
        std::string name;
    };

    // Remove an extension from the registry and, when Aurora owns its files
    // (a .crx Aurora unpacked into <profile>/extensions/...), delete them too.
    //
    // Deleting the managed copy is what makes "Remove" actually remove: the
    // engine's load list is rebuilt from this registry at startup, but a
    // package left behind in the profile is both dead weight and a folder a
    // later reinstall would silently reuse. A folder the USER pointed at with
    // "Load unpacked..." is never touched — it is their source tree.
    Removal uninstall(int64_t id);

    // Forget an extension. Thin wrapper over uninstall() for callers that only
    // care whether the row is gone.
    bool remove(int64_t id);

    // Absolute path of the directory Aurora unpacks packaged extensions into
    // (<profile>/extensions). Empty when the profile location is unknown (an
    // in-memory registry in tests).
    std::string managed_root() const;

private:
    storage::Database& db_;

    // Drop a cached manifest so a later all_detailed() cannot resurrect the
    // metadata of an extension that was removed (or re-read a folder that was
    // deleted and recreated within one filesystem timestamp tick).
    void forget_cached_manifest(const std::string& path) const;

    // Manifest cache: all_detailed() is called on every UI refresh, and reading
    // every extension's manifest.json from disk each time is slow when there are
    // many extensions. This cache stores the parsed manifest keyed by the
    // extension directory path, along with the file's last-modification time.
    // A manifest is only re-read when its mtime changes, so an unchanged set of
    // extensions costs zero disk reads on refresh.
    struct ManifestCacheEntry {
        std::filesystem::file_time_type mtime;
        Manifest manifest;
    };
    mutable std::map<std::string, ManifestCacheEntry> manifest_cache_;
    mutable std::mutex cache_mutex_;
};

} // namespace aurora::extensions
