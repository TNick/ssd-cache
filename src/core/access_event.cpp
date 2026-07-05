/**
 * @file
 * @brief Access-kind string conversions and copy-scheduling policy.
 */

#include "ssd_cache/access_event.h"

#include <stdexcept>

namespace ssd_cache {

std::string access_kind_to_string(AccessKind kind) {
    switch (kind) {
        case AccessKind::ReadOpen:
            return "read_open";
        case AccessKind::WriteObserved:
            return "write_observed";
        case AccessKind::WriteClosed:
            return "write_closed";
        case AccessKind::Rename:
            return "rename";
        case AccessKind::Delete:
            return "delete";
    }

    throw std::invalid_argument("unknown access kind");
}

AccessKind access_kind_from_string(std::string_view value) {
    if (value == "read_open") {
        return AccessKind::ReadOpen;
    }

    if (value == "write_observed") {
        return AccessKind::WriteObserved;
    }

    if (value == "write_closed") {
        return AccessKind::WriteClosed;
    }

    if (value == "rename") {
        return AccessKind::Rename;
    }

    if (value == "delete") {
        return AccessKind::Delete;
    }

    throw std::invalid_argument("unknown access kind string");
}

bool should_schedule_copy(AccessKind kind) {
    return kind == AccessKind::ReadOpen || kind == AccessKind::WriteClosed;
}

}  // namespace ssd_cache
