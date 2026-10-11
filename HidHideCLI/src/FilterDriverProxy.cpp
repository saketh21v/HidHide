// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// FilterDriverProxy.cpp
#include "stdafx.h"
#include "FilterDriverProxy.h"
#include "HidHideIoctlContract.h"
#include "Utils.h"
#include "Volume.h"
#include "Logging.h"

namespace
{
    typedef std::unique_ptr<std::remove_pointer<HANDLE>::type, decltype(&::CloseHandle)> CloseHandlePtr;

    // Get a file handle to the device driver
    // The flag allowFileNotFound is applied when the device couldn't be found and controls whether or not an exception is thrown on failure
    CloseHandlePtr Device(_In_ std::filesystem::path const& deviceName, _In_ bool allowFileNotFound)
    {
        TRACE_ALWAYS(L"");
        auto handle{ CloseHandlePtr(::CreateFileW(deviceName.native().c_str(), GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), &::CloseHandle) };
        if ((INVALID_HANDLE_VALUE == handle.get()) && ((ERROR_FILE_NOT_FOUND != ::GetLastError()) || (!allowFileNotFound))) THROW_WIN32_LAST_ERROR;
        return (handle);
    }

    // Get a file handle to the device driver; will throw when the device isn't found
    CloseHandlePtr Device(_In_ std::filesystem::path const& deviceName)
    {
        return (Device(deviceName, false));
    }

    // Get the current enabled state; returns true when the device is active in hiding devices on the black-list
    bool GetActive(_In_ HANDLE device)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto buffer{ std::vector<BOOLEAN>(1) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_ACTIVE), nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(BOOLEAN)), &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        if (sizeof(BOOLEAN) != needed) THROW_WIN32(ERROR_INVALID_PARAMETER);
        return (FALSE != buffer.at(0));
    }

    // Set the current enabled state
    void SetActive(_In_ HANDLE device, _In_ bool active)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto buffer{ std::vector<BOOLEAN>(1) };
        buffer.at(0) = (active ? TRUE : FALSE);
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_SET_ACTIVE), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(BOOLEAN)), nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
    }

    // Get the device Instance Paths of the Human Interface Devices that are on the black-list (may reference not present devices)
    HidHide::DeviceInstancePaths GetBlacklist(_In_ HANDLE device)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_BLACKLIST), nullptr, 0, nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        auto buffer{ std::vector<WCHAR>(needed / sizeof(WCHAR)) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_BLACKLIST), nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(WCHAR)), &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        return (HidHide::StringListToStringSet(HidHide::MultiStringToStringList(buffer)));
    }

    // Set the device Instance Paths of the Human Interface Devices that are on the black-list
    void SetBlacklist(_In_ HANDLE device, _In_ HidHide::DeviceInstancePaths const& deviceInstancePaths)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto buffer{ HidHide::StringListToMultiString(HidHide::StringSetToStringList(deviceInstancePaths)) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_SET_BLACKLIST), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(WCHAR)), nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
    }

    // Get the applications on the white-list
    HidHide::FullImageNames GetWhitelist(_In_ HANDLE device)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_WHITELIST), nullptr, 0, nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        auto buffer{ std::vector<WCHAR>(needed / sizeof(WCHAR)) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_WHITELIST), nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(WCHAR)), &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        return (HidHide::StringListToPathSet(HidHide::MultiStringToStringList(buffer)));
    }

    // Set the applications on the white-list
    void SetWhitelist(_In_ HANDLE device, _In_ HidHide::FullImageNames const& fullImageNames)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto buffer{ HidHide::StringListToMultiString(HidHide::PathSetToStringList(fullImageNames)) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_SET_WHITELIST), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(WCHAR)), nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
    }

    // Get the current whitelist inverse state; returns true when the whitelist logic is the inverse (effectively an application backlist)
    bool GetInverse(_In_ HANDLE device)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto buffer{ std::vector<BOOLEAN>(1) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_WLINVERSE), nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(BOOLEAN)), &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        if (sizeof(BOOLEAN) != needed) THROW_WIN32(ERROR_INVALID_PARAMETER);
        return (FALSE != buffer.at(0));
    }

    // Set the current whitelist inverse state
    void SetInverse(_In_ HANDLE device, _In_ bool inverse)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto buffer{ std::vector<BOOLEAN>(1) };
        buffer.at(0) = (inverse ? TRUE : FALSE);
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_SET_WLINVERSE), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(BOOLEAN)), nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
    }

    // The wire-format prefixes used to tag the entries of an application profile
    std::wstring const ProfileApplicationPrefix{ L"A:" };
    std::wstring const ProfileDevicePrefix{ L"D:" };

    // Get the names of the application profiles
    std::vector<std::wstring> GetProfileNames(_In_ HANDLE device)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_PROFILES), nullptr, 0, nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        auto buffer{ std::vector<WCHAR>(needed / sizeof(WCHAR)) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_PROFILES), nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(WCHAR)), &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        return (HidHide::MultiStringToStringList(buffer));
    }

    // Get the raw multi-string entries of a single application profile (the first entry is the profile name)
    std::vector<std::wstring> GetProfileEntries(_In_ HANDLE device, _In_ std::wstring const& name)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto request{ HidHide::StringListToMultiString({ name }) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_PROFILE), request.data(), static_cast<DWORD>(request.size() * sizeof(WCHAR)), nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        auto buffer{ std::vector<WCHAR>(needed / sizeof(WCHAR)) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_PROFILE), request.data(), static_cast<DWORD>(request.size() * sizeof(WCHAR)), buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(WCHAR)), &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        return (HidHide::MultiStringToStringList(buffer));
    }

    // Convert the raw multi-string entries of a profile into an ApplicationProfile
    HidHide::ApplicationProfile ProfileFromEntries(_In_ std::vector<std::wstring> const& entries)
    {
        TRACE_ALWAYS(L"");
        HidHide::ApplicationProfile profile;
        for (size_t index{ 1 }; index < entries.size(); index++)
        {
            auto const& entry{ entries.at(index) };
            if (entry.size() < 2) continue;
            auto const prefix{ entry.substr(0, 2) };
            auto const value{ entry.substr(2) };
            if (_wcsnicmp(prefix.c_str(), ProfileApplicationPrefix.c_str(), 2) == 0) profile.Applications.emplace(value);
            else if (_wcsnicmp(prefix.c_str(), ProfileDevicePrefix.c_str(), 2) == 0) profile.DeviceInstancePaths.emplace(value);
        }
        return (profile);
    }

    // Get all application profiles (keyed by profile name)
    HidHide::ApplicationProfiles GetProfiles(_In_ HANDLE device)
    {
        TRACE_ALWAYS(L"");
        HidHide::ApplicationProfiles profiles;
        for (auto const& name : GetProfileNames(device))
        {
            profiles.emplace(name, ProfileFromEntries(GetProfileEntries(device, name)));
        }
        return (profiles);
    }

    // Create or replace an application profile
    void SetProfile(_In_ HANDLE device, _In_ std::wstring const& name, _In_ HidHide::ApplicationProfile const& profile)
    {
        TRACE_ALWAYS(L"");
        std::vector<std::wstring> entries;
        entries.emplace_back(name);
        for (auto const& application : profile.Applications) entries.emplace_back(ProfileApplicationPrefix + application.wstring());
        for (auto const& deviceInstancePath : profile.DeviceInstancePaths) entries.emplace_back(ProfileDevicePrefix + deviceInstancePath);
        DWORD needed{};
        auto request{ HidHide::StringListToMultiString(entries) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_SET_PROFILE), request.data(), static_cast<DWORD>(request.size() * sizeof(WCHAR)), nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
    }

    // Delete an application profile
    void DeleteProfile(_In_ HANDLE device, _In_ std::wstring const& name)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        auto request{ HidHide::StringListToMultiString({ name }) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_DEL_PROFILE), request.data(), static_cast<DWORD>(request.size() * sizeof(WCHAR)), nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
    }

    // Get the name of the currently active application profile (empty when none)
    std::wstring GetActiveProfileName(_In_ HANDLE device)
    {
        TRACE_ALWAYS(L"");
        DWORD needed{};
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_ACTIVE_PROFILE), nullptr, 0, nullptr, 0, &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        auto buffer{ std::vector<WCHAR>(needed / sizeof(WCHAR)) };
        if (FALSE == ::DeviceIoControl(device, static_cast<DWORD>(IOCTL_GET_ACTIVE_PROFILE), nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(WCHAR)), &needed, nullptr)) THROW_WIN32_LAST_ERROR;
        auto const names{ HidHide::MultiStringToStringList(buffer) };
        return (names.empty() ? std::wstring{} : names.at(0));
    }
}

namespace HidHide
{
    _Use_decl_annotations_
    FilterDriverProxy::FilterDriverProxy(bool writeThrough)
        : m_WriteThrough{ writeThrough }
        , m_Device{ ::Device(HidHide::StringTable(IDS_CONTROL_DEVICE_NAME)) }
        , m_Active{ ::GetActive(m_Device.get()) }
        , m_Blacklist{ ::GetBlacklist(m_Device.get()) }
        , m_Whitelist{ ::GetWhitelist(m_Device.get()) }
        , m_Inverse{ ::GetInverse(m_Device.get()) }
        , m_Profiles{ ::GetProfiles(m_Device.get()) }
    {
        TRACE_ALWAYS(L"");

        if (auto const fullImageName{ HidHide::FileNameToFullImageName(HidHide::ModuleFileName()) }; !fullImageName.empty())
        {
            // Ensure the application itself is always on the whitelist if inverse whitelist is off or always off
            // the whitelist if inverse is on and apply the change immediately
            if ((!m_Inverse && m_Whitelist.emplace(fullImageName).second) || (m_Inverse && m_Whitelist.erase(fullImageName)))
                ::SetWhitelist(m_Device.get(), m_Whitelist);
        }
    }

    DWORD FilterDriverProxy::DeviceStatus()
    {
        TRACE_ALWAYS(L"");
        auto const handle{ CloseHandlePtr(::CreateFileW(HidHide::StringTable(IDS_CONTROL_DEVICE_NAME).c_str(), GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), &::CloseHandle) };
        if ((INVALID_HANDLE_VALUE == handle.get()) && (ERROR_ACCESS_DENIED != ::GetLastError()) && (ERROR_FILE_NOT_FOUND != ::GetLastError())) THROW_WIN32_LAST_ERROR;
        return ((INVALID_HANDLE_VALUE == handle.get()) ? ::GetLastError() : ERROR_SUCCESS);
    }

    void FilterDriverProxy::ApplyConfigurationChanges()
    {
        TRACE_ALWAYS(L"");
        if (m_WriteThrough) THROW_WIN32(ERROR_INVALID_PARAMETER);
        if (::GetWhitelist(m_Device.get()) != m_Whitelist) ::SetWhitelist(m_Device.get(), m_Whitelist);
        if (::GetBlacklist(m_Device.get()) != m_Blacklist) ::SetBlacklist(m_Device.get(), m_Blacklist);
        if (::GetActive(m_Device.get()) != m_Active) ::SetActive(m_Device.get(), m_Active);
        if (::GetInverse(m_Device.get()) != m_Inverse) ::SetInverse(m_Device.get(), m_Inverse);

        // Create or replace the desired profiles and delete the ones no longer present
        auto const profilesOnDevice{ ::GetProfiles(m_Device.get()) };
        for (auto const& profile : m_Profiles)
        {
            auto const it{ profilesOnDevice.find(profile.first) };
            if ((std::end(profilesOnDevice) == it) || (it->second.Applications != profile.second.Applications) || (it->second.DeviceInstancePaths != profile.second.DeviceInstancePaths))
            {
                ::SetProfile(m_Device.get(), profile.first, profile.second);
            }
        }
        for (auto const& profile : profilesOnDevice)
        {
            if (std::end(m_Profiles) == m_Profiles.find(profile.first)) ::DeleteProfile(m_Device.get(), profile.first);
        }
    }

    bool FilterDriverProxy::GetActive() const
    {
        TRACE_ALWAYS(L"");
        return (m_Active);
    }

    _Use_decl_annotations_
    void FilterDriverProxy::SetActive(bool active)
    {
        TRACE_ALWAYS(L"");
        if (m_Active != active)
        {
            m_Active = active;
            if (m_WriteThrough) ::SetActive(m_Device.get(), m_Active);
        }
    }

    DeviceInstancePaths FilterDriverProxy::GetBlacklist() const
    {
        TRACE_ALWAYS(L"");
        return (m_Blacklist);
    }

    _Use_decl_annotations_
    void FilterDriverProxy::SetBlacklist(DeviceInstancePaths const& deviceInstancePaths)
    {
        TRACE_ALWAYS(L"");
        if (m_Blacklist != deviceInstancePaths)
        {
            m_Blacklist = deviceInstancePaths;
            if (m_WriteThrough) ::SetBlacklist(m_Device.get(), m_Blacklist);
        }
    }

    _Use_decl_annotations_
    void FilterDriverProxy::BlacklistAddEntry(DeviceInstancePath const& deviceInstancePath)
    {
        TRACE_ALWAYS(L"");
        if (m_Blacklist.emplace(deviceInstancePath).second)
        {
            if (m_WriteThrough) ::SetBlacklist(m_Device.get(), m_Blacklist);
        }
    }

    _Use_decl_annotations_
    void FilterDriverProxy::BlacklistDelEntry(DeviceInstancePath const& deviceInstancePath)
    {
        TRACE_ALWAYS(L"");
        if (auto const it{ m_Blacklist.find(deviceInstancePath) }; std::end(m_Blacklist) != it)
        {
            m_Blacklist.erase(it);
            if (m_WriteThrough) ::SetBlacklist(m_Device.get(), m_Blacklist);
        }
    }

    FullImageNames FilterDriverProxy::GetWhitelist() const
    {
        TRACE_ALWAYS(L"");
        return (m_Whitelist);
    }

    _Use_decl_annotations_
    void FilterDriverProxy::SetWhitelist(FullImageNames const& fullImageNames)
    {
        TRACE_ALWAYS(L"");
        if (m_Whitelist != fullImageNames)
        {
            m_Whitelist = fullImageNames;
            if (m_WriteThrough) ::SetWhitelist(m_Device.get(), m_Whitelist);
        }
    }

    _Use_decl_annotations_
    void FilterDriverProxy::WhitelistAddEntry(FullImageName const& fullImageName)
    {
        TRACE_ALWAYS(L"");
        if (m_Whitelist.emplace(fullImageName).second)
        {
            if (m_WriteThrough) ::SetWhitelist(m_Device.get(), m_Whitelist);
        }
    }

    _Use_decl_annotations_
    void FilterDriverProxy::WhitelistDelEntry(FullImageName const& fullImageName)
    {
        TRACE_ALWAYS(L"");
        if (auto const it{ m_Whitelist.find(fullImageName) }; std::end(m_Whitelist) != it)
        {
            m_Whitelist.erase(it);
            if (m_WriteThrough) ::SetWhitelist(m_Device.get(), m_Whitelist);
        }
    }

    bool FilterDriverProxy::GetInverse() const
    {
        TRACE_ALWAYS(L"");
        return (m_Inverse);
    }

    _Use_decl_annotations_
    void FilterDriverProxy::SetInverse(bool inverse)
    {
        TRACE_ALWAYS(L"");
        if (m_Inverse != inverse)
        {
            m_Inverse = inverse;
            if (m_WriteThrough) ::SetInverse(m_Device.get(), m_Inverse);
        }
    }

    ApplicationProfiles FilterDriverProxy::GetProfiles() const
    {
        TRACE_ALWAYS(L"");
        return (m_Profiles);
    }

    _Use_decl_annotations_
    bool FilterDriverProxy::GetProfile(std::wstring const& name, ApplicationProfile& profile) const
    {
        TRACE_ALWAYS(L"");
        auto const it{ m_Profiles.find(name) };
        if (std::end(m_Profiles) == it) return (false);
        profile = it->second;
        return (true);
    }

    _Use_decl_annotations_
    void FilterDriverProxy::SetProfile(std::wstring const& name, ApplicationProfile const& profile)
    {
        TRACE_ALWAYS(L"");
        m_Profiles[name] = profile;
        if (m_WriteThrough) ::SetProfile(m_Device.get(), name, profile);
    }

    _Use_decl_annotations_
    void FilterDriverProxy::DeleteProfile(std::wstring const& name)
    {
        TRACE_ALWAYS(L"");
        if ((m_Profiles.erase(name) > 0) && (m_WriteThrough)) ::DeleteProfile(m_Device.get(), name);
    }

    _Use_decl_annotations_
    void FilterDriverProxy::ProfileApplicationsAddEntry(std::wstring const& name, FullImageName const& fullImageName)
    {
        TRACE_ALWAYS(L"");
        if (m_Profiles[name].Applications.emplace(fullImageName).second)
        {
            if (m_WriteThrough) ::SetProfile(m_Device.get(), name, m_Profiles[name]);
        }
    }

    _Use_decl_annotations_
    void FilterDriverProxy::ProfileApplicationsDelEntry(std::wstring const& name, FullImageName const& fullImageName)
    {
        TRACE_ALWAYS(L"");
        auto const it{ m_Profiles.find(name) };
        if ((std::end(m_Profiles) != it) && (it->second.Applications.erase(fullImageName) > 0))
        {
            if (m_WriteThrough) ::SetProfile(m_Device.get(), name, it->second);
        }
    }

    _Use_decl_annotations_
    void FilterDriverProxy::ProfileDeviceInstancePathsAddEntry(std::wstring const& name, DeviceInstancePath const& deviceInstancePath)
    {
        TRACE_ALWAYS(L"");
        if (m_Profiles[name].DeviceInstancePaths.emplace(deviceInstancePath).second)
        {
            if (m_WriteThrough) ::SetProfile(m_Device.get(), name, m_Profiles[name]);
        }
    }

    _Use_decl_annotations_
    void FilterDriverProxy::ProfileDeviceInstancePathsDelEntry(std::wstring const& name, DeviceInstancePath const& deviceInstancePath)
    {
        TRACE_ALWAYS(L"");
        auto const it{ m_Profiles.find(name) };
        if ((std::end(m_Profiles) != it) && (it->second.DeviceInstancePaths.erase(deviceInstancePath) > 0))
        {
            if (m_WriteThrough) ::SetProfile(m_Device.get(), name, it->second);
        }
    }

    std::wstring FilterDriverProxy::ActiveProfile() const
    {
        TRACE_ALWAYS(L"");
        return (::GetActiveProfileName(m_Device.get()));
    }
}