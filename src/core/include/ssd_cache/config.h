#pragma once

/**
 * @file
 * @brief Application configuration model, filters and load/save helpers.
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ssd_cache {

/** Operating mode of the cache. */
enum class AppMode {
    Disabled,  /**< No active caching. */
    Monitor,   /**< Source share online; cache populated from observed access. */
    Serve,     /**< Source offline; cache volume presented at the source letter. */
};

/** Machine configuration, loaded from and saved to the INI-style config file. */
struct AppConfig {
    /** UNC of the source share to cache (e.g. \\\\nas\\share). */
    std::wstring source_unc;
    /** Drive letter the source is presented at to the user. */
    wchar_t source_presentation_letter = L'N';
    /** Drive letter the cache SSD is mounted at. */
    wchar_t cache_letter = L'K';
    /** Path to the SQLite cache-index database. */
    std::wstring sqlite_path;
    /** Persisted mode; nullopt when it should be inferred from drive layout. */
    std::optional<AppMode> mode;
    /** Delay between observing an access and copying the file. */
    std::chrono::seconds copy_delay{60};
    /** Compare hashes before overwriting an equally sized cache file. */
    bool compare_hash_before_overwrite = false;
    /**
     * Minimum free space to keep on the cache volume. Before caching a file the
     * service evicts the least recently accessed cached files until free space
     * reaches at least max(incoming file size, this threshold). Default 6 GiB.
     */
    std::uint64_t min_free_bytes = 6ULL * 1024 * 1024 * 1024;
    /** Process image patterns whose accesses are ignored. */
    std::vector<std::wstring> ignored_process_patterns{L"explorer.exe"};
    /** Relative-path patterns that are never cached. */
    std::vector<std::wstring> ignored_path_patterns{
        L"~$*",
        L"*-wal",
        L"*-shm",
        L"*-journal",
    };
};

/**
 * @param config Configuration providing the cache drive letter.
 * @return The cache volume root (e.g. "K:\\").
 */
std::wstring cache_root_from_config(const AppConfig& config);

/**
 * @param config Configuration providing the source presentation letter.
 * @return The source presentation volume root (e.g. "N:\\").
 */
std::wstring source_presentation_root_from_config(const AppConfig& config);

/**
 * Tests a process image path against the ignored-process patterns.
 *
 * @param config Configuration holding the patterns.
 * @param process_path Process image path to test.
 * @return True if @p process_path matches any ignored-process pattern.
 */
bool process_matches_ignored_patterns(
    const AppConfig& config,
    std::wstring_view process_path
);

/**
 * Tests a relative path against the ignored-path patterns.
 *
 * @param config Configuration holding the patterns.
 * @param relative_path Relative path to test.
 * @return True if @p relative_path matches any ignored-path pattern.
 */
bool path_matches_ignored_patterns(
    const AppConfig& config,
    std::wstring_view relative_path
);

/**
 * Converts a mode to its persisted string form.
 *
 * @param mode The mode to convert.
 * @return The canonical name ("disabled", "monitor" or "serve").
 */
std::wstring app_mode_to_wstring(AppMode mode);

/**
 * Parses a mode from its persisted string form.
 *
 * @param value The string to parse.
 * @return The parsed mode.
 * @throws std::invalid_argument if @p value is not a known mode name.
 */
AppMode app_mode_from_wstring(const std::wstring& value);

/**
 * Loads configuration from a file, applying defaults for absent keys.
 *
 * @param path Path to the config file.
 * @return The loaded configuration.
 */
AppConfig load_config_file(const std::wstring& path);

/**
 * Saves configuration to a file, creating parent directories as needed.
 *
 * @param path Path to write the config file to.
 * @param config Configuration to persist.
 */
void save_config_file(const std::wstring& path, const AppConfig& config);

}  // namespace ssd_cache
