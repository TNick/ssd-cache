#pragma once

#include <string>
#include <string_view>

namespace ssd_cache {

std::string wide_to_utf8(std::wstring_view value);

std::wstring utf8_to_wide(std::string_view value);

std::wstring widen_ascii(std::string_view value);

std::string narrow_ascii(std::wstring_view value);

}  // namespace ssd_cache
