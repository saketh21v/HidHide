// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// Profiles.c
#include "stdafx.h"
#include "Profiles.h"
#include "Logging.h"

// Unique memory pool tag for the registry enumeration buffer
#define PROFILES_TAG 'pFHH'

// Open the filter driver parameters key (the parent of the profile store)
// On success the caller becomes responsible for calling WdfRegistryClose on the key
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS OpenParametersKey(_Out_ WDFKEY* parametersKey)
{
    TRACE_ALWAYS(L"");
    return (WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_ALL_ACCESS, WDF_NO_OBJECT_ATTRIBUTES, parametersKey)); // PASSIVE_LEVEL
}

// Open the subkey holding all application profiles
// Returns STATUS_OBJECT_NAME_NOT_FOUND (Error) when the store hasn't been created yet
// On success the caller becomes responsible for calling WdfRegistryClose on the key
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS OpenProfileStoreKey(_In_ WDFKEY parametersKey, _Out_ WDFKEY* profileStoreKey)
{
    TRACE_ALWAYS(L"");

    DECLARE_CONST_UNICODE_STRING(profileStoreSubKey, DRIVER_PROFILE_STORE_SUBKEY);
    return (WdfRegistryOpenKey(parametersKey, &profileStoreSubKey, KEY_ALL_ACCESS, WDF_NO_OBJECT_ATTRIBUTES, profileStoreKey)); // PASSIVE_LEVEL
}

// Create (or open) the subkey holding all application profiles
// On success the caller becomes responsible for calling WdfRegistryClose on the key
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS CreateProfileStoreKey(_In_ WDFKEY parametersKey, _Out_ WDFKEY* profileStoreKey)
{
    TRACE_ALWAYS(L"");

    DECLARE_CONST_UNICODE_STRING(profileStoreSubKey, DRIVER_PROFILE_STORE_SUBKEY);
    return (WdfRegistryCreateKey(parametersKey, &profileStoreSubKey, KEY_ALL_ACCESS, REG_OPTION_NON_VOLATILE, WDF_NO_OBJECT_ATTRIBUTES, profileStoreKey)); // PASSIVE_LEVEL
}

// Create a collection of string objects from a REG_MULTI_SZ value of a given key
// Returns STATUS_PROCESS_IN_JOB (Success) when the value is available and converted into a collection
// Returns STATUS_PROCESS_NOT_IN_JOB (Success) when the value isn't available (the collection returned is empty)
// On success the caller becomes responsible for calling WdfObjectDelete on the collection
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static NTSTATUS CreateCollectionForMultiStringValue(_In_ WDFKEY wdfKey, _In_ PCUNICODE_STRING valueName, _Out_ WDFCOLLECTION* value)
{
    TRACE_ALWAYS(L"");

    WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;
    ULONG                 sizeNeededInBytes;
    ULONG                 valueType;
    NTSTATUS              ntstatus;

    // Create the collection
    ntstatus = WdfCollectionCreate(NULL, value);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);

    // We encountered some unexpected behavior in the WdfRegistryQueryMultiString method when the registry value is empty
    // To counter it, do the checking ourselfs, and bail out on empty strings to avoid a STATUS_OBJECT_TYPE_MISMATCH
    ntstatus = WdfRegistryQueryValue(wdfKey, valueName, 0, NULL, &sizeNeededInBytes, &valueType);
    if (STATUS_BUFFER_OVERFLOW == ntstatus) ntstatus = STATUS_SUCCESS;
    if (!NT_SUCCESS(ntstatus))
    {
        // Convert the situation where we couldn't find the value into a success condition with an empty set
        if (STATUS_OBJECT_NAME_NOT_FOUND == ntstatus) return (STATUS_PROCESS_NOT_IN_JOB);

        WdfObjectDelete(*value);
        *value = NULL;
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryQueryValue", ntstatus);
    }

    // Bail out on an incorrect type
    if (REG_MULTI_SZ != valueType)
    {
        WdfObjectDelete(*value);
        *value = NULL;
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryQueryValue", STATUS_OBJECT_TYPE_MISMATCH);
    }

    // Bail out when the string is empty (two null-terminators or less) as we have an empty set as result
    if (sizeNeededInBytes <= 4) return (STATUS_PROCESS_NOT_IN_JOB);

    // Query the value
    WDF_OBJECT_ATTRIBUTES_INIT(&wdfObjectAttributes);
    wdfObjectAttributes.ParentObject = *value;
    ntstatus = WdfRegistryQueryMultiString(wdfKey, valueName, &wdfObjectAttributes, *value);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfObjectDelete(*value);
        *value = NULL;
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryQueryMultiString", ntstatus);
    }

    return (STATUS_PROCESS_IN_JOB);
}

_Use_decl_annotations_
NTSTATUS HidHideProfileStoreList(WDFCOLLECTION* profileNames)
{
    TRACE_ALWAYS(L"");

    WDFKEY                wdfParametersKey;
    WDFKEY                wdfProfileStoreKey;
    WDF_OBJECT_ATTRIBUTES wdfObjectAttributes;
    WDFSTRING             nameString;
    HANDLE                profileStoreHandle;
    PKEY_BASIC_INFORMATION basicInformation;
    UNICODE_STRING        name;
    NTSTATUS              ntstatus;

    (*profileNames) = NULL;

    // Create the result collection
    ntstatus = WdfCollectionCreate(NULL, profileNames);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfCollectionCreate", ntstatus);

    // Open (creating when needed) the profile store subkey
    ntstatus = OpenParametersKey(&wdfParametersKey);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfObjectDelete(*profileNames);
        *profileNames = NULL;
        LOG_AND_RETURN_NTSTATUS(L"WdfDriverOpenParametersRegistryKey", ntstatus);
    }
    ntstatus = OpenProfileStoreKey(wdfParametersKey, &wdfProfileStoreKey);
    if (STATUS_OBJECT_NAME_NOT_FOUND == ntstatus)
    {
        // The store hasn't been created yet, which yields an empty set
        WdfRegistryClose(wdfParametersKey);
        return (STATUS_SUCCESS);
    }
    if (!NT_SUCCESS(ntstatus))
    {
        WdfRegistryClose(wdfParametersKey);
        WdfObjectDelete(*profileNames);
        *profileNames = NULL;
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryOpenKey", ntstatus);
    }

    // Prepare a buffer for the enumeration (key name plus a terminator)
    basicInformation = (PKEY_BASIC_INFORMATION)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(KEY_BASIC_INFORMATION) + ((DRIVER_PROFILE_NAME_MAX_LENGTH + 1) * sizeof(WCHAR)), PROFILES_TAG);
    if (NULL == basicInformation)
    {
        WdfRegistryClose(wdfProfileStoreKey);
        WdfRegistryClose(wdfParametersKey);
        WdfObjectDelete(*profileNames);
        *profileNames = NULL;
        LOG_AND_RETURN_NTSTATUS(L"ExAllocatePoolWithTag", STATUS_INSUFFICIENT_RESOURCES);
    }

    // Enumerate all subkeys (each subkey is a profile)
    profileStoreHandle = WdfRegistryWdmGetHandle(wdfProfileStoreKey);
    for (ULONG index = 0; ; index++)
    {
        ULONG resultLength;

        ntstatus = ZwEnumerateKey(profileStoreHandle, index, KeyBasicInformation, basicInformation, sizeof(KEY_BASIC_INFORMATION) + ((DRIVER_PROFILE_NAME_MAX_LENGTH + 1) * sizeof(WCHAR)), &resultLength);
        if (STATUS_NO_MORE_ENTRIES == ntstatus) break;
        if (!NT_SUCCESS(ntstatus)) break;

        // Convert the enumerated name into a string object parented to the result collection
        name.Buffer = basicInformation->Name;
        name.Length = (USHORT)basicInformation->NameLength;
        name.MaximumLength = name.Length;
        WDF_OBJECT_ATTRIBUTES_INIT(&wdfObjectAttributes);
        wdfObjectAttributes.ParentObject = *profileNames;
        ntstatus = WdfStringCreate(&name, &wdfObjectAttributes, &nameString);
        if (!NT_SUCCESS(ntstatus)) break;
    }

    ExFreePoolWithTag(basicInformation, PROFILES_TAG);
    WdfRegistryClose(wdfProfileStoreKey);
    WdfRegistryClose(wdfParametersKey);

    // A failure to enumerate the store (other than the absence of entries) is reported as an error
    if (NT_SUCCESS(ntstatus) || (STATUS_NO_MORE_ENTRIES == ntstatus))
    {
        return (STATUS_SUCCESS);
    }

    WdfObjectDelete(*profileNames);
    *profileNames = NULL;
    LOG_AND_RETURN_NTSTATUS(L"ZwEnumerateKey", ntstatus);
}

_Use_decl_annotations_
NTSTATUS HidHideProfileStoreRead(PCUNICODE_STRING profileName, WDFCOLLECTION* applications, WDFCOLLECTION* blacklistedDeviceInstancePaths)
{
    TRACE_ALWAYS(L"");

    WDFKEY   wdfParametersKey;
    WDFKEY   wdfProfileStoreKey;
    WDFKEY   wdfProfileKey;
    NTSTATUS ntstatus;

    (*applications) = NULL;
    (*blacklistedDeviceInstancePaths) = NULL;

    DECLARE_CONST_UNICODE_STRING(applicationsValue, DRIVER_PROFILE_VALUE_APPLICATIONS);
    DECLARE_CONST_UNICODE_STRING(blacklistedDevicesValue, DRIVER_PROFILE_VALUE_BLACKLISTED_DEVICE_INSTANCE_PATHS);

    // Open (creating when needed) the profile store subkey
    ntstatus = OpenParametersKey(&wdfParametersKey);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfDriverOpenParametersRegistryKey", ntstatus);
    ntstatus = OpenProfileStoreKey(wdfParametersKey, &wdfProfileStoreKey);
    if (STATUS_OBJECT_NAME_NOT_FOUND == ntstatus)
    {
        WdfRegistryClose(wdfParametersKey);
        return (STATUS_PROCESS_NOT_IN_JOB);
    }
    if (!NT_SUCCESS(ntstatus))
    {
        WdfRegistryClose(wdfParametersKey);
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryOpenKey", ntstatus);
    }

    // Open the requested profile subkey (read-only)
    ntstatus = WdfRegistryOpenKey(wdfProfileStoreKey, profileName, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &wdfProfileKey);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfRegistryClose(wdfProfileStoreKey);
        WdfRegistryClose(wdfParametersKey);

        // The absence of the profile is a normal condition, not an error
        if (STATUS_OBJECT_NAME_NOT_FOUND == ntstatus) return (STATUS_PROCESS_NOT_IN_JOB);
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryOpenKey", ntstatus);
    }

    // Read both multi-string values (the absence of either yields an empty collection)
    ntstatus = CreateCollectionForMultiStringValue(wdfProfileKey, &applicationsValue, applications);
    if (NT_SUCCESS(ntstatus) || (STATUS_PROCESS_NOT_IN_JOB == ntstatus))
    {
        ntstatus = CreateCollectionForMultiStringValue(wdfProfileKey, &blacklistedDevicesValue, blacklistedDeviceInstancePaths);
    }
    WdfRegistryClose(wdfProfileKey);
    WdfRegistryClose(wdfProfileStoreKey);
    WdfRegistryClose(wdfParametersKey);

    if (!NT_SUCCESS(ntstatus) && (STATUS_PROCESS_NOT_IN_JOB != ntstatus)) LOG_AND_RETURN_NTSTATUS(L"CreateCollectionForMultiStringValue", ntstatus);

    return (STATUS_PROCESS_IN_JOB);
}

_Use_decl_annotations_
NTSTATUS HidHideProfileStoreWrite(PCUNICODE_STRING profileName, WDFCOLLECTION applications, WDFCOLLECTION blacklistedDeviceInstancePaths)
{
    TRACE_ALWAYS(L"");

    WDFKEY   wdfParametersKey;
    WDFKEY   wdfProfileStoreKey;
    WDFKEY   wdfProfileKey;
    NTSTATUS ntstatus;

    DECLARE_CONST_UNICODE_STRING(applicationsValue, DRIVER_PROFILE_VALUE_APPLICATIONS);
    DECLARE_CONST_UNICODE_STRING(blacklistedDevicesValue, DRIVER_PROFILE_VALUE_BLACKLISTED_DEVICE_INSTANCE_PATHS);

    // Open (creating when needed) the profile store subkey
    ntstatus = OpenParametersKey(&wdfParametersKey);
    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfDriverOpenParametersRegistryKey", ntstatus);
    ntstatus = CreateProfileStoreKey(wdfParametersKey, &wdfProfileStoreKey);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfRegistryClose(wdfParametersKey);
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryCreateKey", ntstatus);
    }

    // Create (or open) the profile subkey
    ntstatus = WdfRegistryCreateKey(wdfProfileStoreKey, profileName, KEY_ALL_ACCESS, REG_OPTION_NON_VOLATILE, WDF_NO_OBJECT_ATTRIBUTES, &wdfProfileKey);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfRegistryClose(wdfProfileStoreKey);
        WdfRegistryClose(wdfParametersKey);
        LOG_AND_RETURN_NTSTATUS(L"WdfRegistryCreateKey", ntstatus);
    }

    // Assign both multi-string values
    ntstatus = WdfRegistryAssignMultiString(wdfProfileKey, &applicationsValue, applications);
    if (!NT_SUCCESS(ntstatus)) WdfRegistryClose(wdfProfileKey);
    if (NT_SUCCESS(ntstatus))
    {
        ntstatus = WdfRegistryAssignMultiString(wdfProfileKey, &blacklistedDevicesValue, blacklistedDeviceInstancePaths);
    }
    WdfRegistryClose(wdfProfileKey);
    WdfRegistryClose(wdfProfileStoreKey);
    WdfRegistryClose(wdfParametersKey);

    if (!NT_SUCCESS(ntstatus)) LOG_AND_RETURN_NTSTATUS(L"WdfRegistryAssignMultiString", ntstatus);

    return (STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID HidHideProfileStoreDelete(PCUNICODE_STRING profileName)
{
    TRACE_ALWAYS(L"");

    WDFKEY   wdfParametersKey;
    WDFKEY   wdfProfileStoreKey;
    WDFKEY   wdfProfileKey;
    NTSTATUS ntstatus;

    // Open the profile store subkey (its absence means nothing to delete)
    ntstatus = OpenParametersKey(&wdfParametersKey);
    if (!NT_SUCCESS(ntstatus)) return;
    ntstatus = OpenProfileStoreKey(wdfParametersKey, &wdfProfileStoreKey);
    if (!NT_SUCCESS(ntstatus))
    {
        WdfRegistryClose(wdfParametersKey);
        return;
    }

    // Open the profile subkey for deletion (the absence of the profile is ignored silently)
    ntstatus = WdfRegistryOpenKey(wdfProfileStoreKey, profileName, KEY_ALL_ACCESS, WDF_NO_OBJECT_ATTRIBUTES, &wdfProfileKey);
    if (NT_SUCCESS(ntstatus))
    {
        ZwDeleteKey(WdfRegistryWdmGetHandle(wdfProfileKey));
        WdfRegistryClose(wdfProfileKey);
    }

    WdfRegistryClose(wdfProfileStoreKey);
    WdfRegistryClose(wdfParametersKey);
}
