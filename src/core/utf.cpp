/**
 * @file
 * @brief Implementation of the UTF-8/UTF-16 and ASCII conversion helpers.
 *
 * On Windows the WideChar/MultiByte APIs are used; elsewhere the standard
 * codecvt facet is used as a portable fallback.
 */

#include "ssd_cache/utf.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#endif

namespace ssd_cache {

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

#ifdef _WIN32
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-16 to UTF-8");
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr
    );
    if (written != size) {
        throw std::runtime_error("failed to write UTF-8 string");
    }

    return result;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.to_bytes(value.data(), value.data() + value.size());
#endif
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }

#ifdef _WIN32
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-8 to UTF-16");
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size
    );
    if (written != size) {
        throw std::runtime_error("failed to write UTF-16 string");
    }

    return result;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.from_bytes(value.data(), value.data() + value.size());
#endif
}

std::wstring widen_ascii(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    std::transform(value.begin(), value.end(), std::back_inserter(result), [](char ch) {
        return static_cast<wchar_t>(static_cast<unsigned char>(ch));
    });
    return result;
}

std::string narrow_ascii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    std::transform(value.begin(), value.end(), std::back_inserter(result), [](wchar_t ch) {
        if (ch > 0x7f) {
            throw std::runtime_error("non-ASCII character cannot be narrowed");
        }

        return static_cast<char>(ch);
    });
    return result;
}

}  // namespace ssd_cache
