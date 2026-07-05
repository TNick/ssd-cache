#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssd_cache {

struct PathIdentity {
    std::wstring source_root_id;
    std::wstring relative_path;
};

std::wstring normalize_separators(std::wstring_view path);

std::wstring trim_leading_separators(std::wstring_view path);

std::wstring join_root_relative(
    std::wstring_view root,
    std::wstring_view relative_path
);

std::optional<PathIdentity> parse_unc_path(std::wstring_view path);

std::optional<PathIdentity> parse_windows_remote_device_path(
    std::wstring_view path
);

std::optional<PathIdentity> map_observed_source_path(
    std::wstring_view source_unc,
    std::wstring_view observed_path
);

std::vector<std::wstring> split_path_components(std::wstring_view path);

}  // namespace ssd_cache
