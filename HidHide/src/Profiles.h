// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// Profiles.h
#pragma once

EXTERN_C_START

// The subkey holding all application profiles below the filter driver parameters key
#define DRIVER_PROFILE_STORE_SUBKEY L"Profiles" // HKLM\SYSTEM\CurrentControlSet\Services\HidHide\Parameters\Profiles

// The registry values that make up a single application profile (both REG_MULTI_SZ)
#define DRIVER_PROFILE_VALUE_APPLICATIONS                       L"Applications"                       // The full image names bound to the profile
#define DRIVER_PROFILE_VALUE_BLACKLISTED_DEVICE_INSTANCE_PATHS  L"BlacklistedDeviceInstancePaths"     // The devices hidden while the profile is active

// The wire-format prefixes used to tag entries of a profile in the multi-string exchanged over the I/O control interface
#define DRIVER_PROFILE_ENTRY_APPLICATION_PREFIX L"A:"
#define DRIVER_PROFILE_ENTRY_DEVICE_PREFIX      L"D:"

// The maximum number of characters allowed in a profile name
#define DRIVER_PROFILE_NAME_MAX_LENGTH 128

// An application profile: a named set of applications whose launch activates a specific set of hidden devices
typedef struct _HIDHIDE_PROFILE
{
    LIST_ENTRY     listEntry;
    WDFSTRING      name;                            // The profile name (unique, compared ignoring case)
    WDFCOLLECTION  applications;                    // Collection of string objects holding the bound full image names
    WDFCOLLECTION  blacklistedDeviceInstancePaths;  // Collection of string objects holding the devices hidden while active
} HIDHIDE_PROFILE, *PHIDHIDE_PROFILE;

// An entry on the profile activation stack, binding a running process to the profile it activated
typedef struct _HIDHIDE_PROFILE_ACTIVATION
{
    LIST_ENTRY       listEntry;
    HANDLE           processId;
    PHIDHIDE_PROFILE profile;
} HIDHIDE_PROFILE_ACTIVATION, *PHIDHIDE_PROFILE_ACTIVATION;

// Enumerate the names of all stored profiles into a collection of string objects
// When no profiles are stored, the collection is returned empty
// On success the caller becomes responsible for calling WdfObjectDelete on the collection
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS HidHideProfileStoreList(_Out_ WDFCOLLECTION* profileNames);

// Read a stored profile into two collections of string objects (applications and blacklisted device instance paths)
// Returns STATUS_PROCESS_IN_JOB (Success) when the profile exists and was read
// Returns STATUS_PROCESS_NOT_IN_JOB (Success) when the profile doesn't exist
// On success the caller becomes responsible for calling WdfObjectDelete on both collections
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS HidHideProfileStoreRead(_In_ PCUNICODE_STRING profileName, _Out_ WDFCOLLECTION* applications, _Out_ WDFCOLLECTION* blacklistedDeviceInstancePaths);

// Create or replace a stored profile from the two collections of string objects provided
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS HidHideProfileStoreWrite(_In_ PCUNICODE_STRING profileName, _In_ WDFCOLLECTION applications, _In_ WDFCOLLECTION blacklistedDeviceInstancePaths);

// Delete a stored profile
// When the profile isn't stored, the operation is ignored silently
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID HidHideProfileStoreDelete(_In_ PCUNICODE_STRING profileName);

EXTERN_C_END
