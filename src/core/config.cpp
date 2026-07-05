#include "ssd_cache/config.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "ssd_cache/utf.h"

namespace ssd_cache {
namespace {

std::wstring trim(std::wstring value) {
    const auto not_space = [](wchar_t ch) {
        return std::iswspace(ch) == 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), not_space).base(),
        value.end()
    );
    return value;
}

std::map<std::wstring, std::wstring> read_key_values(const std::wstring& path) {
    std::ifstream input{std::filesystem::path(path)};
    if (!input) {
        return {};
    }

    std::map<std::wstring, std::wstring> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        values.emplace(
            trim(utf8_to_wide(std::string_view(line).substr(0, pos))),
            trim(utf8_to_wide(std::string_view(line).substr(pos + 1)))
        );
    }

    return values;
}

wchar_t read_letter(
    const std::map<std::wstring, std::wstring>& values,
    const std::wstring& key,
    wchar_t fallback
) {
    const auto iter = values.find(key);
    if (iter == values.end() || iter->second.empty()) {
        return fallback;
    }

    return static_cast<wchar_t>(std::towupper(iter->second.front()));
}

}  // namespace

std::wstring cache_root_from_config(const AppConfig& config) {
    std::wstring root;
    root.push_back(config.cache_letter);
    root.append(L":\\");
    return root;
}

std::wstring source_presentation_root_from_config(const AppConfig& config) {
    std::wstring root;
    root.push_back(config.source_presentation_letter);
    root.append(L":\\");
    return root;
}

std::wstring app_mode_to_wstring(AppMode mode) {
    switch (mode) {
        case AppMode::Disabled:
            return L"disabled";
        case AppMode::Monitor:
            return L"monitor";
        case AppMode::Serve:
            return L"serve";
    }

    throw std::invalid_argument("unknown app mode");
}

AppMode app_mode_from_wstring(const std::wstring& value) {
    auto lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });

    if (lowered == L"disabled") {
        return AppMode::Disabled;
    }

    if (lowered == L"monitor") {
        return AppMode::Monitor;
    }

    if (lowered == L"serve") {
        return AppMode::Serve;
    }

    throw std::invalid_argument("unknown app mode string");
}

AppConfig load_config_file(const std::wstring& path) {
    const auto values = read_key_values(path);
    AppConfig config;

    if (const auto iter = values.find(L"source_unc"); iter != values.end()) {
        config.source_unc = iter->second;
    }

    if (const auto iter = values.find(L"sqlite_path"); iter != values.end()) {
        config.sqlite_path = iter->second;
    }

    if (const auto iter = values.find(L"copy_delay_seconds"); iter != values.end()) {
        config.copy_delay = std::chrono::seconds(std::stoll(iter->second));
    }

    if (const auto iter = values.find(L"compare_hash_before_overwrite");
        iter != values.end()) {
        config.compare_hash_before_overwrite =
            iter->second == L"1" || iter->second == L"true";
    }

    config.source_presentation_letter =
        read_letter(values, L"source_presentation_letter", L'N');
    config.cache_letter = read_letter(values, L"cache_letter", L'K');
    return config;
}

void save_config_file(const std::wstring& path, const AppConfig& config) {
    const std::filesystem::path config_path(path);
    if (config_path.has_parent_path()) {
        std::filesystem::create_directories(config_path.parent_path());
    }

    std::ofstream output(config_path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write config file");
    }

    output << "source_unc=" << wide_to_utf8(config.source_unc) << "\n";
    output << "source_presentation_letter="
           << narrow_ascii(std::wstring(1, config.source_presentation_letter))
           << "\n";
    output << "cache_letter="
           << narrow_ascii(std::wstring(1, config.cache_letter))
           << "\n";
    output << "sqlite_path=" << wide_to_utf8(config.sqlite_path) << "\n";
    output << "copy_delay_seconds=" << config.copy_delay.count() << "\n";
    output << "compare_hash_before_overwrite="
           << (config.compare_hash_before_overwrite ? "1" : "0") << "\n";
}

}  // namespace ssd_cache
