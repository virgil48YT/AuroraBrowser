#include "updates/ReleaseVersion.hpp"

#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace aurora::updates {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

bool parse_number(std::string_view text, int& value) {
    if (text.empty() || text.size() > 9) return false;
    int result = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        result = result * 10 + (c - '0');
    }
    value = result;
    return true;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(separator, start);
        parts.push_back(text.substr(
            start, at == std::string_view::npos ? std::string_view::npos
                                                : at - start));
        if (at == std::string_view::npos) break;
        start = at + 1;
    }
    return parts;
}

bool valid_identifier(std::string_view text) {
    if (text.empty()) return false;
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!digit && !alpha && c != '-') return false;
    }
    return true;
}

int compare_prerelease(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;   // release > prerelease
    if (b.empty()) return -1;
    const auto left = split(a, '.');
    const auto right = split(b, '.');
    const std::size_t count = left.size() < right.size() ? left.size()
                                                         : right.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view one = left[i];
        const std::string_view two = right[i];
        const bool one_numeric = !one.empty() &&
                                 one.find_first_not_of("0123456789") ==
                                     std::string_view::npos;
        const bool two_numeric = !two.empty() &&
                                 two.find_first_not_of("0123456789") ==
                                     std::string_view::npos;
        if (one_numeric && two_numeric) {
            int left_value = 0;
            int right_value = 0;
            if (!parse_number(one, left_value) ||
                !parse_number(two, right_value)) {
                return 0;
            }
            if (left_value != right_value) {
                return left_value < right_value ? -1 : 1;
            }
            continue;
        }
        if (one_numeric != two_numeric) return one_numeric ? -1 : 1;
        if (one != two) return one < two ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

} // namespace

std::optional<Version> parse_version(std::string_view text) {
    text = trim(text);
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty()) return std::nullopt;
    const std::size_t plus = text.find('+');
    if (plus != std::string_view::npos) {
        const std::string_view build = text.substr(plus + 1);
        text = text.substr(0, plus);
        if (build.empty()) return std::nullopt;
    }
    const std::size_t dash = text.find('-');
    std::string_view prerelease;
    if (dash != std::string_view::npos) {
        prerelease = text.substr(dash + 1);
        text = text.substr(0, dash);
        if (prerelease.empty()) return std::nullopt;
        for (const std::string_view identifier : split(prerelease, '.')) {
            if (!valid_identifier(identifier)) return std::nullopt;
        }
    }
    const auto core = split(text, '.');
    if (core.empty() || core.size() > 3) return std::nullopt;
    Version version;
    int* const fields[3] = {&version.major, &version.minor, &version.patch};
    for (std::size_t i = 0; i < core.size(); ++i) {
        if (!parse_number(core[i], *fields[i])) return std::nullopt;
    }
    if (!prerelease.empty()) {
        version.prerelease.assign(prerelease.data(), prerelease.size());
    }
    return version;
}

int compare_versions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return compare_prerelease(a.prerelease, b.prerelease);
}

bool is_newer_version(std::string_view candidate, std::string_view installed) {
    const auto parsed_candidate = parse_version(candidate);
    const auto parsed_installed = parse_version(installed);
    if (!parsed_candidate || !parsed_installed) return false;
    return compare_versions(*parsed_candidate, *parsed_installed) > 0;
}

} // namespace aurora::updates
