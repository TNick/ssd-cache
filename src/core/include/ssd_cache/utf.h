#pragma once

/**
 * @file
 * @brief Narrow/wide string conversions used throughout the core library.
 */

#include <string>
#include <string_view>

namespace ssd_cache {

/**
 * Converts a UTF-16 string to UTF-8.
 *
 * @param value UTF-16 input.
 * @return The UTF-8 encoding of @p value.
 */
std::string wide_to_utf8(std::wstring_view value);

/**
 * Converts a UTF-8 string to UTF-16.
 *
 * @param value UTF-8 input.
 * @return The UTF-16 encoding of @p value.
 */
std::wstring utf8_to_wide(std::string_view value);

/**
 * Widens an ASCII string to UTF-16 by zero-extending each byte. Intended for
 * known-ASCII input; non-ASCII bytes are not decoded as UTF-8.
 *
 * @param value ASCII input.
 * @return The widened string.
 */
std::wstring widen_ascii(std::string_view value);

/**
 * Narrows a wide string to ASCII by truncating each code unit to a byte.
 * Intended for known-ASCII input; non-ASCII code units are truncated.
 *
 * @param value Wide input.
 * @return The narrowed string.
 */
std::string narrow_ascii(std::wstring_view value);

}  // namespace ssd_cache
