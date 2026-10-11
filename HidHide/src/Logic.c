// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// Logic.c
#include "stdafx.h"
#include "Logic.h"
#include "Config.h"
#include "ControlDevice.h"
#include "Device.h"
#include "Logging.h"

// Unique memory pool tag for buffers
#define LOGIC_TAG 'oLHH'

// Unique memory pool tag for the profile activation stack entries
#define PROFILES_ACTIVATION_TAG 'aPHH'

// Unique memory pool tag for the temporary buffers holding prefixed profile entries
#define PROFILES_STRING_TAG 'sPHH'

// The control device is created first and should be deleted last after having release the driver resources
// Releasing the control device is required else the driver will never unload
WDFDEVICE s_wdfControlDevice = NULL;

// The device is multi-threading hence we need a lock for the critical sections
// As a rule of thumb, don't use the control device context directly but instead use the methods below
WDFWAITLOCK s_criticalSectionLock = NULL;

_Use_decl_annotations_
NTSTATUS OnDriverCreate(WDFDRIVER wdfDriver)
{
    TRACE_ALWAYS(L"");

    WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;
    NTSTATUS              ntstatus;

    // Do an integrity check on the btree algorithm
    ntstatus = HidHideVerifyInternalConsistency();
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Create the control device so that we have a shared context that is device independent
    ntstatus = HidHideControlDeviceCreate(wdfDriver, &s_wdfControlDevice);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Part of the code is multi-threaded so we need a lock for managing the critical section on the control device context
    WDF_OBJECT_ATTRIBUTES_INIT(&wdfObjectAttributes);
    wdfObjectAttributes.ParentObject = s_wdfControlDevice;
    ntstatus = WdfWaitLockCreate(&wdfObjectAttributes, &s_criticalSectionLock);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfWaitLockCreate", ntstatus);

    // Subscribe to the create process and load image notifications
    ntstatus = PsSetCreateProcessNotifyRoutine(OnSystemProcessChange, FALSE);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"PsSetCreateProcessNotifyRoutine", ntstatus);
    ntstatus = PsSetLoadImageNotifyRoutine(OnSystemLoadImage);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"PsSetLoadImageNotifyRoutine", ntstatus);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID OnSystemShutdown(WDFDEVICE wdfControlDevice)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);

    // Stop monitoring of load image notifications
    PsRemoveLoadImageNotifyRoutine(OnSystemLoadImage);
    PsSetCreateProcessNotifyRoutine(OnSystemProcessChange, TRUE);

    // Release the evaluation cache resources
    HidHideProcessIdsFlushWhitelistEvaluationCache(s_criticalSectionLock);

    // Enter shutdown state
    UpdateDataForControlDeviceDeletionAndDeleteControlDeviceWhenNeeded(0);
}

_Use_decl_annotations_
VOID OnControlDeviceContextCleanup(WDFOBJECT wdfControlDeviceObject)
{
    TRACE_ALWAYS(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext = ControlDeviceGetContext(wdfControlDeviceObject);

    // Drain any remaining session blacklist entries left over at driver unload.
    // s_criticalSectionLock is a child of the control device and is still live during its cleanup
    // callback, but guard against the case where WdfWaitLockCreate failed and left it NULL.
    if (NULL != s_criticalSectionLock) WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    PLIST_ENTRY entry = pControlDeviceContext->sessionBlacklistHead.Flink;
    while (entry != &pControlDeviceContext->sessionBlacklistHead)
    {
        PSESSION_BLACKLIST_ENTRY sbe = CONTAINING_RECORD(entry, SESSION_BLACKLIST_ENTRY, listEntry);
        PLIST_ENTRY next = entry->Flink;
        RemoveEntryList(entry);
        WdfObjectDelete(sbe->deviceInstancePath);
        ExFreePoolWithTag(sbe, 'lBSH');
        entry = next;
    }
    if (NULL != s_criticalSectionLock) WdfWaitLockRelease(s_criticalSectionLock);

    // Release the application profiles and their activation entries
    ProfilesFreeAll(pControlDeviceContext);
}

_Use_decl_annotations_
VOID OnSystemProcessChange(HANDLE parentId, HANDLE processId, BOOLEAN create)
{
    TRACE_PERFORMANCE(L"");
    UNREFERENCED_PARAMETER(parentId);

    // When a process is stopped we need to properly unregister it as we no longer need its information
    // Notice that we aren't receiving notifications for any process with enhanced security
    if (FALSE == create)
    {
        HidHideProcessIdUnregister(s_criticalSectionLock, processId);
        SessionBlacklistCleanupForPid(processId);
        ProfilesDeactivateForPid(processId);
    }
}

_Use_decl_annotations_
VOID OnSystemLoadImage(PUNICODE_STRING fullImageName, HANDLE processId, PIMAGE_INFO imageInfo)
{
    TRACE_PERFORMANCE(L"");
    UNREFERENCED_PARAMETER(imageInfo);

    // Ignore load attempts by device drivers (process id zero) or when the image name isn't provided
    if ((NULL != fullImageName) && (NULL != processId))
    {
        // Only process the first load attempt as this is the executable; subsequent calls are for its load modules
        if (STATUS_SUCCESS == HidHideProcessIdRegister(s_criticalSectionLock, processId, fullImageName))
        {
            // A newly registered process may activate the application profile bound to its executable
            ProfilesTryActivate(processId, fullImageName);
        }
    }
}

_Use_decl_annotations_
NTSTATUS OnDeviceCreate(WDFDEVICE wdfDevice)
{
    TRACE_ALWAYS(L"");

    PDEVICE_CONTEXT pDeviceContext;
    NTSTATUS        ntstatus;

    // Get the device instance path of this device and cache it for future use
    pDeviceContext = DeviceGetContext(wdfDevice);
    ntstatus = HidHideDeviceInstancePath(wdfDevice, &pDeviceContext->deviceInstancePath); // PASSIVE_LEVEL
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    UpdateDataForControlDeviceDeletionAndDeleteControlDeviceWhenNeeded(1);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID OnDeviceContextCleanup(WDFOBJECT wdfDeviceObject)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfDeviceObject);

    PDEVICE_CONTEXT pDeviceContext;

    // Release context resources
    pDeviceContext = DeviceGetContext(wdfDeviceObject);
    WdfObjectDelete(pDeviceContext->deviceInstancePath);
    UpdateDataForControlDeviceDeletionAndDeleteControlDeviceWhenNeeded(-1);
}

_Use_decl_annotations_
VOID OnDeviceFileCreate(WDFDEVICE wdfDevice, WDFREQUEST wdfRequest, WDFFILEOBJECT wdfFileObject)
{
    TRACE_PERFORMANCE(L"");
    UNREFERENCED_PARAMETER(wdfFileObject);

    WDF_REQUEST_SEND_OPTIONS wdfRequestSendOptions;
    PDEVICE_CONTEXT          pDeviceContext;
    UNICODE_STRING           deviceInstancePath;
    PEPROCESS                process;
    HANDLE                   processId;
    ULONG                    sessionId;
    BOOLEAN                  accessDenied;
    BOOLEAN                  cacheHit;
    WDFMEMORY                wdfMemory;
    LPWSTR                   buffer;
    NTSTATUS                 ntstatus;

    // Get the device instance path
    pDeviceContext = DeviceGetContext(wdfDevice);
    WdfStringGetUnicodeString(pDeviceContext->deviceInstancePath, &deviceInstancePath);

    // Should access be granted to a particular client that attempts to access the device?
    // This is determined by;
    // - The process id of the caller       --> a system-level process is always granted access
    // - The active state of service        --> is hide-hide enabled or not?
    // - The device instance path of device --> is the device mentioned on the black-list?
    // - The full load image of the client  --> is the caller on the white-list?

    process = PsGetCurrentProcess();
    processId = PsGetCurrentProcessId();
    if (process != NULL)
    {
        sessionId = PsGetProcessSessionId(process);
    }
    else
    {
        sessionId = 0;
    }

    accessDenied = FALSE;
    if ((SYSTEM_PID != PROCESS_HANDLE_TO_PROCESS_ID(processId)) && (GetActive()) && (Blacklisted(&deviceInstancePath, sessionId)))
    {
        // When the service is active, and the process is not a system-process, and the device being accessed is on the black-list, then the final verdict comes from the white-list
        if (Whitelisted(processId, &cacheHit))
        {
            // Log the first-time that a white-listed application is granted access to a black-listed device
            if (!cacheHit) LogEvent(ETW(Whitelisted), L"%ld (Session ID: %ld)", PROCESS_HANDLE_TO_PROCESS_ID(processId), sessionId);
        }
        else
        {
            // Trace the first-time that an application is denied access to a black-listed device
            if (!cacheHit) TRACE_ALWAYS(L"Device is black-listed hence deny access");
            accessDenied = TRUE;
        }
        // Trace the first-time the device instance path is challenged, in support of the message above
        if (!cacheHit)
        {
            ntstatus = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, NonPagedPoolNx, LOGIC_TAG, sizeof(WCHAR) * ((size_t)deviceInstancePath.Length + 1), &wdfMemory, &buffer);
            if (NT_SUCCESS(ntstatus)) ntstatus = RtlStringCchCopyUnicodeStringEx(buffer, ((size_t)deviceInstancePath.Length + 1), &deviceInstancePath, NULL, NULL, STRSAFE_NO_TRUNCATION);
            if (NT_SUCCESS(ntstatus)) TRACE_ALWAYS(buffer);
            WdfObjectDelete(wdfMemory);
        }
    }

    // Handle the request accordingly
    // Note that a file create has to be handled synchrounously
    if (accessDenied)
    {
        WdfRequestComplete(wdfRequest, STATUS_ACCESS_DENIED);
    }
    else
    {
        // Attach a completion routine and forward the original request to the driver lower in the driver stack
        // Note that WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET can't be used here as a bug check triggers when running driver verifier 
        WdfRequestFormatRequestUsingCurrentType(wdfRequest);
        WdfRequestSetCompletionRoutine(wdfRequest, OnDeviceFileCreateRequestCompletion, WDF_NO_CONTEXT);
        WDF_REQUEST_SEND_OPTIONS_INIT(&wdfRequestSendOptions, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&wdfRequestSendOptions, WDF_REL_TIMEOUT_IN_SEC(60));
        ntstatus = WdfRequestAllocateTimer(wdfRequest);
        if (!NT_SUCCESS(ntstatus))
        {
            // Bail out when we don't have the resources to forward the request
            WdfRequestComplete(wdfRequest, ntstatus);
        }
        else
        {
            // Bail out when we failed forwarding the request
            if (FALSE == WdfRequestSend(wdfRequest, WdfDeviceGetIoTarget(wdfDevice), &wdfRequestSendOptions))
            {
                WdfRequestComplete(wdfRequest, WdfRequestGetStatus(wdfRequest));
            }
        }
    }
}

_Use_decl_annotations_
VOID OnDeviceFileCreateRequestCompletion(WDFREQUEST wdfRequest, WDFIOTARGET wdfIoTarget, PWDF_REQUEST_COMPLETION_PARAMS wdfRequestCompletionParams, WDFCONTEXT wdfContext)
{
    TRACE_PERFORMANCE(L"");
    UNREFERENCED_PARAMETER(wdfIoTarget);
    UNREFERENCED_PARAMETER(wdfContext);

    WdfRequestComplete(wdfRequest, wdfRequestCompletionParams->IoStatus.Status);
}

_Use_decl_annotations_
VOID OnDeviceFileCleanup(WDFFILEOBJECT wdfFileObject)
{
    TRACE_PERFORMANCE(L"");
    UNREFERENCED_PARAMETER(wdfFileObject);

    // No specific action needed for this device driver but we like the tracing for tracing purposes
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceCreate(WDFDEVICE wdfControlDevice)
{
    TRACE_ALWAYS(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    NTSTATUS                ntstatus;

    pControlDeviceContext = ControlDeviceGetContext(wdfControlDevice);
    pControlDeviceContext->numberOfDevicesCreated = 0;
    pControlDeviceContext->shutdownPending = FALSE;
    InitializeListHead(&pControlDeviceContext->sessionBlacklistHead);
    InitializeListHead(&pControlDeviceContext->profilesHead);
    InitializeListHead(&pControlDeviceContext->profileActivationStackHead);
    pControlDeviceContext->activeProfile = NULL;

    // Query the multi-string property containing the white-listed full image names
    DECLARE_CONST_UNICODE_STRING(whitelistedFullImageNames, DRIVER_PROPERTY_WHITELISTED_FULL_IMAGE_NAMES);
    ntstatus = HidHideDriverCreateCollectionForMultiStringProperty(&whitelistedFullImageNames, &pControlDeviceContext->whitelistedFullImageNames);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Query the multi-string property containing the black-listed device instance paths
    DECLARE_CONST_UNICODE_STRING(blacklistedDeviceInstancePaths, DRIVER_PROPERTY_BLACKLISTED_DEVICE_INSTANCE_PATHS);
    ntstatus = HidHideDriverCreateCollectionForMultiStringProperty(&blacklistedDeviceInstancePaths, &pControlDeviceContext->blacklistedDeviceInstancePaths);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Query the boolean property indicating the activity state
    DECLARE_CONST_UNICODE_STRING(active, DRIVER_PROPERTY_ACTIVE);
    ntstatus = HidHideDriverGetBooleanProperty(&active, &pControlDeviceContext->active);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Log the activity state
    if (pControlDeviceContext->active)  LogEvent(ETW(Enabled), L"");
    if (!pControlDeviceContext->active) LogEvent(ETW(Disabled), L"");

    // Query the boolean property indicating the inverse state
    DECLARE_CONST_UNICODE_STRING(whitelistedInverse, DRIVER_PROPERTY_WHITELISTED_INVERSE);
    ntstatus = HidHideDriverGetBooleanProperty(&whitelistedInverse, &pControlDeviceContext->whitelistedInverse);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Load the stored application profiles
    ntstatus = ProfilesLoadAll(pControlDeviceContext);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID OnControlDeviceFileCreate(WDFDEVICE wdfControlDevice, WDFREQUEST wdfRequest, WDFFILEOBJECT wdfFileObject)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfFileObject);

    // No specific action needed for this device driver
    // Note that we have to complete the I/O request!
    WdfRequestComplete(wdfRequest, STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID OnControlDeviceFileCleanup(WDFFILEOBJECT wdfFileObject)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfFileObject);

    // No specific action needed for this device driver but we like the tracing for tracing purposes
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoDeviceControl(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_PERFORMANCE(L"");

    switch (ioControlCode)
    {
    case IOCTL_GET_WHITELIST:
        return (OnControlDeviceIoGetWhitelist(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_SET_WHITELIST:
        return (OnControlDeviceIoSetWhitelist(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_GET_BLACKLIST:
        return (OnControlDeviceIoGetBlacklist(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_SET_BLACKLIST:
        return (OnControlDeviceIoSetBlacklist(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_GET_ACTIVE:
        return (OnControlDeviceIoGetActive(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_SET_ACTIVE:
        return (OnControlDeviceIoSetActive(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_GET_WLINVERSE:
        return (OnControlDeviceIoGetInverse(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_SET_WLINVERSE:
        return (OnControlDeviceIoSetInverse(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_ADD_SESSION_BLACKLIST:
        return (OnControlDeviceIoAddSessionBlacklist(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_CLR_SESSION_BLACKLIST:
        return (OnControlDeviceIoClearSessionBlacklist(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_GET_PROFILES:
        return (OnControlDeviceIoGetProfiles(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_GET_PROFILE:
        return (OnControlDeviceIoGetProfile(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_SET_PROFILE:
        return (OnControlDeviceIoSetProfile(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_DEL_PROFILE:
        return (OnControlDeviceIoDelProfile(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    case IOCTL_GET_ACTIVE_PROFILE:
        return (OnControlDeviceIoGetActiveProfile(wdfControlDevice, wdfQueue, wdfRequest, outputBufferLength, inputBufferLength, ioControlCode));
        break;
    default:
        LOG_AND_RETURN_NTSTATUS(L"OnControlDeviceIoDeviceControl", STATUS_INVALID_PARAMETER);
    }
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoGetWhitelist(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR   buffer;
    size_t   neededSizeInCharacters;
    NTSTATUS ntstatus;

    // Validate buffer and, on success, report the current status
    if (0 != inputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if (0 == outputBufferLength)
    {
        // No output buffer, then only report on the size needed
        ntstatus = GetWhitelist(NULL, 0, &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }
    else
    {
        // With an output buffer provided, try to fill it
        ntstatus = WdfRequestRetrieveOutputBuffer(wdfRequest, outputBufferLength, &buffer, NULL);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
        ntstatus = GetWhitelist(buffer, (outputBufferLength / sizeof(WCHAR)), &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, (neededSizeInCharacters * sizeof(WCHAR)));
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoSetWhitelist(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(inputBufferLength);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR   buffer;
    NTSTATUS ntstatus;

    // Validate buffer and
    if (0 != outputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if (0 == inputBufferLength)
    {
        // No input then clear the list
        ntstatus = SetWhitelist(L"\0\0", 2);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }
    else
    {
        // Get the list provided
        ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveInputBuffer", ntstatus);
        ntstatus = SetWhitelist(buffer, (inputBufferLength / sizeof(WCHAR)));
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, inputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoGetBlacklist(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR   buffer;
    size_t   neededSizeInCharacters;
    NTSTATUS ntstatus;

    // Validate buffer and, on success, report the current status
    if (0 != inputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if (0 == outputBufferLength)
    {
        // No output buffer, then only report on the size needed
        ntstatus = GetBlacklist(NULL, 0, &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }
    else
    {
        // With an output buffer provided, try to fill it
        ntstatus = WdfRequestRetrieveOutputBuffer(wdfRequest, outputBufferLength, &buffer, NULL);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
        ntstatus = GetBlacklist(buffer, (outputBufferLength / sizeof(WCHAR)), &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, (neededSizeInCharacters * sizeof(WCHAR)));
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoSetBlacklist(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(inputBufferLength);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR   buffer;
    NTSTATUS ntstatus;

    // Validate buffer and
    if (0 != outputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if (0 == inputBufferLength)
    {
        // No input then clear the list
        ntstatus = SetBlacklist(L"\0\0", 2);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }
    else
    {
        // Get the list provided
        ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveInputBuffer", ntstatus);
        ntstatus = SetBlacklist(buffer, (inputBufferLength / sizeof(WCHAR)));
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, inputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoGetActive(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    PBOOLEAN buffer;
    NTSTATUS ntstatus;

    // Validate buffer and, on success, report the current status
    if ((0 != inputBufferLength) || (sizeof(BOOLEAN) != outputBufferLength)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    ntstatus = WdfRequestRetrieveOutputBuffer(wdfRequest, outputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
    *buffer = GetActive();

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, outputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoSetActive(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    PBOOLEAN buffer;
    NTSTATUS ntstatus;

    // Validate buffer, retrieve its content, and set the state accordingly
    if ((sizeof(BOOLEAN) != inputBufferLength) || (0 != outputBufferLength)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
    ntstatus = SetActive(FALSE != *buffer);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, outputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoGetInverse(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    PBOOLEAN buffer;
    NTSTATUS ntstatus;

    // Validate buffer and, on success, report the current status
    if ((0 != inputBufferLength) || (sizeof(BOOLEAN) != outputBufferLength)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    ntstatus = WdfRequestRetrieveOutputBuffer(wdfRequest, outputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
    *buffer = GetInverse();

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, outputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoSetInverse(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    PBOOLEAN buffer;
    NTSTATUS ntstatus;

    // Validate buffer, retrieve its content, and set the state accordingly
    if ((sizeof(BOOLEAN) != inputBufferLength) || (0 != outputBufferLength)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
    ntstatus = SetInverse(FALSE != *buffer);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, outputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoAddSessionBlacklist(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR                  buffer;
    NTSTATUS                ntstatus;
    HANDLE                  callerPid;
    size_t                  totalChars;
    LIST_ENTRY              localHead;
    LPWSTR                  current;
    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;

    if (0 != outputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    // MULTI_SZ must be at least two null WCHAR terminators (4 bytes) and a whole number of WCHARs
    if (inputBufferLength < (2 * sizeof(WCHAR)))  LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if (0 != (inputBufferLength % sizeof(WCHAR))) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);

    ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveInputBuffer", ntstatus);

    // Verify the buffer ends with a double-NUL as required by MULTI_SZ format
    totalChars = inputBufferLength / sizeof(WCHAR);
    if ((buffer[totalChars - 1] != L'\0') || (buffer[totalChars - 2] != L'\0'))
        LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);

    callerPid             = PsGetCurrentProcessId();
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);

    // Build all entries into a local list before touching the global list.
    // This makes the operation atomic: either all entries are committed or none are.
    InitializeListHead(&localHead);
    ntstatus = STATUS_SUCCESS;
    current  = buffer;

    while ((size_t)(current - buffer) < totalChars && *current != L'\0')
    {
        size_t remaining = totalChars - (size_t)(current - buffer);
        size_t len = wcsnlen(current, remaining);
        if (len >= remaining)
        {
            // Malformed multi-string: no null terminator within buffer bounds
            ntstatus = STATUS_INVALID_PARAMETER;
            break;
        }

        UNICODE_STRING path;
        path.Buffer = current;
        path.Length = (USHORT)(len * sizeof(WCHAR));
        path.MaximumLength = path.Length + sizeof(WCHAR);

        PSESSION_BLACKLIST_ENTRY entry = (PSESSION_BLACKLIST_ENTRY)ExAllocatePoolWithTag(NonPagedPool, sizeof(SESSION_BLACKLIST_ENTRY), 'lBSH');
        if (NULL == entry)
        {
            ntstatus = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        ntstatus = WdfStringCreate(&path, WDF_NO_OBJECT_ATTRIBUTES, &entry->deviceInstancePath);
        if (!NT_SUCCESS(ntstatus))
        {
            ExFreePoolWithTag(entry, 'lBSH');
            break;
        }

        entry->ownerPid = callerPid;
        InsertTailList(&localHead, &entry->listEntry);

        current += len + 1;
    }

    if (!NT_SUCCESS(ntstatus))
    {
        // Free all locally built entries — nothing has been committed to the global list
        PLIST_ENTRY le = localHead.Flink;
        while (le != &localHead)
        {
            PSESSION_BLACKLIST_ENTRY sbe = CONTAINING_RECORD(le, SESSION_BLACKLIST_ENTRY, listEntry);
            le = le->Flink;
            RemoveEntryList(&sbe->listEntry);
            WdfObjectDelete(sbe->deviceInstancePath);
            ExFreePoolWithTag(sbe, 'lBSH');
        }
        LOG_AND_RETURN_NTSTATUS(L"OnControlDeviceIoAddSessionBlacklist", ntstatus);
    }

    // Splice local list into global list under lock — O(1), no allocations inside critical section
    if (!IsListEmpty(&localHead))
    {
        WdfWaitLockAcquire(s_criticalSectionLock, NULL);
        AppendTailList(&pControlDeviceContext->sessionBlacklistHead, &localHead);
        WdfWaitLockRelease(s_criticalSectionLock);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, inputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoClearSessionBlacklist(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(inputBufferLength);
    UNREFERENCED_PARAMETER(ioControlCode);

    if ((0 != outputBufferLength) || (0 != inputBufferLength)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);

    SessionBlacklistCleanupForPid(PsGetCurrentProcessId());

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, 0);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID SessionBlacklistCleanupForPid(HANDLE processId)
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT  pControlDeviceContext;
    PLIST_ENTRY              entry;
    PSESSION_BLACKLIST_ENTRY sbe;
    PLIST_ENTRY              next;

    if (NULL == s_wdfControlDevice) return;

    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);

    entry = pControlDeviceContext->sessionBlacklistHead.Flink;
    while (entry != &pControlDeviceContext->sessionBlacklistHead)
    {
        sbe  = CONTAINING_RECORD(entry, SESSION_BLACKLIST_ENTRY, listEntry);
        next = entry->Flink;

        if (sbe->ownerPid == processId)
        {
            RemoveEntryList(entry);
            WdfObjectDelete(sbe->deviceInstancePath);
            ExFreePoolWithTag(sbe, 'lBSH');
        }

        entry = next;
    }

    WdfWaitLockRelease(s_criticalSectionLock);
}

_Use_decl_annotations_
BOOLEAN Whitelisted(HANDLE processId, BOOLEAN* cacheHit)
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;

    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    BOOLEAN result = (STATUS_PROCESS_IN_JOB == HidHideProcessIdCheckFullImageNameAgainstWhitelist(s_criticalSectionLock, processId, pControlDeviceContext->whitelistedFullImageNames, cacheHit));
    return GetInverse() ? !result : result;
}

void SplitDeviceData(PUNICODE_STRING input, PUNICODE_STRING deviceInstancePath, ULONG* sessionId)
{
    // Initialize delimiter as '!'
    WCHAR delimiter = L'!';
    USHORT delimiterPosition = 0;

    // Search for the delimiter in the input string
    for (USHORT i = 0; i < input->Length / sizeof(WCHAR); i++)
    {
        if (input->Buffer[i] == delimiter)
        {
            delimiterPosition = i;
            break;
        }
    }

    // Set deviceInstancePath to the portion of the input before the delimiter
    deviceInstancePath->Buffer = input->Buffer;
    deviceInstancePath->Length = (delimiterPosition > 0) ? (delimiterPosition * sizeof(WCHAR)) : (input->Length);
    deviceInstancePath->MaximumLength = deviceInstancePath->Length;

    // Set a default session ID
    *sessionId = 0;

    // We have a jail session filter to work with
    if (delimiterPosition > 0)
    {
        // Set sessionIdString to the portion of the input after the delimiter
        UNICODE_STRING sessionIdString;
        sessionIdString.Buffer = &input->Buffer[delimiterPosition + 1]; // +1 to skip the delimiter
        sessionIdString.Length = input->Length - ((delimiterPosition + 1) * sizeof(WCHAR));
        sessionIdString.MaximumLength = sessionIdString.Length;

        // Convert the sessionIdString to a ULONG
        RtlUnicodeStringToInteger(&sessionIdString, 10, sessionId);
    }
}

_Use_decl_annotations_
BOOLEAN Blacklisted(PUNICODE_STRING deviceInstancePath, ULONG sessionId)
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT  pControlDeviceContext;
    UNICODE_STRING           inputFilter;
    UNICODE_STRING           blacklistedDeviceInstancePath;
    WDFCOLLECTION            deviceInstancePaths;
    ULONG                    jailSessionId;
    PLIST_ENTRY              entry;
    PSESSION_BLACKLIST_ENTRY sbe;

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);

    // The device set in effect is the active profile's set when a profile is active, else the persistent blacklist
    deviceInstancePaths = ((NULL != pControlDeviceContext->activeProfile) ? pControlDeviceContext->activeProfile->blacklistedDeviceInstancePaths : pControlDeviceContext->blacklistedDeviceInstancePaths);

    // Check the device set in effect
    for (ULONG index1 = 0, size1 = WdfCollectionGetCount(deviceInstancePaths); (index1 < size1); index1++)
    {
        WdfStringGetUnicodeString(WdfCollectionGetItem(deviceInstancePaths, index1), &inputFilter); // PASSIVE_LEVEL
        SplitDeviceData(&inputFilter, &blacklistedDeviceInstancePath, &jailSessionId);

        if (0 == RtlCompareUnicodeString(&blacklistedDeviceInstancePath, deviceInstancePath, TRUE))
        {
            WdfWaitLockRelease(s_criticalSectionLock);
            return (sessionId != 0 && jailSessionId != 0 && sessionId == jailSessionId ? FALSE : TRUE);
        }
    }

    // Check session (process-lifetime) blacklist
    entry = pControlDeviceContext->sessionBlacklistHead.Flink;
    while (entry != &pControlDeviceContext->sessionBlacklistHead)
    {
        sbe = CONTAINING_RECORD(entry, SESSION_BLACKLIST_ENTRY, listEntry);
        WdfStringGetUnicodeString(sbe->deviceInstancePath, &inputFilter);

        if (0 == RtlCompareUnicodeString(&inputFilter, deviceInstancePath, TRUE))
        {
            WdfWaitLockRelease(s_criticalSectionLock);
            return (TRUE);
        }

        entry = entry->Flink;
    }

    WdfWaitLockRelease(s_criticalSectionLock);

    return (FALSE);
}

_Use_decl_annotations_
NTSTATUS GetWhitelist(LPWSTR buffer, size_t bufferSizeInCharacters, size_t* neededSizeInCharacters)
{
    TRACE_ALWAYS(L"");

    WDFCOLLECTION wdfCollection;
    NTSTATUS      ntstatus;

    DECLARE_CONST_UNICODE_STRING(parameterName, DRIVER_PROPERTY_WHITELISTED_FULL_IMAGE_NAMES);
    ntstatus = HidHideDriverCreateCollectionForMultiStringProperty(&parameterName, &wdfCollection);
    if (NT_SUCCESS(ntstatus))
    {
        ntstatus = HidHideCollectionToMultiString(wdfCollection, buffer, bufferSizeInCharacters, neededSizeInCharacters);
        WdfObjectDelete(wdfCollection);
    }
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS SetWhitelist(LPWSTR buffer, size_t bufferSizeInCharacters)
{
    TRACE_ALWAYS(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    NTSTATUS                ntstatus;

    // Persist the new setting in the registry
    DECLARE_CONST_UNICODE_STRING(parameterName, DRIVER_PROPERTY_WHITELISTED_FULL_IMAGE_NAMES);
    ntstatus = HidHideDriverSetMultiStringProperty(&parameterName, buffer, bufferSizeInCharacters);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Dispose the old setting and apply the new setting
    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    WdfObjectDelete(pControlDeviceContext->whitelistedFullImageNames);
    ntstatus = HidHideDriverCreateCollectionForMultiStringProperty(&parameterName, &pControlDeviceContext->whitelistedFullImageNames);
    WdfWaitLockRelease(s_criticalSectionLock);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Flush the evaluation cache as it is no longer accurate
    HidHideProcessIdsFlushWhitelistEvaluationCache(s_criticalSectionLock);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS GetBlacklist(LPWSTR buffer, size_t bufferSizeInCharacters, size_t* neededSizeInCharacters)
{
    TRACE_ALWAYS(L"");

    WDFCOLLECTION wdfCollection;
    NTSTATUS      ntstatus;

    DECLARE_CONST_UNICODE_STRING(parameterName, DRIVER_PROPERTY_BLACKLISTED_DEVICE_INSTANCE_PATHS);
    ntstatus = HidHideDriverCreateCollectionForMultiStringProperty(&parameterName, &wdfCollection);
    if (NT_SUCCESS(ntstatus))
    {
        ntstatus = HidHideCollectionToMultiString(wdfCollection, buffer, bufferSizeInCharacters, neededSizeInCharacters);
        WdfObjectDelete(wdfCollection);
    }
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS SetBlacklist(LPWSTR buffer, size_t bufferSizeInCharacters)
{
    TRACE_ALWAYS(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    NTSTATUS                ntstatus;

    // Persist the new setting in the registry
    DECLARE_CONST_UNICODE_STRING(parameterName, DRIVER_PROPERTY_BLACKLISTED_DEVICE_INSTANCE_PATHS);
    ntstatus = HidHideDriverSetMultiStringProperty(&parameterName, buffer, bufferSizeInCharacters);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Dispose the old setting and apply the new setting
    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    WdfObjectDelete(pControlDeviceContext->blacklistedDeviceInstancePaths);
    ntstatus = HidHideDriverCreateCollectionForMultiStringProperty(&parameterName, &pControlDeviceContext->blacklistedDeviceInstancePaths);
    WdfWaitLockRelease(s_criticalSectionLock);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
BOOLEAN GetActive()
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    BOOLEAN                 active;

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    active = pControlDeviceContext->active;
    WdfWaitLockRelease(s_criticalSectionLock);

    return (active);
}

_Use_decl_annotations_
NTSTATUS SetActive(BOOLEAN active)
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    BOOLEAN                 changed;
    NTSTATUS                ntstatus;

    // Persist the new setting in the registry
    DECLARE_CONST_UNICODE_STRING(parameterName, DRIVER_PROPERTY_ACTIVE);
    ntstatus = HidHideDriverSetBooleanProperty(&parameterName, active);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Apply the new setting
    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    changed = (pControlDeviceContext->active != active);
    pControlDeviceContext->active = active;
    WdfWaitLockRelease(s_criticalSectionLock);

    // Log service active changes
    if ((changed) && (active))  LogEvent(ETW(Enabled),  L"");
    if ((changed) && (!active)) LogEvent(ETW(Disabled), L"");

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
BOOLEAN GetInverse()
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    BOOLEAN                 inverse;

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    inverse = pControlDeviceContext->whitelistedInverse;
    WdfWaitLockRelease(s_criticalSectionLock);

    return (inverse);
}

_Use_decl_annotations_
NTSTATUS SetInverse(BOOLEAN inverse)
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    BOOLEAN                 changed;
    NTSTATUS                ntstatus;

    // Persist the new setting in the registry
    DECLARE_CONST_UNICODE_STRING(parameterName, DRIVER_PROPERTY_WHITELISTED_INVERSE);
    ntstatus = HidHideDriverSetBooleanProperty(&parameterName, inverse);
    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    // Apply the new setting
    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    changed = (pControlDeviceContext->whitelistedInverse != inverse);
    pControlDeviceContext->whitelistedInverse = inverse;
    WdfWaitLockRelease(s_criticalSectionLock);

    // Log service inverse changes
    if ((changed) && (inverse))  LogEvent(ETW(Enabled), L"");
    if ((changed) && (!inverse)) LogEvent(ETW(Disabled), L"");

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID UpdateDataForControlDeviceDeletionAndDeleteControlDeviceWhenNeeded(INT32 increment)
{
    TRACE_ALWAYS(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    BOOLEAN                 deleteControlDevice;

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);

    // Memorize the fact that we are in a shutdown state
    if (0 == increment) pControlDeviceContext->shutdownPending = TRUE;

    // When we are in a shutdown state and this is the last device then we should delete the control device
    pControlDeviceContext->numberOfDevicesCreated += increment;
    deleteControlDevice = (((0 == pControlDeviceContext->numberOfDevicesCreated) && (pControlDeviceContext->shutdownPending)) ? TRUE : FALSE);
    WdfWaitLockRelease(s_criticalSectionLock);

    // Delete the control device either when
    // - we just entered the shutdown state and have no devices pending or
    // - we are in the shutdown state and the last device is deleted
    if (deleteControlDevice)
    {
        // For an unknown reason we never seem to reach this particular point in the code when testing the implementation in VM-ware
        // The cleanup of the control device is a pre-requirisite for releasing the driver itself. As the code below isn't reached, the
        // control device context cleanup isn't done (hence the reason for moving most of the cleanup code to the system shutdown event)
        // and with the control device alive the driver unload is probably also never called.
        // Deleting the control device directly from within the system shutdown notification does force a cleanup but results in a bug
        // check when Verifier is enabled. This is valid as per Microsoft documentation, the control device context should be maintained
        // till the last filter device is gone. Apparently the operating system unloads the driver itself before we reach this point.
        TRACE_ALWAYS(L"");
        WdfObjectDelete(s_wdfControlDevice);
        s_wdfControlDevice = NULL;
    }
}

// Unique memory pool tag for the application profile structures
#define PROFILES_PROFILE_TAG 'fPHH'

// Create a string object holding a copy of the value provided and parent it to the object provided
// The handle is deliberately discarded: the string object is released together with its parent
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS CreateChildString(_In_ WDFOBJECT parent, _In_ PCUNICODE_STRING value)
{
    TRACE_PERFORMANCE(L"");

    WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;
    WDFSTRING             string;

    WDF_OBJECT_ATTRIBUTES_INIT(&wdfObjectAttributes);
    wdfObjectAttributes.ParentObject = parent;
    return (WdfStringCreate(value, &wdfObjectAttributes, &string));
}

// Find a profile by name (compared ignoring case) in the control device context provided
// Returns NULL when no profile with the given name is loaded
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static PHIDHIDE_PROFILE FindProfileByName(_In_ PCONTROL_DEVICE_CONTEXT pControlDeviceContext, _In_ PCUNICODE_STRING profileName)
{
    TRACE_PERFORMANCE(L"");

    UNICODE_STRING name;

    for (PLIST_ENTRY entry = pControlDeviceContext->profilesHead.Flink; entry != &pControlDeviceContext->profilesHead; entry = entry->Flink)
    {
        PHIDHIDE_PROFILE profile = CONTAINING_RECORD(entry, HIDHIDE_PROFILE, listEntry);
        WdfStringGetUnicodeString(profile->name, &name); // PASSIVE_LEVEL
        if (0 == RtlCompareUnicodeString(&name, profileName, TRUE)) return (profile);
    }

    return (NULL);
}

// Determine the profile in effect from the activation stack (the most recently activated one, i.e. the tail)
// Returns NULL when the stack is empty (the persistent blacklist applies)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
static PHIDHIDE_PROFILE ActiveProfileFromStack(_In_ PCONTROL_DEVICE_CONTEXT pControlDeviceContext)
{
    TRACE_PERFORMANCE(L"");

    if (IsListEmpty(&pControlDeviceContext->profileActivationStackHead)) return (NULL);

    PHIDHIDE_PROFILE_ACTIVATION activation = CONTAINING_RECORD(pControlDeviceContext->profileActivationStackHead.Blink, HIDHIDE_PROFILE_ACTIVATION, listEntry);
    return (activation->profile);
}

// Does the full image name provided match one of the applications bound to the profile?
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static BOOLEAN ProfileMatchesImage(_In_ PHIDHIDE_PROFILE profile, _In_ PUNICODE_STRING fullImageName)
{
    TRACE_PERFORMANCE(L"");

    UNICODE_STRING image;

    for (ULONG index = 0, size = WdfCollectionGetCount(profile->applications); (index < size); index++)
    {
        WdfStringGetUnicodeString(WdfCollectionGetItem(profile->applications, index), &image); // PASSIVE_LEVEL
        if (0 == RtlCompareUnicodeString(&image, fullImageName, TRUE)) return (TRUE);
    }

    return (FALSE);
}

// Is the profile name provided acceptable as a registry key name?
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
static BOOLEAN IsValidProfileName(_In_ PCUNICODE_STRING profileName)
{
    TRACE_PERFORMANCE(L"");

    if ((0 == profileName->Length) || (profileName->Length > (DRIVER_PROFILE_NAME_MAX_LENGTH * sizeof(WCHAR)))) return (FALSE);

    // Reject the key-path separator as it would address a different key
    for (USHORT index = 0; index < (profileName->Length / sizeof(WCHAR)); index++)
    {
        if (L'\\' == profileName->Buffer[index]) return (FALSE);
    }

    return (TRUE);
}

// Create a profile structure from the name and the two collections of string objects provided
// On success the profile takes ownership of both collections (as well as its own name string)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS ProfileCreate(_In_ PCUNICODE_STRING profileName, _In_ WDFCOLLECTION applications, _In_ WDFCOLLECTION blacklistedDeviceInstancePaths, _Out_ PHIDHIDE_PROFILE* profile)
{
    TRACE_PERFORMANCE(L"");

    PHIDHIDE_PROFILE newProfile;
    NTSTATUS         ntstatus;

    (*profile) = NULL;

    newProfile = (PHIDHIDE_PROFILE)ExAllocatePoolWithTag(NonPagedPool, sizeof(*newProfile), PROFILES_PROFILE_TAG);
    if (NULL == newProfile) LOG_AND_RETURN_NTSTATUS(L"ExAllocatePoolWithTag", STATUS_INSUFFICIENT_RESOURCES);
    RtlZeroMemory(newProfile, sizeof(*newProfile));
    InitializeListHead(&newProfile->listEntry);

    ntstatus = WdfStringCreate(profileName, WDF_NO_OBJECT_ATTRIBUTES, &newProfile->name);
    if (!NT_SUCCESS(ntstatus))
    {
        ExFreePoolWithTag(newProfile, PROFILES_PROFILE_TAG);
        LOG_AND_RETURN_NTSTATUS(L"WdfStringCreate", ntstatus);
    }

    newProfile->applications = applications;
    newProfile->blacklistedDeviceInstancePaths = blacklistedDeviceInstancePaths;
    (*profile) = newProfile;
    return (STATUS_SUCCESS);
}

// Release a profile structure and the string objects it owns
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID ProfileFree(_In_ PHIDHIDE_PROFILE profile)
{
    TRACE_PERFORMANCE(L"");

    if (NULL != profile->name) WdfObjectDelete(profile->name);
    if (NULL != profile->applications) WdfObjectDelete(profile->applications);
    if (NULL != profile->blacklistedDeviceInstancePaths) WdfObjectDelete(profile->blacklistedDeviceInstancePaths);
    ExFreePoolWithTag(profile, PROFILES_PROFILE_TAG);
}

// Append a copy of every string object of the source collection to the destination collection
// The copies are parented to the destination collection
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS DuplicateCollectionInto(_In_ WDFCOLLECTION source, _In_ WDFCOLLECTION destination)
{
    TRACE_PERFORMANCE(L"");

    UNICODE_STRING string;
    NTSTATUS       ntstatus;

    for (ULONG index = 0, size = WdfCollectionGetCount(source); (index < size); index++)
    {
        WdfStringGetUnicodeString(WdfCollectionGetItem(source, index), &string); // PASSIVE_LEVEL
        ntstatus = CreateChildString(destination, &string);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfStringCreate", ntstatus);
    }

    return (STATUS_SUCCESS);
}

// Append a string object holding the prefix followed by the value provided to the collection
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS AddPrefixedStringToCollection(_In_ WDFCOLLECTION collection, _In_ PCWSTR prefix, _In_ PCUNICODE_STRING value)
{
    TRACE_PERFORMANCE(L"");

    UNICODE_STRING string;
    size_t         prefixInCharacters;
    size_t         valueInCharacters;
    LPWSTR         buffer;
    NTSTATUS       ntstatus;

    prefixInCharacters = wcsnlen(prefix, DRIVER_PROFILE_NAME_MAX_LENGTH);
    valueInCharacters = (value->Length / sizeof(WCHAR));

    buffer = (LPWSTR)ExAllocatePoolWithTag(NonPagedPool, ((prefixInCharacters + valueInCharacters + 1) * sizeof(WCHAR)), PROFILES_STRING_TAG);
    if (NULL == buffer) LOG_AND_RETURN_NTSTATUS(L"ExAllocatePoolWithTag", STATUS_INSUFFICIENT_RESOURCES);

    RtlCopyMemory(buffer, prefix, (prefixInCharacters * sizeof(WCHAR)));
    RtlCopyMemory(&buffer[prefixInCharacters], value->Buffer, (valueInCharacters * sizeof(WCHAR)));
    buffer[prefixInCharacters + valueInCharacters] = L'\0';

    string.Buffer = buffer;
    string.Length = (USHORT)((prefixInCharacters + valueInCharacters) * sizeof(WCHAR));
    string.MaximumLength = string.Length + sizeof(WCHAR);
    ntstatus = CreateChildString(collection, &string);
    ExFreePoolWithTag(buffer, PROFILES_STRING_TAG);

    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfStringCreate", ntstatus);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS ProfilesLoadAll(PCONTROL_DEVICE_CONTEXT pControlDeviceContext)
{
    TRACE_ALWAYS(L"");

    WDFCOLLECTION profileNames;
    UNICODE_STRING name;
    NTSTATUS      ntstatus;

    // Enumerate the stored profiles (an absent store yields an empty set which is a normal condition)
    ntstatus = HidHideProfileStoreList(&profileNames);
    if (!NT_SUCCESS(ntstatus))
    {
        // A failure to read the store must not prevent the driver from initializing
        TRACE_ALWAYS(L"Unable to enumerate the application profile store");
        return (STATUS_SUCCESS);
    }

    for (ULONG index = 0, size = WdfCollectionGetCount(profileNames); (index < size); index++)
    {
        WDFCOLLECTION    applications;
        WDFCOLLECTION    blacklistedDeviceInstancePaths;
        PHIDHIDE_PROFILE profile;

        WdfStringGetUnicodeString(WdfCollectionGetItem(profileNames, index), &name); // PASSIVE_LEVEL

        // Skip malformed or unreadable entries rather than failing the entire load
        if (STATUS_PROCESS_IN_JOB != HidHideProfileStoreRead(&name, &applications, &blacklistedDeviceInstancePaths)) continue;
        if (!NT_SUCCESS(ProfileCreate(&name, applications, blacklistedDeviceInstancePaths, &profile)))
        {
            WdfObjectDelete(applications);
            WdfObjectDelete(blacklistedDeviceInstancePaths);
            continue;
        }

        InsertTailList(&pControlDeviceContext->profilesHead, &profile->listEntry);
    }

    WdfObjectDelete(profileNames);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID ProfilesFreeAll(PCONTROL_DEVICE_CONTEXT pControlDeviceContext)
{
    TRACE_ALWAYS(L"");

    if (NULL != s_criticalSectionLock) WdfWaitLockAcquire(s_criticalSectionLock, NULL);

    // Release the activation entries
    PLIST_ENTRY entry = pControlDeviceContext->profileActivationStackHead.Flink;
    while (entry != &pControlDeviceContext->profileActivationStackHead)
    {
        PHIDHIDE_PROFILE_ACTIVATION activation = CONTAINING_RECORD(entry, HIDHIDE_PROFILE_ACTIVATION, listEntry);
        PLIST_ENTRY next = entry->Flink;
        RemoveEntryList(entry);
        ExFreePoolWithTag(activation, PROFILES_ACTIVATION_TAG);
        entry = next;
    }

    // Release the profiles
    entry = pControlDeviceContext->profilesHead.Flink;
    while (entry != &pControlDeviceContext->profilesHead)
    {
        PHIDHIDE_PROFILE profile = CONTAINING_RECORD(entry, HIDHIDE_PROFILE, listEntry);
        PLIST_ENTRY next = entry->Flink;
        RemoveEntryList(entry);
        ProfileFree(profile);
        entry = next;
    }

    pControlDeviceContext->activeProfile = NULL;

    if (NULL != s_criticalSectionLock) WdfWaitLockRelease(s_criticalSectionLock);
}

_Use_decl_annotations_
VOID ProfilesTryActivate(HANDLE processId, PUNICODE_STRING fullImageName)
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;

    if (NULL == s_wdfControlDevice) return;

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);

    for (PLIST_ENTRY entry = pControlDeviceContext->profilesHead.Flink; entry != &pControlDeviceContext->profilesHead; entry = entry->Flink)
    {
        PHIDHIDE_PROFILE profile = CONTAINING_RECORD(entry, HIDHIDE_PROFILE, listEntry);
        if (!ProfileMatchesImage(profile, fullImageName)) continue;

        // Bind the process to the profile and make it the profile in effect (last-started-wins)
        PHIDHIDE_PROFILE_ACTIVATION activation = (PHIDHIDE_PROFILE_ACTIVATION)ExAllocatePoolWithTag(NonPagedPool, sizeof(*activation), PROFILES_ACTIVATION_TAG);
        if (NULL != activation)
        {
            activation->processId = processId;
            activation->profile = profile;
            InsertTailList(&pControlDeviceContext->profileActivationStackHead, &activation->listEntry);
            pControlDeviceContext->activeProfile = profile;
            TRACE_ALWAYS(L"Application profile activated");
        }
        break;
    }

    WdfWaitLockRelease(s_criticalSectionLock);
}

_Use_decl_annotations_
VOID ProfilesDeactivateForPid(HANDLE processId)
{
    TRACE_PERFORMANCE(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;

    if (NULL == s_wdfControlDevice) return;

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);

    PLIST_ENTRY entry = pControlDeviceContext->profileActivationStackHead.Flink;
    while (entry != &pControlDeviceContext->profileActivationStackHead)
    {
        PHIDHIDE_PROFILE_ACTIVATION activation = CONTAINING_RECORD(entry, HIDHIDE_PROFILE_ACTIVATION, listEntry);
        PLIST_ENTRY next = entry->Flink;
        if (activation->processId == processId)
        {
            RemoveEntryList(entry);
            ExFreePoolWithTag(activation, PROFILES_ACTIVATION_TAG);
        }
        entry = next;
    }

    pControlDeviceContext->activeProfile = ActiveProfileFromStack(pControlDeviceContext);

    WdfWaitLockRelease(s_criticalSectionLock);
}

_Use_decl_annotations_
VOID ProfilesPurgeByName(PCUNICODE_STRING profileName)
{
    TRACE_ALWAYS(L"");

    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    PHIDHIDE_PROFILE         target;

    // Remove the stored definition first (PASSIVE_LEVEL registry access)
    HidHideProfileStoreDelete(profileName);

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    target = FindProfileByName(pControlDeviceContext, profileName);

    if (NULL != target)
    {
        // Drop the activation entries referencing the profile and release the profile itself
        PLIST_ENTRY entry = pControlDeviceContext->profileActivationStackHead.Flink;
        while (entry != &pControlDeviceContext->profileActivationStackHead)
        {
            PHIDHIDE_PROFILE_ACTIVATION activation = CONTAINING_RECORD(entry, HIDHIDE_PROFILE_ACTIVATION, listEntry);
            PLIST_ENTRY next = entry->Flink;
            if (activation->profile == target)
            {
                RemoveEntryList(entry);
                ExFreePoolWithTag(activation, PROFILES_ACTIVATION_TAG);
            }
            entry = next;
        }

        RemoveEntryList(&target->listEntry);
        ProfileFree(target);
        pControlDeviceContext->activeProfile = ActiveProfileFromStack(pControlDeviceContext);
    }

    WdfWaitLockRelease(s_criticalSectionLock);
}

_Use_decl_annotations_
NTSTATUS ProfilesGetNames(LPWSTR buffer, size_t bufferSizeInCharacters, size_t* neededSizeInCharacters)
{
    TRACE_ALWAYS(L"");

    WDFCOLLECTION         names;
    PCONTROL_DEVICE_CONTEXT pControlDeviceContext;
    UNICODE_STRING        name;
    NTSTATUS              ntstatus;

    ntstatus = WdfCollectionCreate(NULL, &names);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    for (PLIST_ENTRY entry = pControlDeviceContext->profilesHead.Flink; entry != &pControlDeviceContext->profilesHead; entry = entry->Flink)
    {
        PHIDHIDE_PROFILE profile = CONTAINING_RECORD(entry, HIDHIDE_PROFILE, listEntry);
        WdfStringGetUnicodeString(profile->name, &name); // PASSIVE_LEVEL
        ntstatus = CreateChildString(names, &name);
        if (!NT_SUCCESS(ntstatus)) break;
    }
    WdfWaitLockRelease(s_criticalSectionLock);

    if (NT_SUCCESS(ntstatus)) ntstatus = HidHideCollectionToMultiString(names, buffer, bufferSizeInCharacters, neededSizeInCharacters);
    WdfObjectDelete(names);

    return (ntstatus);
}

_Use_decl_annotations_
NTSTATUS ProfilesGetActiveName(LPWSTR buffer, size_t bufferSizeInCharacters, size_t* neededSizeInCharacters)
{
    TRACE_ALWAYS(L"");

    WDFCOLLECTION            entries;
    PCONTROL_DEVICE_CONTEXT  pControlDeviceContext;
    UNICODE_STRING           name;
    NTSTATUS                 ntstatus;

    ntstatus = WdfCollectionCreate(NULL, &entries);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    if (NULL != pControlDeviceContext->activeProfile)
    {
        WdfStringGetUnicodeString(pControlDeviceContext->activeProfile->name, &name); // PASSIVE_LEVEL
        ntstatus = CreateChildString(entries, &name);
    }
    WdfWaitLockRelease(s_criticalSectionLock);

    if (NT_SUCCESS(ntstatus)) ntstatus = HidHideCollectionToMultiString(entries, buffer, bufferSizeInCharacters, neededSizeInCharacters);
    WdfObjectDelete(entries);

    return (ntstatus);
}

_Use_decl_annotations_
NTSTATUS ProfilesGetOne(PCUNICODE_STRING profileName, LPWSTR buffer, size_t bufferSizeInCharacters, size_t* neededSizeInCharacters)
{
    TRACE_ALWAYS(L"");

    WDFCOLLECTION            entries;
    PCONTROL_DEVICE_CONTEXT  pControlDeviceContext;
    PHIDHIDE_PROFILE         profile;
    UNICODE_STRING           name;
    UNICODE_STRING           value;
    NTSTATUS                 ntstatus;

    ntstatus = WdfCollectionCreate(NULL, &entries);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);

    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    profile = FindProfileByName(pControlDeviceContext, profileName);

    if (NULL != profile)
    {
        // The profile name comes first, then the entries tagged as application or device
        WdfStringGetUnicodeString(profile->name, &name); // PASSIVE_LEVEL
        ntstatus = CreateChildString(entries, &name);

        for (ULONG index = 0; NT_SUCCESS(ntstatus) && (index < WdfCollectionGetCount(profile->applications)); index++)
        {
            WdfStringGetUnicodeString(WdfCollectionGetItem(profile->applications, index), &value);
            ntstatus = AddPrefixedStringToCollection(entries, DRIVER_PROFILE_ENTRY_APPLICATION_PREFIX, &value);
        }
        for (ULONG index = 0; NT_SUCCESS(ntstatus) && (index < WdfCollectionGetCount(profile->blacklistedDeviceInstancePaths)); index++)
        {
            WdfStringGetUnicodeString(WdfCollectionGetItem(profile->blacklistedDeviceInstancePaths, index), &value);
            ntstatus = AddPrefixedStringToCollection(entries, DRIVER_PROFILE_ENTRY_DEVICE_PREFIX, &value);
        }
    }
    WdfWaitLockRelease(s_criticalSectionLock);

    if (NULL == profile)
    {
        WdfObjectDelete(entries);
        LOG_AND_RETURN_NTSTATUS(L"FindProfileByName", STATUS_OBJECT_NAME_NOT_FOUND);
    }

    if (NT_SUCCESS(ntstatus)) ntstatus = HidHideCollectionToMultiString(entries, buffer, bufferSizeInCharacters, neededSizeInCharacters);
    WdfObjectDelete(entries);

    return (ntstatus);
}

_Use_decl_annotations_
NTSTATUS ProfilesSetOne(PCUNICODE_STRING profileName, WDFCOLLECTION applications, WDFCOLLECTION blacklistedDeviceInstancePaths)
{
    TRACE_ALWAYS(L"");

    WDFCOLLECTION            dupApplications;
    WDFCOLLECTION            dupBlacklistedDeviceInstancePaths;
    PCONTROL_DEVICE_CONTEXT  pControlDeviceContext;
    PHIDHIDE_PROFILE         newProfile;
    PHIDHIDE_PROFILE         existing;
    NTSTATUS                 ntstatus;

    // Build profile-owned copies of the supplied sets (the caller retains ownership of the originals)
    ntstatus = WdfCollectionCreate(NULL, &dupApplications);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);
    ntstatus = DuplicateCollectionInto(applications, dupApplications);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfObjectDelete(dupApplications);
        return (ntstatus);
    }
    ntstatus = WdfCollectionCreate(NULL, &dupBlacklistedDeviceInstancePaths);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfObjectDelete(dupApplications);
        LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);
    }
    ntstatus = DuplicateCollectionInto(blacklistedDeviceInstancePaths, dupBlacklistedDeviceInstancePaths);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfObjectDelete(dupBlacklistedDeviceInstancePaths);
        WdfObjectDelete(dupApplications);
        return (ntstatus);
    }

    // Build the profile structure upfront so no allocation happens while holding the lock
    ntstatus = ProfileCreate(profileName, dupApplications, dupBlacklistedDeviceInstancePaths, &newProfile);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfObjectDelete(dupBlacklistedDeviceInstancePaths);
        WdfObjectDelete(dupApplications);
        return (ntstatus);
    }

    // Persist the definition
    ntstatus = HidHideProfileStoreWrite(profileName, applications, blacklistedDeviceInstancePaths);
    if (!NT_SUCCESS(ntstatus))
    {
        ProfileFree(newProfile);
        return (ntstatus);
    }

    // Apply the change in memory
    WdfWaitLockAcquire(s_criticalSectionLock, NULL);
    pControlDeviceContext = ControlDeviceGetContext(s_wdfControlDevice);
    existing = FindProfileByName(pControlDeviceContext, profileName);

    if (NULL != existing)
    {
        // Replace the contents in place so any activation binding keeps referencing a live profile
        if (NULL != existing->applications) WdfObjectDelete(existing->applications);
        if (NULL != existing->blacklistedDeviceInstancePaths) WdfObjectDelete(existing->blacklistedDeviceInstancePaths);
        existing->applications = newProfile->applications;
        existing->blacklistedDeviceInstancePaths = newProfile->blacklistedDeviceInstancePaths;

        // The freshly created name string is redundant for the replace case
        WdfObjectDelete(newProfile->name);
        ExFreePoolWithTag(newProfile, PROFILES_PROFILE_TAG);
    }
    else
    {
        InsertTailList(&pControlDeviceContext->profilesHead, &newProfile->listEntry);
    }
    WdfWaitLockRelease(s_criticalSectionLock);

    return (STATUS_SUCCESS);
}

// Parse the first string of a multi-string buffer (the buffer has been validated to be well-formed)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static VOID ParseFirstMultiString(_In_reads_(totalCharacters) LPWSTR buffer, _In_ size_t totalCharacters, _Out_ PUNICODE_STRING value)
{
    TRACE_PERFORMANCE(L"");

    size_t length = wcsnlen(buffer, totalCharacters);
    value->Buffer = buffer;
    value->Length = (USHORT)(length * sizeof(WCHAR));
    value->MaximumLength = (USHORT)((length + 1) * sizeof(WCHAR));
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoGetProfiles(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR   buffer;
    size_t   neededSizeInCharacters;
    NTSTATUS ntstatus;

    if (0 != inputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if (0 == outputBufferLength)
    {
        ntstatus = ProfilesGetNames(NULL, 0, &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }
    else
    {
        ntstatus = WdfRequestRetrieveOutputBuffer(wdfRequest, outputBufferLength, &buffer, NULL);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
        ntstatus = ProfilesGetNames(buffer, (outputBufferLength / sizeof(WCHAR)), &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, (neededSizeInCharacters * sizeof(WCHAR)));
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoGetProfile(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR         buffer;
    size_t         totalChars;
    UNICODE_STRING profileName;
    size_t         neededSizeInCharacters;
    NTSTATUS       ntstatus;

    // Input is a multi-string holding the profile name
    if (0 == inputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if ((inputBufferLength < (2 * sizeof(WCHAR))) || (0 != (inputBufferLength % sizeof(WCHAR)))) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveInputBuffer", ntstatus);
    totalChars = inputBufferLength / sizeof(WCHAR);
    ParseFirstMultiString(buffer, totalChars, &profileName);
    if (!IsValidProfileName(&profileName)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);

    if (0 == outputBufferLength)
    {
        ntstatus = ProfilesGetOne(&profileName, NULL, 0, &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }
    else
    {
        ntstatus = WdfRequestRetrieveOutputBuffer(wdfRequest, outputBufferLength, &buffer, NULL);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
        ntstatus = ProfilesGetOne(&profileName, buffer, (outputBufferLength / sizeof(WCHAR)), &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, (neededSizeInCharacters * sizeof(WCHAR)));
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoSetProfile(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    WDFCOLLECTION            applications;
    WDFCOLLECTION            blacklistedDeviceInstancePaths;
    LPWSTR                   buffer;
    LPWSTR                   current;
    size_t                   totalChars;
    UNICODE_STRING           profileName;
    NTSTATUS                 ntstatus;

    if (0 != outputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if ((inputBufferLength < (2 * sizeof(WCHAR))) || (0 != (inputBufferLength % sizeof(WCHAR)))) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveInputBuffer", ntstatus);

    // Verify the buffer ends with a double-NUL as required by MULTI_SZ format
    totalChars = inputBufferLength / sizeof(WCHAR);
    if ((buffer[totalChars - 1] != L'\0') || (buffer[totalChars - 2] != L'\0')) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);

    // The first string is the profile name
    ParseFirstMultiString(buffer, totalChars, &profileName);
    if (!IsValidProfileName(&profileName)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);

    ntstatus = WdfCollectionCreate(NULL, &applications);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);
    ntstatus = WdfCollectionCreate(NULL, &blacklistedDeviceInstancePaths);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfObjectDelete(applications);
        LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);
    }

    // Walk the remaining strings, dispatching each one on its tag prefix
    ntstatus = STATUS_SUCCESS;
    current = buffer;
    while (NT_SUCCESS(ntstatus) && (*current != L'\0'))
    {
        size_t   remaining = totalChars - (size_t)(current - buffer);
        size_t   len = wcsnlen(current, remaining);
        WCHAR    prefix;
        WDFCOLLECTION target;

        if (len >= remaining)
        {
            ntstatus = STATUS_INVALID_PARAMETER;
            break;
        }
        if ((current == buffer) || (len < 2) || (current[1] != L':'))
        {
            // Skip the profile name (first string) and reject malformed entries
            if (current != buffer) ntstatus = STATUS_INVALID_PARAMETER;
            current += len + 1;
            continue;
        }

        prefix = current[0];
        if ((prefix == L'A') || (prefix == L'a')) target = applications;
        else if ((prefix == L'D') || (prefix == L'd')) target = blacklistedDeviceInstancePaths;
        else { ntstatus = STATUS_INVALID_PARAMETER; break; }

        UNICODE_STRING value;
        value.Buffer = &current[2];
        value.Length = (USHORT)((len - 2) * sizeof(WCHAR));
        value.MaximumLength = value.Length + sizeof(WCHAR);
        ntstatus = CreateChildString(target, &value);

        current += len + 1;
    }

    if (NT_SUCCESS(ntstatus)) ntstatus = ProfilesSetOne(&profileName, applications, blacklistedDeviceInstancePaths);

    WdfObjectDelete(blacklistedDeviceInstancePaths);
    WdfObjectDelete(applications);

    if (!NT_SUCCESS(ntstatus)) return (ntstatus);

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, inputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoDelProfile(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR         buffer;
    size_t         totalChars;
    UNICODE_STRING profileName;
    NTSTATUS       ntstatus;

    if (0 != outputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if ((inputBufferLength < (2 * sizeof(WCHAR))) || (0 != (inputBufferLength % sizeof(WCHAR)))) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    ntstatus = WdfRequestRetrieveInputBuffer(wdfRequest, inputBufferLength, &buffer, NULL);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveInputBuffer", ntstatus);
    totalChars = inputBufferLength / sizeof(WCHAR);
    ParseFirstMultiString(buffer, totalChars, &profileName);
    if (!IsValidProfileName(&profileName)) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);

    ProfilesPurgeByName(&profileName);

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, inputBufferLength);
    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
NTSTATUS OnControlDeviceIoGetActiveProfile(WDFDEVICE wdfControlDevice, WDFQUEUE wdfQueue, WDFREQUEST wdfRequest, size_t outputBufferLength, size_t inputBufferLength, ULONG ioControlCode)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wdfControlDevice);
    UNREFERENCED_PARAMETER(wdfQueue);
    UNREFERENCED_PARAMETER(ioControlCode);

    LPWSTR   buffer;
    size_t   neededSizeInCharacters;
    NTSTATUS ntstatus;

    if (0 != inputBufferLength) LOG_AND_RETURN_NTSTATUS(L"Validation", STATUS_INVALID_PARAMETER);
    if (0 == outputBufferLength)
    {
        ntstatus = ProfilesGetActiveName(NULL, 0, &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }
    else
    {
        ntstatus = WdfRequestRetrieveOutputBuffer(wdfRequest, outputBufferLength, &buffer, NULL);
        if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRequestRetrieveOutputBuffer", ntstatus);
        ntstatus = ProfilesGetActiveName(buffer, (outputBufferLength / sizeof(WCHAR)), &neededSizeInCharacters);
        if (!NT_SUCCESS(ntstatus)) return (ntstatus);
    }

    WdfRequestCompleteWithInformation(wdfRequest, STATUS_SUCCESS, (neededSizeInCharacters * sizeof(WCHAR)));
    return (STATUS_SUCCESS);
}
