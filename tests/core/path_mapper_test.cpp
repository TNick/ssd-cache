#include "test_macros.h"

#include "ssd_cache/path_mapper.h"

TEST_CASE("parse UNC paths into root and relative path") {
    const auto identity = ssd_cache::parse_unc_path(
        L"\\\\NasBox\\Share\\folder\\file.txt"
    );

    REQUIRE(identity.has_value());
    REQUIRE(identity->source_root_id == L"\\\\nasbox\\share");
    REQUIRE(identity->relative_path == L"folder\\file.txt");
}

TEST_CASE("parse LanManRedirector paths into root and relative path") {
    const auto identity = ssd_cache::parse_windows_remote_device_path(
        L"\\Device\\LanManRedirector\\;N:000000000000b09f\\NasBox\\Share\\a\\b.txt"
    );

    REQUIRE(identity.has_value());
    REQUIRE(identity->source_root_id == L"\\\\nasbox\\share");
    REQUIRE(identity->relative_path == L"a\\b.txt");
}

TEST_CASE("map observed paths only under configured source UNC") {
    const auto identity = ssd_cache::map_observed_source_path(
        L"\\\\NasBox\\Share",
        L"\\\\nasbox\\share\\folder\\file.txt"
    );

    REQUIRE(identity.has_value());
    REQUIRE(identity->relative_path == L"folder\\file.txt");

    const auto other = ssd_cache::map_observed_source_path(
        L"\\\\NasBox\\Share",
        L"\\\\other\\share\\folder\\file.txt"
    );
    REQUIRE(!other.has_value());
}
