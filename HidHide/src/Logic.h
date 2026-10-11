// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// Logic.h
#pragma once

// Undocumented ntddk function that allows easy access to the session ID field in the opaque EPROCESS structure
ULONG PsGetProcessSessionId(PEPROCESS process);

#define DEVICE_HARDWARE_ID                                L"root\\HidHide"
#define CONTROL_DEVICE_NT_DEVICE_NAME                     L"\\Device\\HidHide"
#define CONTROL_DEVICE_DOS_DEVICE_NAME                    L"\\DosDevices\\HidHide"
#define DRIVER_PROPERTY_WHITELISTED_FULL_IMAGE_NAMES      L"WhitelistedFullImageNames"      // HKLM\SYSTEM\CurrentControlSet\Services\HidHide\Parameters\WhitelistedFullImageNames (REG_MULTI_Z)
#define DRIVER_PROPERTY_BLACKLISTED_DEVICE_INSTANCE_PATHS L"BlacklistedDeviceInstancePaths" // HKLM\SYSTEM\CurrentControlSet\Services\HidHide\Parameters\BlacklistedDeviceInstancePaths (REG_MULTI_Z)
#define DRIVER_PROPERTY_ACTIVE                            L"Active"                         // HKLM\SYSTEM\CurrentControlSet\Services\HidHide\Parameters\Active (DWORD)
#define DRIVER_PROPERTY_WHITELISTED_INVERSE               L"WhitelistedInverse"             // HKLM\SYSTEM\CurrentControlSet\Services\HidHide\Parameters\WhitelistedInverse (DWORD)

#include "HidHideIoctlContract.h"
#include "Profiles.h"

// {0C320FF7-BD9B-42B6-BDAF-49FEB9C91649}
DEFINE_GUID(HidHideInterfaceGuid, 0xc320ff7, 0xbd9b, 0x42b6, 0xbd, 0xaf, 0x49, 0xfe, 0xb9, 0xc9, 0x16, 0x49);

// The administration maintained per device (0 .. *)
typedef struct _DEVICE_CONTEXT
{
    // The unique device instance path of this device, suitable as input for CreateFile
    WDFSTRING deviceInstancePath;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

// The administration shared by all devices (0 .. 1)
typedef struct _CONTROL_DEVICE_CONTEXT
{
    // Collection of string objects containing the full image names of applications that will be granted access to blacklisted human interface devices
    WDFCOLLECTION whitelistedFullImageNames;

    // Collection of string objects containing the device instance paths of the human interface devices (HID) that are subject to access control
    WDFCOLLECTION blacklistedDeviceInstancePaths;

    // The device active (enabled) state
    BOOLEAN active;

    // The whitelisted inverse (enabled) state
    BOOLEAN whitelistedInverse;

    // Collection of SESSION_BLACKLIST_ENTRY structures for process-lifetime blacklist entries
    // Entries are automatically removed when the registering process exits
    LIST_ENTRY sessionBlacklistHead;

    // Collection of HIDHIDE_PROFILE structures loaded from the registry (the stored application profiles)
    LIST_ENTRY profilesHead;

    // Stack of HIDHIDE_PROFILE_ACTIVATION structures binding a running process to the profile it activated
    // The tail of the list holds the most recently activated profile (last-started-wins)
    LIST_ENTRY profileActivationStackHead;

    // The profile currently in effect (the tail of the activation stack) or NULL when the persistent blacklist applies
    struct _HIDHIDE_PROFILE* activeProfile;

    // During a shutdown we may only delete the control device object after the last device is removed so keep track of the number of devices and shutdown state
    BOOLEAN shutdownPending;
    INT32 numberOfDevicesCreated;
} CONTROL_DEVICE_CONTEXT, *PCONTROL_DEVICE_CONTEXT;

// An entry in the process-lifetime (session) blacklist
typedef struct _SESSION_BLACKLIST_ENTRY
{
    LIST_ENTRY  listEntry;
    HANDLE      ownerPid;
    WDFSTRING   deviceInstancePath;
} SESSION_BLACKLIST_ENTRY, *PSESSION_BLACKLIST_ENTRY;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_CONTEXT, ControlDeviceGetContext)

EXTERN_C_START

// Notifies the driver that the system is about to lose its power
EVT_WDF_DEVICE_SHUTDOWN_NOTIFICATION OnSystemShutdown;

// Notification handler called on the system-wide creation and deletion of (any) processes
CREATE_PROCESS_NOTIFY_ROUTINE OnSystemProcessChange;

// Notification handler called whenever (any) image is loaded (or mapped into memory)
LOAD_IMAGE_NOTIFY_ROUTINE OnSystemLoadImage;

// Notification handler called when either the framework or a driver attempts to delete the device object
EVT_WDF_DEVICE_CONTEXT_CLEANUP OnDeviceContextCleanup;

// Notification handler called when a user application or another driver opens the device to perform an I/O operation, such as reading or writing a file
EVT_WDF_DEVICE_FILE_CREATE OnDeviceFileCreate;

// Notification handler called when another driver completes a specified I/O request
EVT_WDF_REQUEST_COMPLETION_ROUTINE OnDeviceFileCreateRequestCompletion;

// Notification handler called when the last handle to the specified file object has been closed
EVT_WDF_FILE_CLEANUP OnDeviceFileCleanup;

// Notification handler called when either the framework or a driver attempts to delete the device object
EVT_WDF_DEVICE_CONTEXT_CLEANUP OnControlDeviceContextCleanup;

// Notification handler called when a user application or another driver opens the device to perform an I/O operation, such as reading or writing a file
EVT_WDF_DEVICE_FILE_CREATE OnControlDeviceFileCreate;

// Notification handler called when the last handle to the specified file object has been closed
EVT_WDF_FILE_CLEANUP OnControlDeviceFileCleanup;

// Hook called after having the driver created
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS OnDriverCreate(_In_ WDFDRIVER wdfDriver);

// Hook called after having a device created
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS OnDeviceCreate(_In_ WDFDEVICE wdfDevice);

// Hook called after having the control device created
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS OnControlDeviceCreate(_In_ WDFDEVICE wdfControlDevice);

// Hook called when a device I/O request for the control device is incoming
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoDeviceControl(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle GetWhitelist I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoGetWhitelist(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle SetWhitelist I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoSetWhitelist(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle GetBlacklist I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoGetBlacklist(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle SetBlacklist I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoSetBlacklist(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle GetActive I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoGetActive(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle SetActive I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoSetActive(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle GetInverse I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoGetInverse(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle SetInverse I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoSetInverse(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle AddSessionBlacklist I/O request — adds device instance paths to a process-lifetime blacklist
// Entries are automatically removed when the calling process exits (clean or crash)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoAddSessionBlacklist(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle ClearSessionBlacklist I/O request — removes all session blacklist entries for the calling process
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoClearSessionBlacklist(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle GetProfiles I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoGetProfiles(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle GetProfile I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoGetProfile(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle SetProfile I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoSetProfile(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle DelProfile I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoDelProfile(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Handle GetActiveProfile I/O request from client
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS OnControlDeviceIoGetActiveProfile(_In_ WDFDEVICE wdfDevice, _In_ WDFQUEUE wdfQueue, _In_ WDFREQUEST wdfRequest, _In_ size_t outputBufferLength, _In_ size_t inputBufferLength, _In_ ULONG ioControlCode);

// Remove all session blacklist entries owned by the given process ID
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID SessionBlacklistCleanupForPid(_In_ HANDLE processId);

// Load all stored application profiles into the control device context
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS ProfilesLoadAll(_In_ PCONTROL_DEVICE_CONTEXT pControlDeviceContext);

// Release all application profiles and activation entries held by the control device context
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID ProfilesFreeAll(_In_ PCONTROL_DEVICE_CONTEXT pControlDeviceContext);

// Activate the profile bound to the full image name provided (when any), binding it to the process id
// A later activation takes precedence over an earlier one (last-started-wins)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID ProfilesTryActivate(_In_ HANDLE processId, _In_ PUNICODE_STRING fullImageName);

// Remove all activation entries owned by the given process ID and recompute the active profile
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID ProfilesDeactivateForPid(_In_ HANDLE processId);

// Remove a profile by name: its activation entries, its in-memory copy, and its stored definition
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID ProfilesPurgeByName(_In_ PCUNICODE_STRING profileName);

// Get the names of all loaded profiles in a multi-string format
// When the supplied buffer is NULL, the method returns STATUS_SUCCESS and indicates the buffer size needed for the multi-string (incl. terminator)
// When the supplied buffer isn't NULL, the names will be copied into the buffer, providing the buffer is large enough for holding the result
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS ProfilesGetNames(_Out_writes_to_opt_(bufferSizeInCharacters, *neededSizeInCharacters) LPWSTR buffer, _In_ size_t bufferSizeInCharacters, _Out_ size_t* neededSizeInCharacters);

// Get a single profile in a multi-string format (name first, then the entries tagged with DRIVER_PROFILE_ENTRY_APPLICATION_PREFIX / DRIVER_PROFILE_ENTRY_DEVICE_PREFIX)
// When the supplied buffer is NULL, the method returns STATUS_SUCCESS and indicates the buffer size needed for the multi-string (incl. terminator)
// Returns STATUS_OBJECT_NAME_NOT_FOUND (Error) when the profile doesn't exist
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS ProfilesGetOne(_In_ PCUNICODE_STRING profileName, _Out_writes_to_opt_(bufferSizeInCharacters, *neededSizeInCharacters) LPWSTR buffer, _In_ size_t bufferSizeInCharacters, _Out_ size_t* neededSizeInCharacters);

// Create or replace a profile from the two collections of string objects provided (applications and blacklisted device instance paths)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS ProfilesSetOne(_In_ PCUNICODE_STRING profileName, _In_ WDFCOLLECTION applications, _In_ WDFCOLLECTION blacklistedDeviceInstancePaths);

// Get the name of the currently active profile (empty when none) in a multi-string format
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS ProfilesGetActiveName(_Out_writes_to_opt_(bufferSizeInCharacters, *neededSizeInCharacters) LPWSTR buffer, _In_ size_t bufferSizeInCharacters, _Out_ size_t* neededSizeInCharacters);


// Is the process id on the whitelist?
// On a match, the cache-hit indicates if its the first time or not
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN Whitelisted(_In_ HANDLE processId, _Out_ BOOLEAN* cacheHit);

// Is this device instance on the blacklist?
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN Blacklisted(_In_ PUNICODE_STRING deviceInstancePath, ULONG sessionId);

// Get the whitelist in a multi-string format
// When the supplied buffer is NULL, the method returns STATUS_SUCCESS and indicates the buffer size needed for the multi-string (incl. terminator)
// When the supplied buffer isn't NULL, the list will be copied into the buffer, providing the buffer is large enough for holding the result
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS GetWhitelist(_Out_writes_to_opt_(bufferSizeInCharacters, *neededSizeInCharacters) LPWSTR buffer, _In_ size_t bufferSizeInCharacters, _Out_ size_t* neededSizeInCharacters);

// Set the whitelist in a multi-string format
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS SetWhitelist(_In_reads_(bufferSizeInCharacters) LPWSTR buffer, _In_ size_t bufferSizeInCharacters);

// Get the blacklist in a multi-string format
// When the supplied buffer is NULL, the method returns STATUS_SUCCESS and indicates the buffer size needed for the multi-string (incl. terminator)
// When the supplied buffer isn't NULL, the list will be copied into the buffer, providing the buffer is large enough for holding the result
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS GetBlacklist(_Out_writes_to_opt_(bufferSizeInCharacters, *neededSizeInCharacters) LPWSTR buffer, _In_ size_t bufferSizeInCharacters, _Out_ size_t* neededSizeInCharacters);

// Set the blacklist in a multi-string format
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS SetBlacklist(_In_reads_(bufferSizeInCharacters) LPWSTR buffer, _In_ size_t bufferSizeInCharacters);

// Get the active state (enable/disable service)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN GetActive();

// Set the active state (enable/disable service)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS SetActive(_In_ BOOLEAN active);

// Get the inverse state (enable/disable whitelist inverse)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN GetInverse();

// Set the inverse state (enable/disable whitelist inverse)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS SetInverse(_In_ BOOLEAN inverse);

// Update data for control device deletion and delete the control device when needed
// A positive number indicates that we have one or more new filter devices created
// A negative number indicates the number of filter devices destroyed
// A value of zero indicates that we entered the shutdown state
// In the shutdown state, the control device is deleted when there are no filter devices left
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID UpdateDataForControlDeviceDeletionAndDeleteControlDeviceWhenNeeded(_In_ INT32 increment);

EXTERN_C_END
