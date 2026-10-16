#include "extensions/ExtensionLoadList.hpp"

#include <filesystem>
#include <set>
#include <system_error>

namespace aurora::extensions {

std::string_view load_skip_reason_name(LoadSkipReason reason) {
    switch (reason) {
        case LoadSkipReason::EmptyPath: return "empty path";
        case LoadSkipReason::ContainsComma:
            return "path contains a comma (the --load-extension separator)";
        case LoadSkipReason::Duplicate: return "duplicate path";
        case LoadSkipReason::NotLoadable:
            return "folder or manifest.json no longer exists";
    }
    return "unknown";
}

std::string ExtensionLoadList::switch_value() const {
    std::string joined;
    for (const auto& dir : dirs) {
        if (!joined.empty()) joined.push_back(',');
        joined += dir;
    }
    return joined;
}

ExtensionLoadList build_load_list(const std::vector<std::string>& enabled_paths,
                                  const LoadableDirPredicate& is_loadable) {
    ExtensionLoadList out;
    std::set<std::string> seen;
    for (const auto& path : enabled_paths) {
        if (path.empty()) {
            out.skipped.push_back({path, LoadSkipReason::EmptyPath});
            continue;
        }
        if (path.find(',') != std::string::npos) {
            out.skipped.push_back({path, LoadSkipReason::ContainsComma});
            continue;
        }
        if (!seen.insert(path).second) {
            out.skipped.push_back({path, LoadSkipReason::Duplicate});
            continue;
        }
        if (is_loadable && !is_loadable(path)) {
            out.skipped.push_back({path, LoadSkipReason::NotLoadable});
            continue;
        }
        out.dirs.push_back(path);
    }
    return out;
}

bool is_loadable_extension_dir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root(dir);
    if (!fs::is_directory(root, ec) || ec) return false;
    return fs::is_regular_file(root / "manifest.json", ec) && !ec;
}

std::string load_extension_switch_value(const std::vector<std::string>& dirs) {
    return build_load_list(dirs, nullptr).switch_value();
}

} // namespace aurora::extensions
