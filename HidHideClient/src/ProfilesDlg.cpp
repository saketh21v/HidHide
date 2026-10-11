// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// ProfilesDlg.cpp
#include "stdafx.h"
#include "ProfilesDlg.h"
#include "HidHideClientDlg.h"
#include "Utils.h"
#include "Volume.h"
#include "Logging.h"

// Define user-message for processing refresh requests
constexpr auto WM_USER_PROFILES_REFRESH{ WM_USER + 2 };

namespace
{
    // The maximum number of characters allowed in a profile name (mirrors the driver's limit)
    constexpr size_t MaxProfileNameLength{ 128 };

    // Is the value provided acceptable as a profile name?
    bool ValidProfileName(_In_ std::wstring const& value)
    {
        if (value.empty() || (value.size() > MaxProfileNameLength)) return (false);
        return (std::wstring::npos == value.find(L'\\'));
    }

    // Capture the list box content as a set of full image names
    HidHide::FullImageNames ListBoxToPathSet(_In_ CListBox const& listBox)
    {
        TRACE_ALWAYS(L"");
        HidHide::FullImageNames result;

        for (int index{}, size(listBox.GetCount()); (index < size); index++)
        {
            CString value;
            listBox.GetText(index, value);
            result.emplace(value.GetBuffer());
        }

        return (result);
    }

    // Capture the list box content as a set of strings
    HidHide::DeviceInstancePaths ListBoxToStringSet(_In_ CListBox const& listBox)
    {
        TRACE_ALWAYS(L"");
        HidHide::DeviceInstancePaths result;

        for (int index{}, size(listBox.GetCount()); (index < size); index++)
        {
            CString value;
            listBox.GetText(index, value);
            result.emplace(value.GetBuffer());
        }

        return (result);
    }
}

/////////////////////////////////////////////////////////////////////////////
// CInputDlg

IMPLEMENT_DYNAMIC(CInputDlg, CDialogEx)

_Use_decl_annotations_
CInputDlg::CInputDlg(std::wstring const& title, std::wstring const& guidance, std::wstring const& value, std::function<bool(std::wstring const&)> validator, std::wstring const& errorMessage, CWnd* pParent)
    : CDialogEx(IDD_DIALOG_PROFILE_NAME, pParent)
    , m_Title{ title }
    , m_Guidance{ guidance }
    , m_Value{ value }
    , m_Validator{ std::move(validator) }
    , m_ErrorMessage{ errorMessage }
    , m_GuidanceControl{}
    , m_Edit{}
{
    TRACE_ALWAYS(L"");
}

std::wstring const& CInputDlg::Value() const noexcept
{
    return (m_Value);
}

_Use_decl_annotations_
void CInputDlg::DoDataExchange(CDataExchange* pDX)
{
    TRACE_ALWAYS(L"");
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_STATIC_PROFILE_NAME_GUIDANCE, m_GuidanceControl);
    DDX_Control(pDX, IDC_EDIT_PROFILE_NAME,             m_Edit);
}

BOOL CInputDlg::OnInitDialog()
{
    TRACE_ALWAYS(L"");
    CDialogEx::OnInitDialog();

    // Apply the title, guidance text and initial value
    SetWindowTextW(m_Title.c_str());
    m_GuidanceControl.SetWindowTextW(m_Guidance.c_str());
    m_Edit.SetWindowTextW(m_Value.c_str());

    return (TRUE);
}

void CInputDlg::OnOK()
{
    TRACE_ALWAYS(L"");

    CString value;
    m_Edit.GetWindowText(value);
    std::wstring const entered{ value.GetString() };

    // Reject the value when the validator (if any) doesn't accept it
    if ((nullptr != m_Validator) && (!m_Validator(entered)))
    {
        AfxMessageBox(m_ErrorMessage.c_str(), MB_ICONWARNING);
        return;
    }

    m_Value = entered;
    CDialogEx::OnOK();
}

/////////////////////////////////////////////////////////////////////////////
// CProfilesDlg

IMPLEMENT_DYNAMIC(CProfilesDlg, CDialogEx)

#pragma warning(push)
#pragma warning(disable: 26454 28213) // Warnings caused by Microsoft MFC macros
BEGIN_MESSAGE_MAP(CProfilesDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BUTTON_PROFILE_INSERT,              &CProfilesDlg::OnBnClickedButtonProfileInsert)
    ON_BN_CLICKED(IDC_BUTTON_PROFILE_DELETE,              &CProfilesDlg::OnBnClickedButtonProfileDelete)
    ON_LBN_SELCHANGE(IDC_LIST_PROFILES,                   &CProfilesDlg::OnLbnSelchangeListProfiles)
    ON_BN_CLICKED(IDC_BUTTON_PROFILE_APPLICATIONS_INSERT, &CProfilesDlg::OnBnClickedButtonApplicationsInsert)
    ON_BN_CLICKED(IDC_BUTTON_PROFILE_APPLICATIONS_DELETE, &CProfilesDlg::OnBnClickedButtonApplicationsDelete)
    ON_BN_CLICKED(IDC_BUTTON_PROFILE_DEVICES_INSERT,      &CProfilesDlg::OnBnClickedButtonDevicesInsert)
    ON_BN_CLICKED(IDC_BUTTON_PROFILE_DEVICES_DELETE,      &CProfilesDlg::OnBnClickedButtonDevicesDelete)
    ON_MESSAGE(WM_USER_PROFILES_REFRESH, &CProfilesDlg::OnUserMessageRefresh)
    ON_WM_SHOWWINDOW()
END_MESSAGE_MAP()
#pragma warning(pop)

_Use_decl_annotations_
CProfilesDlg::CProfilesDlg(CHidHideClientDlg& hidHideClientDlg, CWnd* pParent)
    : CDialogEx(IDD_DIALOG_PROFILES, pParent)
    , HidHide::IDropTarget()
    , m_HidHideClientDlg{ hidHideClientDlg }
    , m_DropTargetFullImageNames{}
    , m_Profiles{}
    , m_ProfileInsert{}
    , m_ProfileDelete{}
    , m_ApplicationsLabel{}
    , m_Applications{}
    , m_ApplicationsInsert{}
    , m_ApplicationsDelete{}
    , m_DevicesLabel{}
    , m_Devices{}
    , m_DevicesInsert{}
    , m_DevicesDelete{}
    , m_Guidance{}
{
    TRACE_ALWAYS(L"");
}

CProfilesDlg::~CProfilesDlg()
{
    TRACE_ALWAYS(L"");
}

HidHide::FilterDriverProxy& CProfilesDlg::FilterDriverProxy() noexcept
{
    return (m_HidHideClientDlg.FilterDriverProxy());
}

_Use_decl_annotations_
void CProfilesDlg::DoDataExchange(CDataExchange* pDX)
{
    TRACE_ALWAYS(L"");
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_PROFILES,                   m_Profiles);
    DDX_Control(pDX, IDC_BUTTON_PROFILE_INSERT,           m_ProfileInsert);
    DDX_Control(pDX, IDC_BUTTON_PROFILE_DELETE,           m_ProfileDelete);
    DDX_Control(pDX, IDC_STATIC_PROFILE_APPLICATIONS,     m_ApplicationsLabel);
    DDX_Control(pDX, IDC_LIST_PROFILE_APPLICATIONS,       m_Applications);
    DDX_Control(pDX, IDC_BUTTON_PROFILE_APPLICATIONS_INSERT, m_ApplicationsInsert);
    DDX_Control(pDX, IDC_BUTTON_PROFILE_APPLICATIONS_DELETE, m_ApplicationsDelete);
    DDX_Control(pDX, IDC_STATIC_PROFILE_DEVICES,          m_DevicesLabel);
    DDX_Control(pDX, IDC_LIST_PROFILE_DEVICES,            m_Devices);
    DDX_Control(pDX, IDC_BUTTON_PROFILE_DEVICES_INSERT,   m_DevicesInsert);
    DDX_Control(pDX, IDC_BUTTON_PROFILE_DEVICES_DELETE,   m_DevicesDelete);
    DDX_Control(pDX, IDC_STATIC_PROFILES_GUIDANCE,        m_Guidance);
}

BOOL CProfilesDlg::OnInitDialog()
{
    TRACE_ALWAYS(L"");
    CDialogEx::OnInitDialog();

    // Apply the labels from the string table
    m_Guidance.SetWindowTextW(HidHide::StringTable(IDS_STATIC_PROFILES_GUIDANCE).c_str());
    m_ApplicationsLabel.SetWindowTextW(HidHide::StringTable(IDS_STATIC_PROFILE_APPLICATIONS).c_str());
    m_DevicesLabel.SetWindowTextW(HidHide::StringTable(IDS_STATIC_PROFILE_DEVICES).c_str());
    m_ProfileInsert.SetWindowTextW(HidHide::StringTable(IDS_BUTTON_PROFILE_INSERT).c_str());
    m_ProfileDelete.SetWindowTextW(HidHide::StringTable(IDS_BUTTON_PROFILE_DELETE).c_str());
    m_ApplicationsInsert.SetWindowTextW(HidHide::StringTable(IDS_BUTTON_PROFILE_APPLICATIONS_INSERT).c_str());
    m_ApplicationsDelete.SetWindowTextW(HidHide::StringTable(IDS_BUTTON_PROFILE_APPLICATIONS_DELETE).c_str());
    m_DevicesInsert.SetWindowTextW(HidHide::StringTable(IDS_BUTTON_PROFILE_DEVICES_INSERT).c_str());
    m_DevicesDelete.SetWindowTextW(HidHide::StringTable(IDS_BUTTON_PROFILE_DEVICES_DELETE).c_str());

    return (TRUE);
}

_Use_decl_annotations_
void CProfilesDlg::OnShowWindow(BOOL bShow, UINT nStatus)
{
    TRACE_ALWAYS(L"");
    CDialogEx::OnShowWindow(bShow, nStatus);
    Refresh();
}

void CProfilesDlg::Refresh()
{
    TRACE_ALWAYS(L"");
    PostMessageW(WM_USER_PROFILES_REFRESH, 0, NULL);
}

_Use_decl_annotations_
LRESULT CProfilesDlg::OnUserMessageRefresh(WPARAM wParam, LPARAM lParam)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    // Remember the current selection so it can be restored
    auto const previous{ SelectedProfile() };

    m_Profiles.ResetContent();
    for (auto const& profile : FilterDriverProxy().GetProfiles())
    {
        auto const index{ m_Profiles.AddString(profile.first.c_str()) };
        if ((LB_ERR == index) || (LB_ERRSPACE == index)) THROW_WIN32(ERROR_INVALID_PARAMETER);
        if (profile.first == previous) m_Profiles.SetCurSel(index);
    }

    RefreshDetail();
    return (0);
}

std::wstring CProfilesDlg::SelectedProfile() const
{
    TRACE_PERFORMANCE(L"");

    auto const selection{ m_Profiles.GetCurSel() };
    if (LB_ERR == selection) return (std::wstring{});

    CString value;
    m_Profiles.GetText(selection, value);
    return (value.GetString());
}

void CProfilesDlg::RefreshDetail()
{
    TRACE_ALWAYS(L"");

    m_Applications.ResetContent();
    m_Devices.ResetContent();

    HidHide::ApplicationProfile profile;
    if (!FilterDriverProxy().GetProfile(SelectedProfile(), profile)) return;

    for (auto const& application : profile.Applications)
    {
        if (LB_ERR == m_Applications.AddString(application.c_str())) THROW_WIN32(ERROR_INVALID_PARAMETER);
    }
    for (auto const& deviceInstancePath : profile.DeviceInstancePaths)
    {
        if (LB_ERR == m_Devices.AddString(deviceInstancePath.c_str())) THROW_WIN32(ERROR_INVALID_PARAMETER);
    }
}

void CProfilesDlg::StoreSelectedProfileFromLists()
{
    TRACE_ALWAYS(L"");

    auto const name{ SelectedProfile() };
    if (name.empty()) return;

    HidHide::ApplicationProfile profile;
    profile.Applications = ListBoxToPathSet(m_Applications);
    profile.DeviceInstancePaths = ListBoxToStringSet(m_Devices);
    FilterDriverProxy().SetProfile(name, profile);
    Refresh();
}

void CProfilesDlg::OnBnClickedButtonProfileInsert()
{
    TRACE_ALWAYS(L"");

    CInputDlg dlg(HidHide::StringTable(IDS_DIALOG_PROFILE_NAME), HidHide::StringTable(IDS_STATIC_PROFILE_NAME_GUIDANCE), L"",
        [](std::wstring const& value) { return (ValidProfileName(value)); },
        HidHide::StringTable(IDS_PROFILE_NAME_INVALID), this);
    if (IDOK != dlg.DoModal()) return;

    // Avoid clobbering an existing profile
    HidHide::ApplicationProfile existing;
    if (FilterDriverProxy().GetProfile(dlg.Value(), existing)) return;

    FilterDriverProxy().SetProfile(dlg.Value(), HidHide::ApplicationProfile{});
    Refresh();
}

void CProfilesDlg::OnBnClickedButtonProfileDelete()
{
    TRACE_ALWAYS(L"");

    auto const name{ SelectedProfile() };
    if (name.empty()) return;

    FilterDriverProxy().DeleteProfile(name);
    Refresh();
}

void CProfilesDlg::OnLbnSelchangeListProfiles()
{
    TRACE_ALWAYS(L"");
    RefreshDetail();
}

void CProfilesDlg::OnBnClickedButtonApplicationsInsert()
{
    TRACE_ALWAYS(L"");

    if (SelectedProfile().empty()) return;

    // Ask the user to pin-point a specific file
    CFileDialog fileDlg(TRUE, L"exe", nullptr, (OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST), HidHide::StringTable(IDS_STATIC_FILE_OPEN_FILTER).c_str());
    auto const title{ HidHide::StringTable(IDS_DIALOG_FILE_OPEN) };
    fileDlg.m_pOFN->lpstrTitle = title.c_str();
    if (IDOK != fileDlg.DoModal()) return;

    // Convert the file name into a full image name and add it unless already present
    auto const fullImageName{ HidHide::FileNameToFullImageName(fileDlg.GetPathName().GetString()) };
    if (fullImageName.empty()) return;
    if (LB_ERR == m_Applications.AddString(fullImageName.c_str())) THROW_WIN32(ERROR_INVALID_PARAMETER);

    StoreSelectedProfileFromLists();
}

void CProfilesDlg::OnBnClickedButtonApplicationsDelete()
{
    TRACE_ALWAYS(L"");

    // Get the array of selected items
    auto const size{ m_Applications.GetSelCount() };
    CArray<int, int> itemsSelected;
    itemsSelected.SetSize(size);
    if (LB_ERR == m_Applications.GetSelItems(size, itemsSelected.GetData())) THROW_WIN32(ERROR_INVALID_PARAMETER);

    for (int index{ size }; (index > 0);)
    {
        index--;
        if (LB_ERR == m_Applications.DeleteString(itemsSelected[index])) THROW_WIN32(ERROR_INVALID_PARAMETER);
    }

    StoreSelectedProfileFromLists();
}

void CProfilesDlg::OnBnClickedButtonDevicesInsert()
{
    TRACE_ALWAYS(L"");

    if (SelectedProfile().empty()) return;

    CInputDlg dlg(HidHide::StringTable(IDS_DIALOG_DEVICE_NAME), HidHide::StringTable(IDS_STATIC_DEVICE_NAME_GUIDANCE), L"",
        [](std::wstring const& value) { return (!value.empty() && (value.size() <= MAX_DEVICE_ID_LEN)); },
        HidHide::StringTable(IDS_VALUE_INVALID), this);
    if (IDOK != dlg.DoModal()) return;

    if (LB_ERR == m_Devices.AddString(dlg.Value().c_str())) THROW_WIN32(ERROR_INVALID_PARAMETER);

    StoreSelectedProfileFromLists();
}

void CProfilesDlg::OnBnClickedButtonDevicesDelete()
{
    TRACE_ALWAYS(L"");

    // Get the array of selected items
    auto const size{ m_Devices.GetSelCount() };
    CArray<int, int> itemsSelected;
    itemsSelected.SetSize(size);
    if (LB_ERR == m_Devices.GetSelItems(size, itemsSelected.GetData())) THROW_WIN32(ERROR_INVALID_PARAMETER);

    for (int index{ size }; (index > 0);)
    {
        index--;
        if (LB_ERR == m_Devices.DeleteString(itemsSelected[index])) THROW_WIN32(ERROR_INVALID_PARAMETER);
    }

    StoreSelectedProfileFromLists();
}

_Use_decl_annotations_
DROPEFFECT CProfilesDlg::OnDragEnter(CWnd* pWnd, COleDataObject* pDataObject, DWORD dwKeyState, CPoint point)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(pWnd);
    UNREFERENCED_PARAMETER(point);

    // Flush the previous list
    m_DropTargetFullImageNames.clear();

    // Only accept one or more application files (.exe, .com, .bin) located on a volume
    for (auto const& fullyQualifiedFileName : HidHide::DragTargetFileNames(pDataObject))
    {
        if (HidHide::FileIsAnApplication(fullyQualifiedFileName))
        {
            if (auto const fullImageName{ HidHide::FileNameToFullImageName(fullyQualifiedFileName) }; !fullImageName.empty())
            {
                m_DropTargetFullImageNames.emplace(fullImageName);
                continue;
            }
        }

        m_DropTargetFullImageNames.clear();
        break;
    }

    return ((HidHide::DragTargetCopyOperation(dwKeyState) && (!m_DropTargetFullImageNames.empty())) ? DROPEFFECT_COPY : DROPEFFECT_NONE);
}

_Use_decl_annotations_
DROPEFFECT CProfilesDlg::OnDragOver(CWnd* pWnd, COleDataObject* pDataObject, DWORD dwKeyState, CPoint point)
{
    TRACE_PERFORMANCE(L"");
    UNREFERENCED_PARAMETER(pWnd);
    UNREFERENCED_PARAMETER(pDataObject);

    return ((HidHide::DragTargetCopyOperation(dwKeyState) && (!m_DropTargetFullImageNames.empty()) && (MousePointerAtApplications(point))) ? DROPEFFECT_COPY : DROPEFFECT_NONE);
}

_Use_decl_annotations_
DROPEFFECT CProfilesDlg::OnDropEx(CWnd* pWnd, COleDataObject* pDataObject, DROPEFFECT dropDefault, DROPEFFECT dropList, CPoint point)
{
    TRACE_ALWAYS(L"");
    UNREFERENCED_PARAMETER(pWnd);
    UNREFERENCED_PARAMETER(pDataObject);
    UNREFERENCED_PARAMETER(dropDefault);
    UNREFERENCED_PARAMETER(dropList);

    if ((m_DropTargetFullImageNames.empty()) || (SelectedProfile().empty()) || (!MousePointerAtApplications(point))) return (DROPEFFECT_NONE);

    for (auto const& fullImageName : m_DropTargetFullImageNames)
    {
        if (LB_ERR == m_Applications.AddString(fullImageName.c_str())) THROW_WIN32(ERROR_INVALID_PARAMETER);
    }

    StoreSelectedProfileFromLists();
    return (DROPEFFECT_COPY);
}

_Use_decl_annotations_
bool CProfilesDlg::MousePointerAtApplications(CPoint point)
{
    TRACE_PERFORMANCE(L"");

    CRect rect{};
    GetDlgItem(IDC_LIST_PROFILE_APPLICATIONS)->GetWindowRect(&rect);
    GetParent()->ScreenToClient(&rect);
    return (FALSE != rect.PtInRect(point));
}
