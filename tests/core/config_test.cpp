#include "test_macros.h"

#include <filesystem>
#include <fstream>

#include "ssd_cache/config.h"

namespace {

std::wstring config_path(const wchar_t* name) {
    auto path = std::filesystem::temp_directory_path();
    path /= name;
    std::filesystem::remove(path);
    return path.wstring();
}

}  // namespace

TEST_CASE("config defaults to ignoring explorer process") {
    const auto path = config_path(L"ssd-cache-config-default.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "source_unc=\\\\nas\\share\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(ssd_cache::process_matches_ignored_patterns(
        config,
        L"C:\\Windows\\explorer.exe"
    ));
}

TEST_CASE("config defaults to ignoring temporary paths") {
    const auto path = config_path(L"ssd-cache-config-temp-path-default.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "source_unc=\\\\nas\\share\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(ssd_cache::path_matches_ignored_patterns(
        config,
        L"Licitatii\\__gen_doc__\\~$GenerareDocumente.xlsm"
    ));
    REQUIRE(ssd_cache::path_matches_ignored_patterns(
        config,
        L"cache-index.sqlite3-wal"
    ));
    REQUIRE(ssd_cache::path_matches_ignored_patterns(
        config,
        L"sqlite3-shm"
    ));
    REQUIRE(ssd_cache::path_matches_ignored_patterns(config, L"db-wal"));
    REQUIRE(ssd_cache::path_matches_ignored_patterns(config, L"db-shm"));
    REQUIRE(ssd_cache::path_matches_ignored_patterns(
        config,
        L"cache-index.sqlite3-journal"
    ));
    REQUIRE(!ssd_cache::path_matches_ignored_patterns(
        config,
        L"Advaita\\Termene OCPI.pdf"
    ));
}

TEST_CASE("config parses persisted app mode") {
    const auto path = config_path(L"ssd-cache-config-mode.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "mode=monitor\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(config.mode.has_value());
    REQUIRE(*config.mode == ssd_cache::AppMode::Monitor);
}

TEST_CASE("config defaults min free space to 6 GiB") {
    const auto path = config_path(L"ssd-cache-config-minfree-default.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "source_unc=\\\\nas\\share\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(config.min_free_bytes == 6ULL * 1024 * 1024 * 1024);
}

TEST_CASE("config parses and round-trips min free space in MiB") {
    const auto path = config_path(L"ssd-cache-config-minfree.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "min_free_space_mb=2048\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);
    REQUIRE(config.min_free_bytes == 2048ULL * 1024 * 1024);

    const auto saved = config_path(L"ssd-cache-config-minfree-out.ini");
    ssd_cache::save_config_file(saved, config);
    const auto reloaded = ssd_cache::load_config_file(saved);
    REQUIRE(reloaded.min_free_bytes == 2048ULL * 1024 * 1024);
}

TEST_CASE("config explicit empty process filter disables default") {
    const auto path = config_path(L"ssd-cache-config-empty-filter.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "ignored_process_patterns=\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(config.ignored_process_patterns.empty());
    REQUIRE(!ssd_cache::process_matches_ignored_patterns(
        config,
        L"C:\\Windows\\explorer.exe"
    ));
}

TEST_CASE("config explicit empty path filter disables default") {
    const auto path = config_path(L"ssd-cache-config-empty-path-filter.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "ignored_path_patterns=\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(config.ignored_path_patterns.empty());
    REQUIRE(!ssd_cache::path_matches_ignored_patterns(
        config,
        L"Licitatii\\__gen_doc__\\~$GenerareDocumente.xlsm"
    ));
    REQUIRE(!ssd_cache::path_matches_ignored_patterns(config, L"db-wal"));
}

TEST_CASE("config parses process filter wildcard list") {
    const auto path = config_path(L"ssd-cache-config-filter.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "ignored_process_patterns=explorer.exe;*\\Search*.exe,Acrobat?.exe\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(ssd_cache::process_matches_ignored_patterns(
        config,
        L"C:\\Windows\\explorer.exe"
    ));
    REQUIRE(ssd_cache::process_matches_ignored_patterns(
        config,
        L"C:\\Program Files\\SearchHost.exe"
    ));
    REQUIRE(ssd_cache::process_matches_ignored_patterns(
        config,
        L"C:\\Program Files\\Acrobat1.exe"
    ));
    REQUIRE(!ssd_cache::process_matches_ignored_patterns(
        config,
        L"C:\\Program Files\\reader.exe"
    ));
}

TEST_CASE("config parses path filter wildcard list") {
    const auto path = config_path(L"ssd-cache-config-path-filter.ini");
    std::ofstream output(std::filesystem::path(path), std::ios::trunc);
    output << "ignored_path_patterns=*.tmp;temp\\*,~$*\n";
    output.close();

    const auto config = ssd_cache::load_config_file(path);

    REQUIRE(ssd_cache::path_matches_ignored_patterns(
        config,
        L"folder\\file.tmp"
    ));
    REQUIRE(ssd_cache::path_matches_ignored_patterns(
        config,
        L"temp\\generated.xlsx"
    ));
    REQUIRE(ssd_cache::path_matches_ignored_patterns(
        config,
        L"docs\\~$Report.docx"
    ));
    REQUIRE(!ssd_cache::path_matches_ignored_patterns(
        config,
        L"docs\\Report.docx"
    ));
}
