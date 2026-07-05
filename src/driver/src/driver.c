#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>

#include "cachemon_messages.h"

#define CACHEMON_CONTEXT_TAG 'hSmC'
#define CACHEMON_NAME_TAG 'nCmC'

typedef struct _CACHEMON_STREAM_HANDLE_CONTEXT {
    BOOLEAN WriteObserved;
} CACHEMON_STREAM_HANDLE_CONTEXT, *PCACHEMON_STREAM_HANDLE_CONTEXT;

static PFLT_FILTER g_filter = NULL;
static PFLT_PORT g_server_port = NULL;
static PFLT_PORT g_client_port = NULL;
static HANDLE g_service_pid = NULL;
static FAST_MUTEX g_client_lock;

DRIVER_INITIALIZE DriverEntry;

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

static const FLT_OPERATION_REGISTRATION g_callbacks[] = {
    {IRP_MJ_CREATE, 0, CachemonPreCreate, CachemonPostCreate},
    {IRP_MJ_READ, 0, CachemonPreRead, NULL},
    {IRP_MJ_WRITE, 0, CachemonPreWrite, NULL},
    {IRP_MJ_CLEANUP, 0, CachemonPreCleanup, NULL},
    {IRP_MJ_SET_INFORMATION, 0, CachemonPreSetInformation, NULL},
    {IRP_MJ_OPERATION_END}
};

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

static BOOLEAN CachemonShouldIgnore(PFLT_CALLBACK_DATA Data) {
    if (Data == NULL || Data->Iopb == NULL) {
        return TRUE;
    }

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_GENERATED_IO)) {
        return TRUE;
    }

    if (FlagOn(
        Data->Iopb->IrpFlags,
        IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO
    )) {
        return TRUE;
    }

    if (g_service_pid != NULL &&
        (HANDLE)(ULONG_PTR)FltGetRequestorProcessId(Data) == g_service_pid) {
        return TRUE;
    }

    return FALSE;
}

static VOID CachemonLog(_In_z_ PCSTR Message) {
    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_INFO_LEVEL,
        "CacheMon: %s\n",
        Message
    );
}

static VOID CachemonLogStatus(_In_z_ PCSTR Prefix, NTSTATUS Status) {
    CHAR message[128];
    ULONG status_value;
    SIZE_T index = 0;
    SIZE_T prefix_index = 0;
    INT shift;
    static const CHAR hex_digits[] = "0123456789ABCDEF";
    static const CHAR separator[] = " status=0x";

    while (Prefix[prefix_index] != '\0' &&
        index + RTL_NUMBER_OF(separator) + 8 < RTL_NUMBER_OF(message)) {
        message[index] = Prefix[prefix_index];
        ++index;
        ++prefix_index;
    }

    prefix_index = 0;
    while (separator[prefix_index] != '\0' &&
        index + 1 < RTL_NUMBER_OF(message)) {
        message[index] = separator[prefix_index];
        ++index;
        ++prefix_index;
    }

    status_value = (ULONG)Status;
    for (shift = 28; shift >= 0 && index + 1 < RTL_NUMBER_OF(message);
        shift -= 4) {
        message[index] = hex_digits[(status_value >> shift) & 0xF];
        ++index;
    }

    message[index] = '\0';
    CachemonLog(message);
}

static VOID CachemonCopyUnicodeString(
    wchar_t* Destination,
    ULONG DestinationCount,
    PCUNICODE_STRING Source
) {
    ULONG chars_to_copy;

    if (DestinationCount == 0) {
        return;
    }

    RtlZeroMemory(Destination, DestinationCount * sizeof(wchar_t));
    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0) {
        return;
    }

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

    if (CachemonShouldIgnore(Data)) {
        return;
    }

    status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT,
        &name_info
    );
    if (!NT_SUCCESS(status)) {
        CachemonLog("Failed to resolve file name for activity event.");
        return;
    }

    RtlZeroMemory(&event, sizeof(event));
    event.version = CACHEMON_EVENT_VERSION;
    event.kind = (uint32_t)Kind;
    event.requestor_pid = (uint64_t)(ULONG_PTR)FltGetRequestorProcessId(Data);
    CachemonCopyUnicodeString(
        event.relative_path,
        CACHEMON_MAX_RELATIVE_CHARS,
        &name_info->Name
    );

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

static FLT_POSTOP_CALLBACK_STATUS CachemonPostCreate(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID CompletionContext,
    FLT_POST_OPERATION_FLAGS Flags
) {
    ACCESS_MASK desired_access;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (FlagOn(Flags, FLTFL_POST_OPERATION_DRAINING)) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (!NT_SUCCESS(Data->IoStatus.Status)) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    desired_access =
        Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
    if (FlagOn(desired_access, FILE_READ_DATA | GENERIC_READ)) {
        CachemonSendEvent(Data, FltObjects, CachemonAccessReadOpen);
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

static FLT_PREOP_CALLBACK_STATUS CachemonPreRead(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    UNREFERENCED_PARAMETER(CompletionContext);
    CachemonSendEvent(Data, FltObjects, CachemonAccessReadOpen);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static FLT_PREOP_CALLBACK_STATUS CachemonPreWrite(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    NTSTATUS status;
    PCACHEMON_STREAM_HANDLE_CONTEXT context = NULL;

    UNREFERENCED_PARAMETER(CompletionContext);

    if (CachemonShouldIgnore(Data)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

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

    if (context != NULL) {
        context->WriteObserved = TRUE;
        FltReleaseContext(context);
    }

    CachemonSendEvent(Data, FltObjects, CachemonAccessWriteObserved);
    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static FLT_PREOP_CALLBACK_STATUS CachemonPreCleanup(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    NTSTATUS status;
    PCACHEMON_STREAM_HANDLE_CONTEXT context = NULL;

    UNREFERENCED_PARAMETER(CompletionContext);

    status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&context
    );
    if (NT_SUCCESS(status) && context != NULL) {
        if (context->WriteObserved) {
            CachemonSendEvent(Data, FltObjects, CachemonAccessWriteClosed);
        }

        FltDeleteStreamHandleContext(
            FltObjects->Instance,
            FltObjects->FileObject,
            NULL
        );
        FltReleaseContext(context);
    }

    return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

static FLT_PREOP_CALLBACK_STATUS CachemonPreSetInformation(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    PVOID* CompletionContext
) {
    FILE_INFORMATION_CLASS info_class;

    UNREFERENCED_PARAMETER(CompletionContext);

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

static VOID CachemonContextCleanup(
    PFLT_CONTEXT Context,
    FLT_CONTEXT_TYPE ContextType
) {
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ContextType);
}

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

    ExAcquireFastMutex(&g_client_lock);
    if (g_client_port != NULL) {
        FltCloseClientPort(g_filter, &g_client_port);
    }
    g_client_port = ClientPort;
    ExReleaseFastMutex(&g_client_lock);
    CachemonLog("Driver accepted user-mode connection.");
    return STATUS_SUCCESS;
}

static VOID CachemonDisconnect(PVOID ConnectionCookie) {
    UNREFERENCED_PARAMETER(ConnectionCookie);

    ExAcquireFastMutex(&g_client_lock);
    if (g_client_port != NULL) {
        FltCloseClientPort(g_filter, &g_client_port);
        g_client_port = NULL;
    }
    g_service_pid = NULL;
    ExReleaseFastMutex(&g_client_lock);
    CachemonLog("Driver disconnected user-mode client.");
}

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

    if (ReturnOutputBufferLength != NULL) {
        *ReturnOutputBufferLength = 0;
    }

    if (InputBufferLength < sizeof(CACHEMON_COMMAND)) {
        return STATUS_INVALID_PARAMETER;
    }

    command = (PCACHEMON_COMMAND)InputBuffer;
    if (command->version != CACHEMON_COMMAND_VERSION) {
        return STATUS_INVALID_PARAMETER;
    }

    if (command->command == CachemonCommandRegisterService) {
        g_service_pid = (HANDLE)(ULONG_PTR)command->service_pid;
        CachemonLog("Driver registered service process.");
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS CachemonUnload(FLT_FILTER_UNLOAD_FLAGS Flags) {
    UNREFERENCED_PARAMETER(Flags);

    if (g_server_port != NULL) {
        FltCloseCommunicationPort(g_server_port);
        g_server_port = NULL;
    }

    ExAcquireFastMutex(&g_client_lock);
    if (g_client_port != NULL) {
        FltCloseClientPort(g_filter, &g_client_port);
        g_client_port = NULL;
    }
    ExReleaseFastMutex(&g_client_lock);

    if (g_filter != NULL) {
        FltUnregisterFilter(g_filter);
        g_filter = NULL;
    }

    CachemonLog("Driver unloaded.");

    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    NTSTATUS status;
    PSECURITY_DESCRIPTOR security_descriptor = NULL;
    OBJECT_ATTRIBUTES object_attributes;
    UNICODE_STRING port_name;

    UNREFERENCED_PARAMETER(RegistryPath);

    ExInitializeFastMutex(&g_client_lock);
    CachemonLog("DriverEntry started.");

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

    RtlInitUnicodeString(&port_name, CACHEMON_PORT_NAME);
    InitializeObjectAttributes(
        &object_attributes,
        &port_name,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        security_descriptor
    );

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
