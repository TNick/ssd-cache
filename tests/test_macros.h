#pragma once

#include <stdexcept>
#include <string>

void register_test(const char* name, void (*fn)());

#define SSD_CACHE_JOIN_DETAIL(left, right) left##right
#define SSD_CACHE_JOIN(left, right) SSD_CACHE_JOIN_DETAIL(left, right)

#define TEST_CASE(name)                                                       \
    static void SSD_CACHE_JOIN(test_, __LINE__)();                            \
    namespace {                                                               \
    struct SSD_CACHE_JOIN(RegisterTest_, __LINE__) {                          \
        SSD_CACHE_JOIN(RegisterTest_, __LINE__)() {                           \
            register_test(name, SSD_CACHE_JOIN(test_, __LINE__));             \
        }                                                                     \
    } SSD_CACHE_JOIN(register_test_, __LINE__);                               \
    }                                                                         \
    static void SSD_CACHE_JOIN(test_, __LINE__)()

#define REQUIRE(condition)                                                     \
    do {                                                                       \
        if (!(condition)) {                                                     \
            throw std::runtime_error(                                           \
                std::string("require failed: ") + #condition                   \
            );                                                                 \
        }                                                                      \
    } while (false)
