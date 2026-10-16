// Aurora — the engine's extension load list.
//
// Chromium loads unpacked extensions at engine startup from the browser
// process's `--load-extension=dir1,dir2,...` switch (CefLifecycle.cpp appends
// it in OnBeforeCommandLineProcessing). This file is the pure logic between
// the registry (ExtensionService::enabled_paths()) and that switch: which
// registered directories are actually loadable, and the exact switch value.
//
// It lives in the core library (no Qt, no CEF) so it is unit-tested in
// tests/test_extension_load_list.cpp; CEF itself cannot run in CI.
//
// Scope, stated honestly (see docs/extensions.md):
//   * Only unpacked directories are loaded. There is no Chrome Web Store
//     install path and no CRX signature verification (install_from_file()
//     unpacks a .crx without verifying its signature).
//   * Extensions get exactly the chrome.* APIs stock Chromium wires up for
//     the way Aurora embeds it (CEF Chrome bootstrap, Alloy-style browsers).
//   * The list is read once, at engine startup, so changes need a restart.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::extensions {

// Why a registered directory was left out of the load list.
enum class LoadSkipReason {
    EmptyPath,      // registry row with no path
    ContainsComma,  // ',' is the switch's separator; cannot be expressed
    Duplicate,      // same directory listed twice
    NotLoadable,    // directory (or its manifest.json) no longer exists
};

std::string_view load_skip_reason_name(LoadSkipReason reason);

struct SkippedExtension {
    std::string path;
    LoadSkipReason reason = LoadSkipReason::EmptyPath;
};

struct ExtensionLoadList {
    std::vector<std::string> dirs;          // in registry order, loadable
    std::vector<SkippedExtension> skipped;  // everything left out, with why

    // The value for Chromium's --load-extension switch: `dirs` joined with
    // ','. Empty when there is nothing to load — callers must then not append
    // the switch at all.
    std::string switch_value() const;
};

// Predicate deciding whether a directory can be handed to Chromium. Injected
// so the filtering rules are testable without touching the filesystem.
using LoadableDirPredicate = std::function<bool(const std::string& dir)>;

// Build the load list from enabled registry paths. Order is preserved; empty
// paths, paths containing a comma and exact duplicates are dropped first, then
// `is_loadable` is asked about each remaining directory. A null predicate
// accepts every directory (useful when the caller already filtered).
ExtensionLoadList build_load_list(const std::vector<std::string>& enabled_paths,
                                  const LoadableDirPredicate& is_loadable);

// The default, filesystem-backed predicate: `dir` is an existing directory
// containing a regular manifest.json. Touches the filesystem — call it only
// from the pre-UI startup path or a worker thread, never from the UI thread.
bool is_loadable_extension_dir(const std::string& dir);

// Join already-validated directories into a --load-extension value. Applies
// the same cheap, filesystem-free rules as build_load_list() (drops empty
// paths, comma-containing paths and duplicates), so the CEF layer can never
// emit a corrupt switch even if handed an unfiltered list.
std::string load_extension_switch_value(const std::vector<std::string>& dirs);

} // namespace aurora::extensions
