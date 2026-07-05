#pragma once

#include <stdint.h>

#define CACHEMON_PORT_NAME L"\\CacheMonPort"
#define CACHEMON_COMMAND_VERSION 1
#define CACHEMON_EVENT_VERSION 1
#define CACHEMON_MAX_ROOT_CHARS 260
#define CACHEMON_MAX_LOG_PATH_CHARS 260
#define CACHEMON_MAX_RELATIVE_CHARS 1024

typedef enum CACHEMON_ACCESS_KIND {
    CachemonAccessReadOpen = 1,
    CachemonAccessWriteObserved = 2,
    CachemonAccessWriteClosed = 3,
    CachemonAccessRename = 4,
    CachemonAccessDelete = 5
} CACHEMON_ACCESS_KIND;

typedef enum CACHEMON_COMMAND_KIND {
    CachemonCommandRegisterService = 1,
    CachemonCommandSetSourceRoot = 2
} CACHEMON_COMMAND_KIND;

typedef struct CACHEMON_COMMAND {
    uint32_t version;
    uint32_t command;
    uint64_t service_pid;
    wchar_t source_unc[CACHEMON_MAX_ROOT_CHARS];
    wchar_t log_path[CACHEMON_MAX_LOG_PATH_CHARS];
} CACHEMON_COMMAND;
typedef CACHEMON_COMMAND* PCACHEMON_COMMAND;

typedef struct CACHEMON_EVENT {
    uint32_t version;
    uint32_t kind;
    uint64_t size_hint;
    uint64_t requestor_pid;
    wchar_t source_root_id[CACHEMON_MAX_ROOT_CHARS];
    wchar_t relative_path[CACHEMON_MAX_RELATIVE_CHARS];
} CACHEMON_EVENT;
typedef CACHEMON_EVENT* PCACHEMON_EVENT;
