#include "ssd_cache/path_mapper.h"

#include <algorithm>
#include <cwctype>

namespace ssd_cache {
namespace {

bool equals_ignore_case(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }

    return true;
}

bool starts_with_ignore_case(
    std::wstring_view value,
    std::wstring_view prefix
) {
    if (value.size() < prefix.size()) {
        return false;
    }

    return equals_ignore_case(value.substr(0, prefix.size()), prefix);
}

std::wstring lower_root(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

}  // namespace

std::wstring normalize_separators(std::wstring_view path) {
    std::wstring result(path);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
}

std::wstring trim_leading_separators(std::wstring_view path) {
    auto normalized = normalize_separators(path);
    while (!normalized.empty() && normalized.front() == L'\\') {
        normalized.erase(normalized.begin());
    }

    return normalized;
}

std::vector<std::wstring> split_path_components(std::wstring_view path) {
    std::vector<std::wstring> components;
    const auto normalized = normalize_separators(path);
    std::size_t start = 0;

    while (start < normalized.size()) {
        while (start < normalized.size() && normalized[start] == L'\\') {
            ++start;
        }

        std::size_t end = start;
        while (end < normalized.size() && normalized[end] != L'\\') {
            ++end;
        }

        if (end > start) {
            components.emplace_back(normalized.substr(start, end - start));
        }

        start = end;
    }

    return components;
}

std::wstring join_root_relative(
    std::wstring_view root,
    std::wstring_view relative_path
) {
    auto result = normalize_separators(root);
    auto relative = trim_leading_separators(relative_path);

    while (!result.empty() && result.back() == L'\\') {
        result.pop_back();
    }

    if (!relative.empty()) {
        result.push_back(L'\\');
        result.append(relative);
    }

    return result;
}

std::optional<PathIdentity> parse_unc_path(std::wstring_view path) {
    const auto normalized = normalize_separators(path);
    if (!starts_with_ignore_case(normalized, L"\\\\")) {
        return std::nullopt;
    }

    const auto components = split_path_components(normalized);
    if (components.size() < 2) {
        return std::nullopt;
    }

    std::wstring root = L"\\\\";
    root.append(components[0]);
    root.push_back(L'\\');
    root.append(components[1]);

    std::wstring relative;
    for (std::size_t index = 2; index < components.size(); ++index) {
        if (!relative.empty()) {
            relative.push_back(L'\\');
        }

        relative.append(components[index]);
    }

    return PathIdentity{lower_root(root), relative};
}

std::optional<PathIdentity> parse_windows_remote_device_path(
    std::wstring_view path
) {
    const auto components = split_path_components(path);
    if (components.size() < 4) {
        return std::nullopt;
    }

    if (!equals_ignore_case(components[0], L"Device")) {
        return std::nullopt;
    }

    if (!equals_ignore_case(components[1], L"LanManRedirector") &&
        !equals_ignore_case(components[1], L"Mup")) {
        return std::nullopt;
    }

    std::size_t server_index = 2;
    if (components[server_index].starts_with(L";")) {
        ++server_index;
    }

    if (components.size() <= server_index + 1) {
        return std::nullopt;
    }

    std::wstring root = L"\\\\";
    root.append(components[server_index]);
    root.push_back(L'\\');
    root.append(components[server_index + 1]);

    std::wstring relative;
    for (std::size_t index = server_index + 2; index < components.size(); ++index) {
        if (!relative.empty()) {
            relative.push_back(L'\\');
        }

        relative.append(components[index]);
    }

    return PathIdentity{lower_root(root), relative};
}

std::optional<PathIdentity> map_observed_source_path(
    std::wstring_view source_unc,
    std::wstring_view observed_path
) {
    const auto source_identity = parse_unc_path(source_unc);
    if (!source_identity) {
        return std::nullopt;
    }

    auto observed = parse_unc_path(observed_path);
    if (!observed) {
        observed = parse_windows_remote_device_path(observed_path);
    }

    if (!observed) {
        return std::nullopt;
    }

    if (!equals_ignore_case(
            observed->source_root_id,
            source_identity->source_root_id
        )) {
        return std::nullopt;
    }

    return observed;
}

}  // namespace ssd_cache
