#pragma once

/**
 * @file
 * @brief Wire format for CacheMon minifilter user-mode communication.
 *
 * Shared between the kernel driver and user-mode service. Defines the
 * Filter Manager communication port name, command and event layouts, and
 * version numbers. Both sides must agree on these structures and constants.
 */

#include <stdint.h>

/** Filter Manager communication port opened by the CacheMon minifilter. */
#define CACHEMON_PORT_NAME L"\\CacheMonPort"

/** Expected @c version field in a CACHEMON_COMMAND payload. */
#define CACHEMON_COMMAND_VERSION 1

/** Expected @c version field in a CACHEMON_EVENT payload. */
#define CACHEMON_EVENT_VERSION 1

/** Maximum wchar_t count for source-root identifiers and UNC paths. */
#define CACHEMON_MAX_ROOT_CHARS 260

/** Maximum wchar_t count for driver log paths sent with register commands. */
#define CACHEMON_MAX_LOG_PATH_CHARS 260

/** Maximum wchar_t count for file paths relative to a source root. */
#define CACHEMON_MAX_RELATIVE_CHARS 1024

/** Kind of file access reported in a CACHEMON_EVENT. */
typedef enum CACHEMON_ACCESS_KIND {
    CachemonAccessReadOpen = 1,      /**< A file was opened for reading. */
    CachemonAccessWriteObserved = 2, /**< A write to a file was observed. */
    CachemonAccessWriteClosed = 3,   /**< A file previously written was closed. */
    CachemonAccessRename = 4,        /**< A file was renamed. */
    CachemonAccessDelete = 5         /**< A file was deleted. */
} CACHEMON_ACCESS_KIND;

/** Command sent from user mode to the minifilter over the communication port. */
typedef enum CACHEMON_COMMAND_KIND {
    CachemonCommandRegisterService = 1, /**< Register the service process pid. */
    CachemonCommandSetSourceRoot = 2    /**< Reserved: set the monitored source. */
} CACHEMON_COMMAND_KIND;

/** Fixed-layout command buffer sent via FilterSendMessage. */
typedef struct CACHEMON_COMMAND {
    /** Must equal CACHEMON_COMMAND_VERSION. */
    uint32_t version;
    /** CACHEMON_COMMAND_KIND value selecting the operation. */
    uint32_t command;
    /** Process id of the registering service (for I/O self-filtering). */
    uint64_t service_pid;
    /** Monitored source UNC path (null-terminated within the array). */
    wchar_t source_unc[CACHEMON_MAX_ROOT_CHARS];
    /** NT path to the driver log file (null-terminated within the array). */
    wchar_t log_path[CACHEMON_MAX_LOG_PATH_CHARS];
} CACHEMON_COMMAND;

/** Pointer to a CACHEMON_COMMAND. */
typedef CACHEMON_COMMAND* PCACHEMON_COMMAND;

/** Fixed-layout event record delivered via FilterGetMessage. */
typedef struct CACHEMON_EVENT {
    /** Must equal CACHEMON_EVENT_VERSION. */
    uint32_t version;
    /** CACHEMON_ACCESS_KIND value describing the observed access. */
    uint32_t kind;
    /** Best-effort file size from the driver; 0 when unknown. */
    uint64_t size_hint;
    /** Process id that triggered the access; 0 when unknown. */
    uint64_t requestor_pid;
    /** Source root identifier (null-terminated within the array). */
    wchar_t source_root_id[CACHEMON_MAX_ROOT_CHARS];
    /** Path relative to @c source_root_id (null-terminated within the array). */
    wchar_t relative_path[CACHEMON_MAX_RELATIVE_CHARS];
} CACHEMON_EVENT;

/** Pointer to a CACHEMON_EVENT. */
typedef CACHEMON_EVENT* PCACHEMON_EVENT;
