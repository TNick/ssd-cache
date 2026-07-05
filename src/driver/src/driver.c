/**
 * @file
 * @brief CacheMon minifilter driver.
 *
 * A file-system minifilter that observes access to files on the monitored
 * source and reports each access to the user-mode service over a filter
 * communication port. The service uses these events to decide what to cache.
 *
 * Flow: DriverEntry registers the filter, its operation callbacks and a
 * communication port. The pre/post-operation callbacks translate relevant IRPs
 * (create-for-read, read, write, cleanup, rename/delete) into CACHEMON_EVENT
 * messages sent to the connected client. The service registers its own pid so
 * its I/O can be ignored, preventing a feedback loop while it populates the
 * cache.
 */

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>

#include "cachemon_messages.h"

/** Pool tag for stream-handle contexts ('CmSh'). */
#define CACHEMON_CONTEXT_TAG 'hSmC'
/** Pool tag for file-name allocations ('CmCn'). */
#define CACHEMON_NAME_TAG 'nCmC'

/**
 * Per-open stream-handle context used to remember whether the handle was
 * written to, so a write-closed event can be emitted at cleanup.
 */
typedef struct _CACHEMON_STREAM_HANDLE_CONTEXT {
    BOOLEAN WriteObserved;  /**< Set once a write on this handle is observed. */
} CACHEMON_STREAM_HANDLE_CONTEXT, *PCACHEMON_STREAM_HANDLE_CONTEXT;

static PFLT_FILTER g_filter = NULL;       /**< Registered filter handle. */
static PFLT_PORT g_server_port = NULL;    /**< Listening communication port. */
static PFLT_PORT g_client_port = NULL;    /**< Connected client port, or NULL. */
static HANDLE g_service_pid = NULL;       /**< Service pid whose I/O is ignored. */
static FAST_MUTEX g_client_lock;          /**< Guards g_client_port/g_service_pid. */

DRIVER_INITIALIZE DriverEntry;

// Forward declarations of the minifilter callbacks and helpers; each is
// documented at its definition below.
static VOID CachemonLog(_In_z_ PCSTR Message);

static VOID CachemonLogStatus(_In_z_ PCSTR Prefix, NTSTATUS Status);

static NTSTATUS CachemonUnload(FLT_FILTER_UNLOAD_FLAGS Flags);

static FLT_PREOP_CALLBACK_STATUS CachemonPreRead(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
);

static FLT_PREOP_CALLBACK_STATUS CachemonPreWrite(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
);

static FLT_PREOP_CALLBACK_STATUS CachemonPreCleanup(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
);

static FLT_PREOP_CALLBACK_STATUS CachemonPreSetInformation(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
);

static FLT_PREOP_CALLBACK_STATUS CachemonPreCreate(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
);

static FLT_POSTOP_CALLBACK_STATUS CachemonPostCreate(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID CompletionContext,
    FLT_POST_OPERATION_FLAGS Flags
);

static VOID CachemonContextCleanup(
    PFLT_CONTEXT Context,
    FLT_CONTEXT_TYPE ContextType
);

static NTSTATUS CachemonConnect(
    PFLT_PORT ClientPort,
    PVOID ServerPortCookie,
    PVOID ConnectionContext,
    ULONG SizeOfContext,
    PVOID* ConnectionPortCookie
);

static VOID CachemonDisconnect(PVOID ConnectionCookie);

static NTSTATUS CachemonMessage(
    PVOID PortCookie,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PULONG ReturnOutputBufferLength
);

/** Operation callbacks registered with the filter manager. */
static const FLT_OPERATION_REGISTRATION g_callbacks[] = {
    {IRP_MJ_CREATE, 0, CachemonPreCreate, CachemonPostCreate},
    {IRP_MJ_READ, 0, CachemonPreRead, NULL},
    {IRP_MJ_WRITE, 0, CachemonPreWrite, NULL},
    {IRP_MJ_CLEANUP, 0, CachemonPreCleanup, NULL},
    {IRP_MJ_SET_INFORMATION, 0, CachemonPreSetInformation, NULL},
    {IRP_MJ_OPERATION_END}
};

/** Context types the filter uses (a single stream-handle context). */
static const FLT_CONTEXT_REGISTRATION g_contexts[] = {
    {
        FLT_STREAMHANDLE_CONTEXT,
        0,
        CachemonContextCleanup,
        sizeof(CACHEMON_STREAM_HANDLE_CONTEXT),
        CACHEMON_CONTEXT_TAG
    },
    {FLT_CONTEXT_END}
};

/** Top-level filter registration passed to FltRegisterFilter. */
static const FLT_REGISTRATION g_filter_registration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    g_contexts,
    g_callbacks,
    CachemonUnload,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/**
 * Decides whether an operation should be ignored rather than reported.
 *
 * @param Data The callback data for the operation.
 * @return TRUE for data that is absent, filter-generated I/O, paging I/O, or
 *         issued by the registered service process; FALSE otherwise.
 */
static BOOLEAN CachemonShouldIgnore(PFLT_CALLBACK_DATA Data) {
    // No usable request to inspect.
    if (Data == NULL || Data->Iopb == NULL) {
        return TRUE;
    }

    // Skip I/O the filter manager itself generated (not a real user access).
    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_GENERATED_IO)) {
        return TRUE;
    }

    // Skip paging I/O (e.g. memory-mapped access), which is not a file open we
    // want to treat as a cache-worthy access.
    if (FlagOn(
        Data->Iopb->IrpFlags,
        IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO
    )) {
        return TRUE;
    }

    // Skip the service's own I/O so its cache-population reads/writes do not feed
    // back as new events.
    if (g_service_pid != NULL &&
        (HANDLE)(ULONG_PTR)FltGetRequestorProcessId(Data) == g_service_pid) {
        return TRUE;
    }

    return FALSE;
}

/**
 * Emits a debug log line via DbgPrintEx (viewable in WinDbg/DebugView).
 *
 * @param Message Null-terminated message to log.
 */
static VOID CachemonLog(_In_z_ PCSTR Message) {
    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_INFO_LEVEL,
        "CacheMon: %s\n",
        Message
    );
}

/**
 * Logs a message followed by an NTSTATUS in hex. Formats manually (no CRT/RTL
 * string formatting) to stay safe at the IRQL these callbacks may run at.
 *
 * @param Prefix Null-terminated text placed before the status.
 * @param Status The status code to append as " status=0x........".
 */
static VOID CachemonLogStatus(_In_z_ PCSTR Prefix, NTSTATUS Status) {
    CHAR message[128];
    ULONG status_value;
    SIZE_T index = 0;
    SIZE_T prefix_index = 0;
    INT shift;
    static const CHAR hex_digits[] = "0123456789ABCDEF";
    static const CHAR separator[] = " status=0x";

    // Copy the caller's prefix, leaving room for the separator and 8 hex digits.
    while (Prefix[prefix_index] != '\0' &&
        index + RTL_NUMBER_OF(separator) + 8 < RTL_NUMBER_OF(message)) {
        message[index] = Prefix[prefix_index];
        ++index;
        ++prefix_index;
    }

    // Append the " status=0x" separator.
    prefix_index = 0;
    while (separator[prefix_index] != '\0' &&
        index + 1 < RTL_NUMBER_OF(message)) {
        message[index] = separator[prefix_index];
        ++index;
        ++prefix_index;
    }

    // Append the 32-bit status as 8 hex digits, most significant nibble first.
    status_value = (ULONG)Status;
    for (shift = 28; shift >= 0 && index + 1 < RTL_NUMBER_OF(message);
        shift -= 4) {
        message[index] = hex_digits[(status_value >> shift) & 0xF];
        ++index;
    }

    // Terminate and emit the assembled line.
    message[index] = '\0';
    CachemonLog(message);
}

/**
 * Copies a UNICODE_STRING into a fixed-size, null-terminated wide buffer,
 * truncating if necessary. The destination is always zeroed first.
 *
 * @param Destination Destination buffer.
 * @param DestinationCount Capacity of @p Destination in wchar_t (including the
 *        terminator); a zero count is a no-op.
 * @param Source Source string; a NULL/empty source leaves the buffer empty.
 */
static VOID CachemonCopyUnicodeString(
    wchar_t* Destination,
    ULONG DestinationCount,
    PCUNICODE_STRING Source
) {
    ULONG chars_to_copy;

    // Nothing to copy into.
    if (DestinationCount == 0) {
        return;
    }

    // Start from an all-zero (empty, terminated) buffer; an absent source stops
    // here, leaving the buffer empty.
    RtlZeroMemory(Destination, DestinationCount * sizeof(wchar_t));
    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0) {
        return;
    }

    // Copy as many characters as fit, reserving one slot for the terminator.
    chars_to_copy = Source->Length / sizeof(wchar_t);
    if (chars_to_copy >= DestinationCount) {
        chars_to_copy = DestinationCount - 1;
    }

    RtlCopyMemory(
        Destination,
        Source->Buffer,
        chars_to_copy * sizeof(wchar_t)
    );
    Destination[chars_to_copy] = L'\0';
}

/**
 * Builds a CACHEMON_EVENT for the operation and sends it to the connected
 * client (if any). Ignored operations and name-resolution failures are dropped.
 * Sending uses a short timeout so a stalled client cannot block file I/O.
 *
 * @param Data The callback data for the operation.
 * @param FltObjects Related objects for the operation (unused).
 * @param Kind The access kind to report.
 */
static VOID CachemonSendEvent(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    CACHEMON_ACCESS_KIND Kind
) {
    NTSTATUS status;
    PFLT_FILE_NAME_INFORMATION name_info = NULL;
    CACHEMON_EVENT event;
    LARGE_INTEGER timeout;

    UNREFERENCED_PARAMETER(FltObjects);

    // Drop operations we never report (service's own I/O, paging I/O, etc.).
    if (CachemonShouldIgnore(Data)) {
        return;
    }

    // Resolve the normalized file name; without it there is nothing to report.
    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &name_info
    );
    if (!NT_SUCCESS(status)) {
        CachemonLog("Failed to resolve file name for activity event.");
        return;
    }

    // Populate the event with the kind, requesting pid and file path.
    RtlZeroMemory(&event, sizeof(event));
    event.version = CACHEMON_EVENT_VERSION;
    event.kind = (uint32_t)Kind;
    event.requestor_pid = (uint64_t)(ULONG_PTR)FltGetRequestorProcessId(Data);
    CachemonCopyUnicodeString(
        event.relative_path,
        CACHEMON_MAX_RELATIVE_CHARS,
        &name_info->Name
    );

    // Send to the client under the lock, with a 100ms timeout so a stalled or
    // slow client cannot hold up the file operation indefinitely.
    timeout.QuadPart = -1000 * 1000;
    ExAcquireFastMutex(&g_client_lock);
    if (g_client_port != NULL) {
        (VOID)FltSendMessage(
            g_filter,
            &g_client_port,
            &event,
            sizeof(event),
            NULL,
            NULL,
            &timeout
        );
    }
    ExReleaseFastMutex(&g_client_lock);

    FltReleaseFileNameInformation(name_info);
}

/**
 * Pre-create callback. Requests a synchronized post-create so the create's final
 * status and granted access can be inspected there.
 *
 * @param Data The callback data (unused here).
 * @param FltObjects Related objects (unused here).
 * @param CompletionContext Set to NULL; no context is passed to post-create.
 * @return FLT_PREOP_SYNCHRONIZE to receive a synchronized post-create.
 */
static FLT_PREOP_CALLBACK_STATUS CachemonPreCreate(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;
    return FLT_PREOP_SYNCHRONIZE;
}

/**
 * Post-create callback. On a successful open that requested read access, reports
 * a read-open event (a good proxy for "this file may be worth caching").
 *
 * @param Data The callback data for the completed create.
 * @param FltObjects Related objects for the operation.
 * @param CompletionContext Context from pre-create (unused; always NULL).
 * @param Flags Post-operation flags; draining short-circuits processing.
 * @return FLT_POSTOP_FINISHED_PROCESSING.
 */
static FLT_POSTOP_CALLBACK_STATUS CachemonPostCreate(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID CompletionContext,
    FLT_POST_OPERATION_FLAGS Flags
) {
    ACCESS_MASK desired_access;

    UNREFERENCED_PARAMETER(CompletionContext);

    // The instance is being torn down; skip processing.
    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING)) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    // The open failed, so there is no successful access to report.
    if (!NT_SUCCESS(Data->IoStatus.Status)) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    // Only opens that asked for read access are treated as cache-worthy reads.
    desired_access =
        Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    if (FlagOn(desired_access, FILE_READ_DATA | GENERIC_READ)) {
        CachemonSendEvent(Data, FltObjects, CachemonAccessReadOpen);
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

/**
 * Pre-read callback. Reports a read-open event for the read.
 *
 * @param Data The callback data for the read.
 * @param FltObjects Related objects for the operation.
 * @param CompletionContext Unused; no post-read callback is requested.
 * @return FLT_PREOP_SUCCESS_NO_CALLBACK.
 */
static FLT_PREOP_CALLBACK_STATUS CachemonPreRead(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    UNREFERENCED_PARAMETER(CompletionContext);
    CachemonSendEvent(Data, FltObjects, CachemonAccessReadOpen);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/**
 * Pre-write callback. Marks the stream handle as written (allocating the
 * stream-handle context if needed) so cleanup can emit a write-closed event,
 * and reports a write-observed event now.
 *
 * @param Data The callback data for the write.
 * @param FltObjects Related objects for the operation.
 * @param CompletionContext Unused; no post-write callback is requested.
 * @return FLT_PREOP_SUCCESS_NO_CALLBACK.
 */
static FLT_PREOP_CALLBACK_STATUS CachemonPreWrite(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    NTSTATUS status;
    PCACHEMON_STREAM_HANDLE_CONTEXT context = NULL;

    UNREFERENCED_PARAMETER(CompletionContext);

    // Ignored writes need neither a context nor an event.
    if (CachemonShouldIgnore(Data)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    // Fetch this handle's stream context, allocating and attaching one on first
    // write so we can remember that the handle was written to.
    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&context
    );
    if (!NT_SUCCESS(status)) {
        status = FltAllocateContext(
            g_filter,
            FLT_STREAMHANDLE_CONTEXT,
            sizeof(CACHEMON_STREAM_HANDLE_CONTEXT),
            NonPagedPoolNx,
            (PFLT_CONTEXT*)&context
        );
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(context, sizeof(*context));
            (VOID)FltSetStreamHandleContext(
                FltObjects->Instance,
                FltObjects->FileObject,
                FLT_SET_CONTEXT_KEEP_IF_EXISTS,
                context,
                NULL
            );
        }
    }

    // Mark the handle as written and release our reference.
    if (context != NULL) {
        context->WriteObserved = TRUE;
        FltReleaseContext(context);
    }

    // Report the write itself.
    CachemonSendEvent(Data, FltObjects, CachemonAccessWriteObserved);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/**
 * Pre-cleanup callback. When the handle being closed was written to, reports a
 * write-closed event, then deletes and releases the stream-handle context.
 *
 * @param Data The callback data for the cleanup.
 * @param FltObjects Related objects for the operation.
 * @param CompletionContext Unused; no post-cleanup callback is requested.
 * @return FLT_PREOP_SUCCESS_NO_CALLBACK.
 */
static FLT_PREOP_CALLBACK_STATUS CachemonPreCleanup(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    NTSTATUS status;
    PCACHEMON_STREAM_HANDLE_CONTEXT context = NULL;

    UNREFERENCED_PARAMETER(CompletionContext);

    // If this handle carries our stream context, it was tracked for writes.
    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&context
    );
    if (NT_SUCCESS(status) && context != NULL) {
        // A written-to handle being closed means the file's contents settled:
        // report a write-closed event.
        if (context->WriteObserved) {
            CachemonSendEvent(Data, FltObjects, CachemonAccessWriteClosed);
        }

        // Drop the context now that the handle is going away.
        FltDeleteStreamHandleContext(
            FltObjects->Instance,
            FltObjects->FileObject,
            NULL
        );
        FltReleaseContext(context);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/**
 * Pre-set-information callback. Reports a rename event for rename info classes
 * and a delete event for disposition (delete) info classes.
 *
 * @param Data The callback data for the set-information operation.
 * @param FltObjects Related objects for the operation.
 * @param CompletionContext Unused; no post callback is requested.
 * @return FLT_PREOP_SUCCESS_NO_CALLBACK.
 */
static FLT_PREOP_CALLBACK_STATUS CachemonPreSetInformation(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    FILE_INFORMATION_CLASS info_class;

    UNREFERENCED_PARAMETER(CompletionContext);

    // Report renames and deletes; ignore all other set-information classes.
    info_class = Data->Iopb->Parameters.SetFileInformation.FileInformationClass;
    if (info_class == FileRenameInformation ||
        info_class == FileRenameInformationEx) {
        CachemonSendEvent(Data, FltObjects, CachemonAccessRename);
    } else if (info_class == FileDispositionInformation ||
        info_class == FileDispositionInformationEx) {
        CachemonSendEvent(Data, FltObjects, CachemonAccessDelete);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

/**
 * Context-cleanup callback invoked when a filter context is torn down. The
 * stream-handle context holds no owned resources, so nothing is freed here.
 *
 * @param Context The context being cleaned up (unused).
 * @param ContextType The type of context (unused).
 */
static VOID CachemonContextCleanup(
    PFLT_CONTEXT Context,
    FLT_CONTEXT_TYPE ContextType
) {
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ContextType);
}

/**
 * Connection callback invoked when the user-mode service connects to the port.
 * Stores the client port, replacing any previous connection.
 *
 * @param ClientPort The new client port to communicate back on.
 * @param ServerPortCookie Server port cookie (unused).
 * @param ConnectionContext Client-supplied connection context (unused).
 * @param SizeOfContext Size of @p ConnectionContext (unused).
 * @param ConnectionPortCookie Out cookie for this connection (unused).
 * @return STATUS_SUCCESS.
 */
static NTSTATUS CachemonConnect(
    PFLT_PORT ClientPort,
    PVOID ServerPortCookie,
    PVOID ConnectionContext,
    ULONG SizeOfContext,
    PVOID* ConnectionPortCookie
) {
    UNREFERENCED_PARAMETER(ServerPortCookie);
    UNREFERENCED_PARAMETER(ConnectionContext);
    UNREFERENCED_PARAMETER(SizeOfContext);
    UNREFERENCED_PARAMETER(ConnectionPortCookie);

    // Adopt the new client port under the lock, closing any stale one first so
    // only a single client is ever connected.
    ExAcquireFastMutex(&g_client_lock);
    if (g_client_port != NULL) {
        FltCloseClientPort(g_filter, &g_client_port);
    }
    g_client_port = ClientPort;
    ExReleaseFastMutex(&g_client_lock);
    CachemonLog("Driver accepted user-mode connection.");
    return STATUS_SUCCESS;
}

/**
 * Disconnection callback invoked when the client disconnects. Closes the client
 * port and forgets the registered service pid.
 *
 * @param ConnectionCookie Connection cookie from CachemonConnect (unused).
 */
static VOID CachemonDisconnect(PVOID ConnectionCookie) {
    UNREFERENCED_PARAMETER(ConnectionCookie);

    // Close the client port and clear the registered pid so subsequent I/O is no
    // longer treated as the service's.
    ExAcquireFastMutex(&g_client_lock);
    if (g_client_port != NULL) {
        FltCloseClientPort(g_filter, &g_client_port);
        g_client_port = NULL;
    }
    g_service_pid = NULL;
    ExReleaseFastMutex(&g_client_lock);
    CachemonLog("Driver disconnected user-mode client.");
}

/**
 * Message callback for commands sent by the client. Currently handles only the
 * register-service command, which records the service pid so its I/O is ignored.
 *
 * @param PortCookie Port cookie (unused).
 * @param InputBuffer Command buffer (a CACHEMON_COMMAND).
 * @param InputBufferLength Size of @p InputBuffer in bytes.
 * @param OutputBuffer Reply buffer (unused).
 * @param OutputBufferLength Size of @p OutputBuffer (unused).
 * @param ReturnOutputBufferLength Out: bytes written to the reply (set to 0).
 * @return STATUS_SUCCESS on a recognized command; STATUS_INVALID_PARAMETER for a
 *         too-small buffer, wrong version, or unknown command.
 */
static NTSTATUS CachemonMessage(
    PVOID PortCookie,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    PULONG ReturnOutputBufferLength
) {
    PCACHEMON_COMMAND command;

    UNREFERENCED_PARAMETER(PortCookie);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    // This driver sends no reply payload.
    if (ReturnOutputBufferLength != NULL) {
        *ReturnOutputBufferLength = 0;
    }

    // Reject undersized or version-mismatched commands.
    if (InputBufferLength < sizeof(CACHEMON_COMMAND)) {
        return STATUS_INVALID_PARAMETER;
    }

    command = (PCACHEMON_COMMAND)InputBuffer;
    if (command->version != CACHEMON_COMMAND_VERSION) {
        return STATUS_INVALID_PARAMETER;
    }

    // Register-service: remember the caller's pid so its own I/O is ignored.
    if (command->command == CachemonCommandRegisterService) {
        g_service_pid = (HANDLE)(ULONG_PTR)command->service_pid;
        CachemonLog("Driver registered service process.");
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

/**
 * Filter unload callback. Closes the server and client ports and unregisters the
 * filter. Also used for cleanup on a failed DriverEntry.
 *
 * @param Flags Unload flags (unused).
 * @return STATUS_SUCCESS.
 */
static NTSTATUS CachemonUnload(FLT_FILTER_UNLOAD_FLAGS Flags) {
    UNREFERENCED_PARAMETER(Flags);

    // Stop accepting new connections.
    if (g_server_port != NULL) {
        FltCloseCommunicationPort(g_server_port);
        g_server_port = NULL;
    }

    // Drop any live client connection under the lock.
    ExAcquireFastMutex(&g_client_lock);
    if (g_client_port != NULL) {
        FltCloseClientPort(g_filter, &g_client_port);
        g_client_port = NULL;
    }
    ExReleaseFastMutex(&g_client_lock);

    // Tear down the filter registration.
    if (g_filter != NULL) {
        FltUnregisterFilter(g_filter);
        g_filter = NULL;
    }

    CachemonLog("Driver unloaded.");

    return STATUS_SUCCESS;
}

/**
 * Driver entry point. Registers the minifilter, builds a default security
 * descriptor, creates the communication port and starts filtering. On any
 * failure it unwinds whatever was set up and returns the failing status.
 *
 * @param DriverObject The driver object for this driver.
 * @param RegistryPath The driver's registry path (unused).
 * @return STATUS_SUCCESS on success, or the first failing NTSTATUS.
 */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    NTSTATUS status;
    PSECURITY_DESCRIPTOR security_descriptor = NULL;
    OBJECT_ATTRIBUTES object_attributes;
    UNICODE_STRING port_name;

    UNREFERENCED_PARAMETER(RegistryPath);

    // Initialize the lock guarding the client connection state.
    ExInitializeFastMutex(&g_client_lock);
    CachemonLog("DriverEntry started.");

    // Register the minifilter (callbacks and contexts). Nothing to unwind on
    // failure yet.
    CachemonLog("Calling FltRegisterFilter.");
    status = FltRegisterFilter(
        DriverObject,
        &g_filter_registration,
        &g_filter
    );
    if (!NT_SUCCESS(status)) {
        CachemonLogStatus("FltRegisterFilter failed.", status);
        return status;
    }
    CachemonLogStatus("FltRegisterFilter succeeded.", status);

    // Build the security descriptor that governs who may open the port. On
    // failure, undo the filter registration.
    CachemonLog("Calling FltBuildDefaultSecurityDescriptor.");
    status = FltBuildDefaultSecurityDescriptor(
        &security_descriptor,
        FLT_PORT_ALL_ACCESS
    );
    if (!NT_SUCCESS(status)) {
        CachemonLogStatus(
            "FltBuildDefaultSecurityDescriptor failed.",
            status
        );
        FltUnregisterFilter(g_filter);
        g_filter = NULL;
        return status;
    }
    CachemonLogStatus(
        "FltBuildDefaultSecurityDescriptor succeeded.",
        status
    );

    // Describe the named communication port with that security descriptor.
    RtlInitUnicodeString(&port_name, CACHEMON_PORT_NAME);
    InitializeObjectAttributes(
        &object_attributes,
        &port_name,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        security_descriptor
    );

    // Create the port (max 1 connection). The descriptor is no longer needed
    // afterward and is freed either way; on failure, undo the registration.
    CachemonLog("Calling FltCreateCommunicationPort.");
    status = FltCreateCommunicationPort(
        g_filter,
        &g_server_port,
        &object_attributes,
        NULL,
        CachemonConnect,
        CachemonDisconnect,
        CachemonMessage,
        1
    );
    FltFreeSecurityDescriptor(security_descriptor);

    if (!NT_SUCCESS(status)) {
        CachemonLogStatus("FltCreateCommunicationPort failed.", status);
        FltUnregisterFilter(g_filter);
        g_filter = NULL;
        return status;
    }
    CachemonLogStatus("FltCreateCommunicationPort succeeded.", status);

    // Begin filtering. On failure, CachemonUnload tears down everything set up
    // above (ports and filter).
    CachemonLog("Calling FltStartFiltering.");
    status = FltStartFiltering(g_filter);
    if (!NT_SUCCESS(status)) {
        CachemonLogStatus("FltStartFiltering failed.", status);
        CachemonUnload(0);
        return status;
    }
    CachemonLogStatus("FltStartFiltering succeeded.", status);

    CachemonLog("DriverEntry completed successfully.");
    return STATUS_SUCCESS;
}
