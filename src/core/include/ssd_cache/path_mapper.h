#pragma once

/**
 * @file
 * @brief Path normalization and source-path mapping helpers.
 *
 * Translates between the various path shapes seen at runtime (UNC paths, the
 * driver's remote-device paths) and the source-root / relative-path identity
 * the cache index keys on.
 */

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssd_cache {

/** A path split into its source root and the portion relative to that root. */
struct PathIdentity {
    std::wstring source_root_id;  /**< The root (e.g. a UNC share). */
    std::wstring relative_path;   /**< Path relative to source_root_id. */
};

/**
 * Normalizes path separators to a single canonical form.
 *
 * @param path Path to normalize.
 * @return The normalized path.
 */
std::wstring normalize_separators(std::wstring_view path);

/**
 * Removes leading path separators.
 *
 * @param path Path to trim.
 * @return The path without any leading separators.
 */
std::wstring trim_leading_separators(std::wstring_view path);

/**
 * Joins a root and a relative path into a single path.
 *
 * @param root Root portion (e.g. a UNC share or drive root).
 * @param relative_path Path relative to @p root.
 * @return The combined path with a single separator between the parts.
 */
std::wstring join_root_relative(
    std::wstring_view root,
    std::wstring_view relative_path
);

/**
 * Parses a UNC path into its share root and relative remainder.
 *
 * @param path A UNC path (e.g. \\\\server\\share\\dir\\file).
 * @return The parsed identity, or nullopt if @p path is not a UNC path.
 */
std::optional<PathIdentity> parse_unc_path(std::wstring_view path);

/**
 * Parses a Windows remote-device path (the form the driver reports).
 *
 * @param path A remote-device path.
 * @return The parsed identity, or nullopt if @p path is not such a path.
 */
std::optional<PathIdentity> parse_windows_remote_device_path(
    std::wstring_view path
);

/**
 * Maps a path observed by the driver onto the configured source root, yielding
 * the identity used to key the cache index.
 *
 * @param source_unc The configured source UNC to map against.
 * @param observed_path The path as observed by the driver.
 * @return The mapped identity, or nullopt if @p observed_path does not belong
 *         to @p source_unc.
 */
std::optional<PathIdentity> map_observed_source_path(
    std::wstring_view source_unc,
    std::wstring_view observed_path
);

/**
 * Splits a path into its individual components.
 *
 * @param path Path to split.
 * @return The path components in order, with separators removed.
 */
std::vector<std::wstring> split_path_components(std::wstring_view path);

}  // namespace ssd_cache
